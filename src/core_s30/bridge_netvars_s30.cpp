//=============================================================================//
//
// Purpose: the S30 client's script netvar table, sent to the dedi by name.
//
// S30 RegisterNetworkedVariable (worker 0xB80210, this = the VM) records each var
// in a 350 x 0x30 hash table (0x85CCE40): {name hash +0, category +8, type +0xC,
// slot +0x10, second slot +0x14, min +0x18, max +0x1C}, keyed by the hash of the
// name's string object (+0x18 of it; the characters at +0x24). The dedi keys its
// own vars by another hash, so the worker's name is kept and the table goes to the
// dedi by name (C2S 'NVT1', dedi sndc_s30_remap), which then sends its netvar
// arrays in this table's slots.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "thirdparty/detours/include/detours.h"
#include "bridge_netvars_s30.h"
#include "bridge_bits_s30.h"
#include "bridge_netchan_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternRegister =
	"48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 50 8B 05 ?? ?? ?? ?? 4C 8B F9 "
	"0F 29 74 24 40 0F 29 7C 24 30 83 F8 01";
// movabs r10, <hash divisor> / lea r11, [table] in the worker's lookup.
static const char* const s_pszPatternTable = "49 BA 03 62 53 69 C8 B9 CF 2E 4C 8D 1D ?? ?? ?? ??";
static const int kTableEntries = 350;
static const int kEntrySize = 0x30;
static const uint32_t kMagic = 0x3154564Eu; // 'NVT1'
static const int kFramePayload = 900;

typedef int64_t (*Register_t)(uint8_t* pVM);
static Register_t v_Register = nullptr;
static const uint8_t* s_pTable = nullptr;

static SRWLOCK s_lock = SRWLOCK_INIT;
static std::unordered_map<uint64_t, std::string> s_names; // name hash -> name

// The VM's first argument as a string object: [hash at +0x18, characters at +0x24].
static bool NetVars_ReadName(uint8_t* pVM, uint64_t& nHash, char* pszName, size_t nName)
{
	__try
	{
		const uint8_t* const pStack = *reinterpret_cast<uint8_t* const*>(pVM + 0x40);
		const uint64_t nObject = *reinterpret_cast<const uint64_t*>(pStack + 8);
		if ((nObject >> 0x3B) != 0xA) // string
			return false;
		const uint8_t* const pString = reinterpret_cast<const uint8_t*>((nObject & 0x1FFFFFFFFFFFull) * 8);
		nHash = *reinterpret_cast<const uint64_t*>(pString + 0x18);
		strncpy_s(pszName, nName, reinterpret_cast<const char*>(pString + 0x24), _TRUNCATE);
		return nHash != 0 && pszName[0];
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

static int64_t h_Register(uint8_t* pVM)
{
	uint64_t nHash = 0;
	char szName[128];
	const bool bName = NetVars_ReadName(pVM, nHash, szName, sizeof(szName));
	const int64_t nResult = v_Register(pVM);
	if (bName)
	{
		AcquireSRWLockExclusive(&s_lock);
		s_names[nHash] = szName;
		ReleaseSRWLockExclusive(&s_lock);
	}
	return nResult;
}

bool S30NetVars_Install(void)
{
	int nMatches = 0;
	uint8_t* const pTableRef = S30Pattern_FindUnique(s_pszPatternTable, &nMatches);
	v_Register = reinterpret_cast<Register_t>(S30Pattern_FindUnique(s_pszPatternRegister, &nMatches));
	if (!pTableRef || !v_Register)
	{
		S30Log("[NETVARS] registration worker or table unresolved -- script netvars stay unmapped\n");
		return false;
	}
	s_pTable = pTableRef + 17 + *reinterpret_cast<const int32_t*>(pTableRef + 13);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_Register), h_Register);
	const LONG nError = DetourTransactionCommit();
	S30Log("[NETVARS] RegisterNetworkedVariable worker %s, table 0x%llX\n", nError == NO_ERROR ? "hooked" : "FAILED",
		static_cast<unsigned long long>(S30Pattern_Rva(s_pTable)));
	return nError == NO_ERROR;
}

//-----------------------------------------------------------------------------
// Sending
//-----------------------------------------------------------------------------
struct NetVar_t
{
	std::string name;
	uint8_t nCategory, nType, nSlot, nSlot2;
	float flMin, flMax;
};

static std::vector<NetVar_t> s_pending;  // being sent
static size_t s_nNextToSend = 0;
static size_t s_nLastSentCount = 0;
static uint64_t s_nLastSentSum = 0;
static size_t s_nSeenCount = 0;
static uint64_t s_nSeenSum = 0;
static ULONGLONG s_nSeenSinceMs = 0;

void S30NetVars_Reset(void)
{
	s_pending.clear();
	s_nNextToSend = 0;
	s_nLastSentCount = 0;
	s_nLastSentSum = 0;
	s_nSeenCount = 0;
	s_nSeenSum = 0;
	s_nSeenSinceMs = 0;
}

static void NetVars_Collect(std::vector<NetVar_t>& out, size_t& nUnnamed)
{
	nUnnamed = 0;
	AcquireSRWLockShared(&s_lock);
	for (int i = 0; i < kTableEntries; ++i)
	{
		const uint8_t* const e = s_pTable + i * kEntrySize;
		const uint64_t nHash = *reinterpret_cast<const uint64_t*>(e);
		if (!nHash)
			continue;
		const auto it = s_names.find(nHash);
		if (it == s_names.end())
		{
			++nUnnamed;
			continue;
		}
		NetVar_t v;
		v.name = it->second;
		v.nCategory = static_cast<uint8_t>(*reinterpret_cast<const int*>(e + 8));
		v.nType = static_cast<uint8_t>(*reinterpret_cast<const int*>(e + 0xC));
		v.nSlot = static_cast<uint8_t>(*reinterpret_cast<const int*>(e + 0x10));
		v.nSlot2 = static_cast<uint8_t>(*reinterpret_cast<const int*>(e + 0x14));
		memcpy(&v.flMin, e + 0x18, 4);
		memcpy(&v.flMax, e + 0x1C, 4);
		out.push_back(std::move(v));
	}
	ReleaseSRWLockShared(&s_lock);
}

// One frame: [u32 'NVT1'][u16 total][u16 first][u8 n] then n entries.
static bool NetVars_SendFrame(void)
{
	const size_t nTotal = s_pending.size();
	uint8_t frame[kFramePayload + 64];
	uint32_t off = 0;
	auto put = [&](const void* p, uint32_t n)
	{
		memcpy(frame + off, p, n);
		off += n;
	};
	const uint16_t nTotal16 = static_cast<uint16_t>(nTotal), nFirst = static_cast<uint16_t>(s_nNextToSend);
	uint8_t nCount = 0;
	put(&kMagic, 4);
	put(&nTotal16, 2);
	put(&nFirst, 2);
	const uint32_t nCountAt = off;
	put(&nCount, 1);
	size_t i = s_nNextToSend;
	for (; i < nTotal && nCount < 255; ++i)
	{
		const NetVar_t& v = s_pending[i];
		const uint8_t nLen = static_cast<uint8_t>(v.name.size() > 120 ? 120 : v.name.size());
		if (off + 13 + nLen > kFramePayload)
			break;
		put(&v.nCategory, 1);
		put(&v.nType, 1);
		put(&v.nSlot, 1);
		put(&v.nSlot2, 1);
		put(&v.flMin, 4);
		put(&v.flMax, 4);
		put(&nLen, 1);
		put(v.name.data(), nLen);
		++nCount;
	}
	frame[nCountAt] = nCount;
	// S3 net_ScriptMessage (68), untyped: [1 0][16 byte length][frame]
	uint8_t msg[kFramePayload + 96];
	S30BitWriter_t w(msg, sizeof(msg));
	w.Write(68, 7);
	w.Write(0, 1);
	w.Write(off, 16);
	for (uint32_t b = 0; b < off; ++b)
		w.Write(frame[b], 8);
	if (w.m_bOverflow || !S30Netchan_QueueC2S(msg, w.m_nPos, 2, 100))
		return false; // the C2S ring is full: next snapshot
	s_nNextToSend = i;
	return true;
}

void S30NetVars_OnSnapshot(void)
{
	if (!s_pTable)
		return;
	if (!s_pending.empty())
	{
		// A few frames per snapshot keep the C2S ring free for the game's own messages.
		for (int n = 0; n < 2 && s_nNextToSend < s_pending.size(); ++n)
			if (!NetVars_SendFrame())
				return;
		if (s_nNextToSend >= s_pending.size())
		{
			S30Log("[NETVARS] sent %zu script netvars to the dedi\n", s_pending.size());
			s_pending.clear();
			s_nNextToSend = 0;
		}
		return;
	}
	// Registration settles during the level load: send once the table has not
	// changed for a second, and again whenever it changes.
	std::vector<NetVar_t> vars;
	size_t nUnnamed = 0;
	NetVars_Collect(vars, nUnnamed);
	uint64_t nSum = 0;
	for (const NetVar_t& v : vars)
		nSum = nSum * 31 + std::hash<std::string>()(v.name) + v.nSlot;
	const ULONGLONG nNow = GetTickCount64();
	if (vars.size() != s_nSeenCount || nSum != s_nSeenSum)
	{
		s_nSeenCount = vars.size();
		s_nSeenSum = nSum;
		s_nSeenSinceMs = nNow;
		return;
	}
	if (vars.empty() || nNow - s_nSeenSinceMs < 1000 || (vars.size() == s_nLastSentCount && nSum == s_nLastSentSum))
		return;
	if (nUnnamed)
		S30Log("[NETVARS] %zu registered netvars without a captured name -- not sent\n", nUnnamed);
	s_nLastSentCount = vars.size();
	s_nLastSentSum = nSum;
	s_pending = std::move(vars);
	s_nNextToSend = 0;
}
