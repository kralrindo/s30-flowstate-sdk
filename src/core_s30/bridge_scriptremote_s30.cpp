//=============================================================================//
//
// Purpose: the dedi's server-to-client script remote calls on the S30 VMs.
//
// S30.1 facts (s30/docs/12, verified in the S30 IDB):
//   0xF594F0  CSquirrelVM::FindFunction(vm, name, signature, scope): a 16-byte
//             function handle (the engine frees it with 0xF65650 + 0x127DFD0).
//   0xF597E0  CSquirrelVM::ExecuteFunction(vm, hFunc, args, nArgs, ret, scope)
//             (callers: 0x66AF15 GetBleedoutEndTime, 0x2C6AE9).
//   ScriptVariant_t: 12-byte value, int16 flags +12, int16 type +14; S30 types
//             float 1, vector 3, int 5, bool 6, cstring 0x24, hscript 0x25
//             (S21: cstring 0x22, hscript 0x23 -- two field types were added).
//   CLIENT VM: the CSquirrelVM* the engine passes with "ClientCodeCallback_MapSpawn"
//             (0x4073D28); UI VM: with "UICodeCallback_RunHeadless" (0x400A490).
//   0xAC5150  C_BaseEntity::GetScriptInstance.
//   0xC0C5D0  GetEntityFromEncodedEHandle: handle table 0x58D4A18, 32-byte slots
//             {entity, serial at +8}; scripts only take heavyweight entities (+0xC0).
//   0xAD32B0  stores one Remote_RegisterClientFunction / Remote_RegisterUIFunction
//             registration (only caller 0xAD4864 in their body 0xAD4640):
//             (this, hFunc, name, flags 2*bUI+1, schema, schema bytes, params);
//             0 when stored. The schema (built by 0xAD3550) is per param a type
//             byte: float 1 / vector 3 + min, max, bits; int 5 + min, max; bool 6;
//             entity 0x2A; typed_entity 0x2B + class name; itemflavor 0x2C + kind
//             byte + 1 or 2 ints. S30 has no string param.
//   FindFunction skips the signature check for a null signature (0xF68A73); the
//   engine's ExecuteFunction calls pass a zeroed return variant and a null scope.
// The dedi's entity handles are 16:16 (index, serial), as the S21 bridge reads them.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "thirdparty/detours/include/detours.h"
#include "bridge_scriptremote_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"
#include "bridge_bits_s30.h"
#include "bridge_netchan_s30.h"

static const char* const s_pszPatternFindFunction =
	"48 89 5C 24 10 4C 89 4C 24 20 57 48 83 EC 30 48 8B 79 30 4D 8B C8 48 8B CF 4C 8D 44 24 58 48 8B DA E8";
static const char* const s_pszPatternExecuteFunction =
	"40 56 48 83 EC 30 48 89 5C 24 40 48 8B D9 48 89 6C 24 48 B1 0C 48 89 7C 24 50 48 8B EA 4C 89 6C 24 58 49 8B F8";
static const char* const s_pszPatternGetScriptInstance =
	"48 89 5C 24 08 57 48 83 EC 20 80 B9 C0 00 00 00 00 48 8B F9 75 ?? 48 8B 01 FF 90 A0 04 00 00 48 8B CF 48 8B D8 E8";
static const char* const s_pszPatternStoreRemote =
	"4C 8B DC 49 89 5B 08 49 89 73 10 57 48 81 EC 70 01 00 00 0F B7 05";
// 0xAD2FA0 stores a Remote_RegisterServerFunction registration (via 0xAD4140): (this,
// name, flags, schema, schema bytes, params), 0 when stored; the index it gets is the
// u16 counter read at +0x116 (0x24DDB68), bumped only on success (docs/15).
static const char* const s_pszPatternStoreServer =
	"40 55 56 57 48 81 EC 70 01 00 00 49 8B E9 41 8B F0 48 8B FA E8";
static const int s_nServerCountRead = 0x116; // movzx eax, word ptr [rip+counter]
static const char* const s_pszPatternEntityFromEHandle =
	"40 57 48 83 EC 20 8B C1 C1 F8 0E 3D 00 00 01 00 73 ?? 81 F9 FF FF FF 00 74 ?? 8D 04 8D 00 00 00 00 81 E1 FF 3F 00 00 "
	"25 00 00 FF FF 0B C1 48 8D 0D";
static const int s_nEntityTableLea = 0x2E; // lea rcx, [rip+table] in the pattern above

static const uint32_t kFrameMagic = 0x53523244u; // 'SR2D', the dedi SDK's call frame
static const uint32_t kMantleBoostMagic = 0x3156424Du; // 'MBV1', BRIDGE_S2C_MANTLEBOOST_MAGIC
static const uint32_t kPortalMagic = 0x504F5254u; // 'PORT', the dev portal teleport frame
static const int kMaxArgs = 16;
static const int kMaxEntities = 0x4000;
static const int ENT_HEAVYWEIGHT = 0xC0;

// ScriptVariant_t types (S30.1)
static const int16_t SV_FLOAT = 1, SV_VECTOR = 3, SV_INT = 5, SV_BOOL = 6, SV_CSTRING = 0x24, SV_HSCRIPT = 0x25;

#pragma pack(push, 2)
struct ScriptVariant_t
{
	union
	{
		int nInt;
		float flFloat;
		float vec[3];
		bool bBool;
		const char* pszString;
		void* hScript;
	};
	int16_t nFlags;
	int16_t nType;
};
#pragma pack(pop)
static_assert(sizeof(ScriptVariant_t) == 16, "ScriptVariant_t is 16 bytes");

typedef void* (*FindFunction_t)(void* pVM, const char* pszName, const char* pszSignature, void* hScope);
typedef int (*ExecuteFunction_t)(void* pVM, void* hFunc, ScriptVariant_t* pArgs, unsigned int nArgs, ScriptVariant_t* pReturn, void* hScope);
typedef void* (*GetScriptInstance_t)(void* pEntity);
typedef int (*StoreRemote_t)(void* pThis, void* hFunc, const char* pszName, int nFlags, const uint8_t* pSchema, int nSchemaBytes, int nParams);

static FindFunction_t v_FindFunction = nullptr;
static ExecuteFunction_t v_ExecuteFunction = nullptr;
static GetScriptInstance_t v_GetScriptInstance = nullptr;
static const uint8_t* s_pEntityTable = nullptr;
static void** s_ppClientVM = nullptr;
static void** s_ppUIVM = nullptr;
static StoreRemote_t v_StoreRemote = nullptr;
typedef int (*StoreServer_t)(void* pThis, const char* pszName, int nFlags, const uint8_t* pSchema, int nSchemaBytes, int nParams);
static StoreServer_t v_StoreServer = nullptr;
static const uint32_t kC2SFrameMagic = 0x53523243u; // "C2RS" in memory: the dedi's name-carried client call (docs/15)

// S30 registration param types (0xAD3550)
static const uint8_t RP_FLOAT = 1, RP_VECTOR = 3, RP_INT = 5, RP_BOOL = 6, RP_ENTITY = 0x2A, RP_TYPED_ENTITY = 0x2B, RP_ITEMFLAVOR = 0x2C;

// One registered client/UI remote function as the S30 scripts declared it.
struct RemoteSchema_t
{
	bool bUI;
	uint32_t nParams;
	uint8_t nType[kMaxArgs];
	int32_t nMin[kMaxArgs], nMax[kMaxArgs];
	float flMin[kMaxArgs], flMax[kMaxArgs];
	uint32_t nBits[kMaxArgs];
};

static SRWLOCK s_lock = SRWLOCK_INIT;
static std::unordered_map<std::string, RemoteSchema_t> s_schemas;

//-----------------------------------------------------------------------------
// Registered remote functions
//-----------------------------------------------------------------------------
static bool ScriptRemote_ParseSchema(const uint8_t* p, int nBytes, int nParams, RemoteSchema_t& s)
{
	if (nParams < 0 || nParams > kMaxArgs || nBytes < 0 || nBytes > 256 || (nBytes && !p))
		return false;
	s.nParams = static_cast<uint32_t>(nParams);
	int pos = 0;
	for (int i = 0; i < nParams; ++i)
	{
		if (pos >= nBytes)
			return false;
		const uint8_t t = p[pos++];
		s.nType[i] = t;
		int nSkip = 0;
		switch (t)
		{
		case RP_FLOAT:
		case RP_VECTOR: // min, max, bits
			nSkip = 12;
			if (pos + nSkip <= nBytes)
			{
				memcpy(&s.flMin[i], p + pos, 4);
				memcpy(&s.flMax[i], p + pos + 4, 4);
				memcpy(&s.nBits[i], p + pos + 8, 4);
			}
			break;
		case RP_INT: // min, max
			nSkip = 8;
			if (pos + nSkip <= nBytes)
			{
				memcpy(&s.nMin[i], p + pos, 4);
				memcpy(&s.nMax[i], p + pos + 4, 4);
			}
			break;
		case RP_BOOL:
		case RP_ENTITY:
			break;
		case RP_TYPED_ENTITY: // class name
			while (pos + nSkip < nBytes && p[pos + nSkip])
				++nSkip;
			++nSkip;
			break;
		case RP_ITEMFLAVOR: // kind byte, then a range (2 ints) or a type (1 int)
			if (pos >= nBytes)
				return false;
			nSkip = p[pos] ? 9 : 5;
			break;
		default:
			return false;
		}
		if (pos + nSkip > nBytes)
			return false;
		pos += nSkip;
	}
	return pos == nBytes;
}

static int h_StoreRemote(void* pThis, void* hFunc, const char* pszName, int nFlags, const uint8_t* pSchema, int nSchemaBytes, int nParams)
{
	const int nResult = v_StoreRemote(pThis, hFunc, pszName, nFlags, pSchema, nSchemaBytes, nParams);
	if (nResult != 0 || !pszName || !pszName[0])
		return nResult;
	RemoteSchema_t s = {};
	s.bUI = (nFlags & 2) != 0;
	if (!ScriptRemote_ParseSchema(pSchema, nSchemaBytes, nParams, s))
	{
		S30Log("[SCRIPT-REMOTE] %s: schema unreadable (%d bytes, %d params) -- its calls are dropped\n", pszName, nSchemaBytes, nParams);
		return nResult;
	}
	AcquireSRWLockExclusive(&s_lock);
	s_schemas[pszName] = s;
	ReleaseSRWLockExclusive(&s_lock);
	return nResult;
}

// Server functions (Remote_RegisterServerFunction), by the index S30 sends them with.
struct ServerFunc_t
{
	std::string name;
	RemoteSchema_t schema;
};
static const int kMaxServerFuncs = 1024; // the 10-bit index
static ServerFunc_t s_serverFuncs[kMaxServerFuncs];
static const uint16_t* s_pServerFuncCount = nullptr;

static int h_StoreServer(void* pThis, const char* pszName, int nFlags, const uint8_t* pSchema, int nSchemaBytes, int nParams)
{
	const uint16_t nIndex = s_pServerFuncCount ? *s_pServerFuncCount : 0xFFFF;
	const int nResult = v_StoreServer(pThis, pszName, nFlags, pSchema, nSchemaBytes, nParams);
	if (nResult != 0 || !pszName || nIndex >= kMaxServerFuncs)
		return nResult;
	RemoteSchema_t s = {};
	const bool bOk = ScriptRemote_ParseSchema(pSchema, nSchemaBytes, nParams, s);
	AcquireSRWLockExclusive(&s_lock);
	s_serverFuncs[nIndex].name = bOk ? pszName : "";
	s_serverFuncs[nIndex].schema = s;
	ReleaseSRWLockExclusive(&s_lock);
	if (!bOk)
		S30Log("[SCRIPT-REMOTE] server function %s: schema unreadable (%d bytes, %d params) -- its calls are dropped\n",
			pszName, nSchemaBytes, nParams);
	return nResult;
}

// Bits of an int param's range, as the S30 encoder counts them (0xAD48A0).
static int ScriptRemote_IntBits(int32_t nMin, int32_t nMax)
{
	int n = 0;
	for (uint32_t i = static_cast<uint32_t>(nMax - nMin); i; i >>= 1)
		++n;
	return n;
}

static float ScriptRemote_Dequantize(uint32_t q, float flMin, float flMax, uint32_t nBits)
{
	return flMin + (flMax - flMin) * static_cast<float>(q) / static_cast<float>((1u << nBits) - 1);
}

bool S30ScriptRemote_RelayC2S(S30BitReader_t& r, int nEndBit, uint32_t nSeq)
{
	// S30 4: [1 typed][15 byte length][1 0][payload]; payload [10 index][31 0][1 0][args]
	// Server functions are all typed registrations (0xAD4140 passes flags 1), so typed is 1.
	const uint32_t bTyped = r.Read(1);
	const uint32_t nBytes = r.Read(15);
	r.Read(1);
	if (!bTyped || r.m_bOverflow || static_cast<int>(nBytes) * 8 > nEndBit - r.m_nPos)
	{
		S30Log("[SCRIPT-REMOTE] client script message: typed %u, %u bytes of %d bits -- not a server call, dropped\n",
			bTyped, nBytes, nEndBit - r.m_nPos);
		return false;
	}
	const int nPayloadEnd = r.m_nPos + static_cast<int>(nBytes) * 8;
	const uint32_t nIndex = r.Read(10);
	r.Read(32);
	ServerFunc_t f;
	AcquireSRWLockShared(&s_lock);
	if (nIndex < kMaxServerFuncs)
		f = s_serverFuncs[nIndex];
	ReleaseSRWLockShared(&s_lock);
	if (f.name.empty())
	{
		S30Log("[SCRIPT-REMOTE] client call to server function #%u: not a registration the bridge saw -- dropped\n", nIndex);
		return false;
	}
	// The dedi's name-carried frame (docs/15): [u32 'C2RS'][u32 seq][u8 name length][name][u8 args]
	// then per arg [u8 tag][value]: 1 float, 3 vector, 5 int, 6 bool, 0x28/0x29 entity, 0x2A itemflavor.
	uint8_t frame[512];
	uint32_t off = 0;
	auto put = [&](const void* p, uint32_t n) -> bool
	{
		if (off + n > sizeof(frame))
			return false;
		memcpy(frame + off, p, n);
		off += n;
		return true;
	};
	const uint32_t nMagic = kC2SFrameMagic;
	const uint8_t nName = static_cast<uint8_t>(f.name.size() > 64 ? 0 : f.name.size());
	const uint8_t nArgs = static_cast<uint8_t>(f.schema.nParams);
	bool bOk = nName && put(&nMagic, 4) && put(&nSeq, 4) && put(&nName, 1) && put(f.name.data(), nName) && put(&nArgs, 1);
	for (uint32_t i = 0; i < f.schema.nParams && bOk; ++i)
	{
		const RemoteSchema_t& s = f.schema;
		uint8_t nTag = 0;
		switch (s.nType[i])
		{
		case RP_INT:
		{
			const int32_t v = static_cast<int32_t>(r.Read(ScriptRemote_IntBits(s.nMin[i], s.nMax[i]))) + s.nMin[i];
			nTag = 5;
			bOk = put(&nTag, 1) && put(&v, 4);
			break;
		}
		case RP_FLOAT:
		case RP_VECTOR:
		{
			float v[3];
			const int nComps = s.nType[i] == RP_VECTOR ? 3 : 1;
			for (int k = 0; k < nComps; ++k)
			{
				if (s.nBits[i] >= 32)
				{
					const uint32_t raw = r.Read(32);
					memcpy(&v[k], &raw, 4);
				}
				else
					v[k] = ScriptRemote_Dequantize(r.Read(static_cast<int>(s.nBits[i])), s.flMin[i], s.flMax[i], s.nBits[i]);
			}
			nTag = s.nType[i] == RP_VECTOR ? 3 : 1;
			bOk = put(&nTag, 1) && put(v, 4 * nComps);
			break;
		}
		case RP_BOOL:
		{
			const uint8_t v = static_cast<uint8_t>(r.Read(1));
			nTag = 6;
			bOk = put(&nTag, 1) && put(&v, 1);
			break;
		}
		case RP_ENTITY:
		case RP_TYPED_ENTITY:
		{
			const uint32_t eh = r.Read(32); // (serial << 16) | index on both sides (docs/15)
			nTag = s.nType[i] == RP_ENTITY ? 0x28 : 0x29;
			bOk = put(&nTag, 1) && put(&eh, 4);
			break;
		}
		case RP_ITEMFLAVOR:
		{
			const uint32_t v = r.Read(32);
			nTag = 0x2A;
			bOk = put(&nTag, 1) && put(&v, 4);
			break;
		}
		default:
			bOk = false;
			break;
		}
	}
	if (!bOk || r.m_bOverflow || r.m_nPos > nPayloadEnd || nPayloadEnd - r.m_nPos > 7)
	{
		S30Log("[SCRIPT-REMOTE] client call %s: payload does not match its registration (%d of %u bytes) -- dropped\n",
			f.name.c_str(), (r.m_nPos - (nPayloadEnd - static_cast<int>(nBytes) * 8)) / 8, nBytes);
		return false;
	}
	// S3 net_ScriptMessage (68), untyped: [1 0][16 byte length][frame]
	uint8_t msg[540];
	S30BitWriter_t w(msg, sizeof(msg));
	w.Write(68, 7);
	w.Write(0, 1);
	w.Write(off, 16);
	for (uint32_t i = 0; i < off; ++i)
		w.Write(frame[i], 8);
	if (w.m_bOverflow)
		return false;
	const bool bQueued = S30Netchan_QueueC2S(msg, w.m_nPos, 3, 50);
	static volatile LONG s_nLog = 0;
	if (!bQueued || InterlockedIncrement(&s_nLog) <= 64)
		S30Log("[SCRIPT-REMOTE] client call %s (%u args, seq %u)%s\n", f.name.c_str(), f.schema.nParams, nSeq,
			bQueued ? "" : " -- C2S queue full, dropped");
	return bQueued;
}

//-----------------------------------------------------------------------------
// Calls
//-----------------------------------------------------------------------------
struct RemoteArg_t
{
	uint8_t nTag; // the dedi's wire tag: 1 float, 3 vector, 5 int, 6 bool, 0x22 string, 0x28/0x29 entity
	int32_t nInt;
	float flFloat;
	float vec[3];
	uint32_t nEHandle;
	char szString[256];
};

struct RemoteCall_t
{
	char szName[65];
	bool bUI;
	uint32_t nTick;
	uint32_t nHolds;
	uint32_t nArgs;
	RemoteArg_t args[kMaxArgs];
};

static const int kQueueSize = 256;
static RemoteCall_t s_queue[kQueueSize];
static uint32_t s_nHead = 0, s_nTail = 0;
static const uint32_t kMaxHolds = 64; // snapshots a call may wait for its stamp

// Function handles per VM (FindFunction allocates one per lookup).
struct FuncCache_t
{
	char szName[65];
	void* pVM;
	void* hFunc;
};
static FuncCache_t s_funcs[512];
static int s_nFuncs = 0;

static void* ScriptRemote_Entity(uint32_t nEHandle)
{
	if (nEHandle == 0xFFFFFFFFu || !s_pEntityTable)
		return nullptr;
	const uint32_t nIndex = nEHandle & 0xFFFF;
	if (nIndex >= kMaxEntities)
		return nullptr;
	const uint8_t* const pSlot = s_pEntityTable + 32ull * nIndex;
	if (*reinterpret_cast<const uint32_t*>(pSlot + 8) != (nEHandle >> 16))
		return nullptr;
	uint8_t* const pEntity = *reinterpret_cast<uint8_t* const*>(pSlot);
	return pEntity && pEntity[ENT_HEAVYWEIGHT] ? pEntity : nullptr;
}

static void* ScriptRemote_FindFunction(void* pVM, const char* pszName)
{
	for (int i = 0; i < s_nFuncs; ++i)
	{
		if (s_funcs[i].pVM == pVM && !strcmp(s_funcs[i].szName, pszName))
			return s_funcs[i].hFunc;
	}
	if (s_nFuncs >= static_cast<int>(sizeof(s_funcs) / sizeof(s_funcs[0])))
		return nullptr;
	void* const hFunc = v_FindFunction(pVM, pszName, nullptr, nullptr);
	if (hFunc)
	{
		FuncCache_t& e = s_funcs[s_nFuncs++];
		strcpy_s(e.szName, pszName);
		e.pVM = pVM;
		e.hFunc = hFunc;
	}
	return hFunc;
}

static int ScriptRemote_Execute(void* pVM, void* hFunc, ScriptVariant_t* pArgs, unsigned int nArgs, bool* pbFault)
{
	__try
	{
		ScriptVariant_t ret = {};
		return v_ExecuteFunction(pVM, hFunc, pArgs, nArgs, &ret, nullptr);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*pbFault = true;
		return -1;
	}
}

static void ScriptRemote_Run(RemoteCall_t& c)
{
	void* const pVM = c.bUI ? (s_ppUIVM ? *s_ppUIVM : nullptr) : (s_ppClientVM ? *s_ppClientVM : nullptr);
	if (!pVM)
	{
		S30LogFile("[SCRIPT-REMOTE] %s: %s VM not up -- dropped\n", c.szName, c.bUI ? "UI" : "CLIENT");
		return;
	}
	// A VM was rebuilt (level change): its cached handles are stale.
	for (int i = 0; i < s_nFuncs; )
	{
		if (s_funcs[i].pVM != *s_ppClientVM && (!s_ppUIVM || s_funcs[i].pVM != *s_ppUIVM))
			s_funcs[i] = s_funcs[--s_nFuncs];
		else
			++i;
	}
	void* const hFunc = ScriptRemote_FindFunction(pVM, c.szName);
	if (!hFunc)
	{
		static volatile LONG s_nMiss = 0;
		if (InterlockedIncrement(&s_nMiss) <= 32)
			S30Log("[SCRIPT-REMOTE] %s: no such function on the %s VM -- dropped\n", c.szName, c.bUI ? "UI" : "CLIENT");
		return;
	}
	alignas(16) ScriptVariant_t sv[kMaxArgs] = {};
	for (uint32_t i = 0; i < c.nArgs; ++i)
	{
		const RemoteArg_t& a = c.args[i];
		switch (a.nTag)
		{
		case 1: sv[i].nType = SV_FLOAT; sv[i].flFloat = a.flFloat; break;
		case 3: sv[i].nType = SV_VECTOR; memcpy(sv[i].vec, a.vec, sizeof(a.vec)); break;
		case 5: sv[i].nType = SV_INT; sv[i].nInt = a.nInt; break;
		case 6: sv[i].nType = SV_BOOL; sv[i].bBool = a.nInt != 0; break;
		case 0x22: sv[i].nType = SV_CSTRING; sv[i].pszString = a.szString; break;
		default: // entity: null when it is not on the client (yet)
		{
			void* const pEntity = ScriptRemote_Entity(a.nEHandle);
			sv[i].nType = SV_HSCRIPT;
			sv[i].hScript = pEntity && v_GetScriptInstance ? v_GetScriptInstance(pEntity) : nullptr;
			break;
		}
		}
	}
	bool bFault = false;
	const int nStatus = ScriptRemote_Execute(pVM, hFunc, sv, c.nArgs, &bFault);
	static volatile LONG s_nRun = 0;
	const LONG n = InterlockedIncrement(&s_nRun);
	if (bFault || nStatus < 0 || n <= 64)
		S30LogFile("[SCRIPT-REMOTE] #%ld %s(%u args) on %s: %s\n", n, c.szName, c.nArgs, c.bUI ? "UI" : "CLIENT",
			bFault ? "FAULT" : nStatus < 0 ? "script error" : "ok");
}

static bool ScriptRemote_Parse(const uint8_t* p, uint32_t nBytes, RemoteCall_t& c)
{
	uint32_t off = 0;
	auto take = [&](void* pDst, uint32_t n) -> bool
	{
		if (off + n > nBytes)
			return false;
		memcpy(pDst, p + off, n);
		off += n;
		return true;
	};
	uint32_t nMagic = 0;
	uint8_t bUI = 0, nReplay = 0, nName = 0, nArgs = 0;
	if (!take(&nMagic, 4) || nMagic != kFrameMagic || !take(&c.nTick, 4) || !take(&bUI, 1) || !take(&nReplay, 1)
		|| !take(&nName, 1) || !nName || nName > 64 || !take(c.szName, nName))
		return false;
	c.szName[nName] = '\0';
	for (uint8_t i = 0; i < nName; ++i)
	{
		if (c.szName[i] < 32 || c.szName[i] > 126)
			return false;
	}
	if (!take(&nArgs, 1) || nArgs > kMaxArgs)
		return false;
	c.nArgs = nArgs;
	for (uint32_t i = 0; i < nArgs; ++i)
	{
		RemoteArg_t& a = c.args[i];
		memset(&a, 0, sizeof(a));
		if (!take(&a.nTag, 1))
			return false;
		bool bOk = true;
		switch (a.nTag)
		{
		case 1: bOk = take(&a.flFloat, 4) && std::isfinite(a.flFloat); break;
		case 3: bOk = take(a.vec, 12) && std::isfinite(a.vec[0]) && std::isfinite(a.vec[1]) && std::isfinite(a.vec[2]); break;
		case 5: bOk = take(&a.nInt, 4); break;
		case 6:
		{
			uint8_t b = 0;
			bOk = take(&b, 1);
			a.nInt = b != 0;
			break;
		}
		case 0x22:
		{
			uint8_t nLen = 0;
			bOk = take(&nLen, 1) && take(a.szString, nLen);
			a.szString[nLen] = '\0';
			break;
		}
		case 0x28:
		case 0x29:
			bOk = take(&a.nEHandle, 4);
			break;
		default:
			bOk = false;
			break;
		}
		if (!bOk)
			return false;
	}
	c.bUI = bUI != 0;
	c.nHolds = 0;
	return off == nBytes;
}

// The dedi's wire tags against the S30 schema: counts, types and the registered
// ranges (as the S21 bridge checks them: a 32-bit float is sent raw, unranged).
static bool ScriptRemote_Matches(const RemoteCall_t& c, const RemoteSchema_t& s, char* pszWhy, size_t nWhy)
{
	if (c.nArgs != s.nParams)
	{
		sprintf_s(pszWhy, nWhy, "dedi sends %u args, S30 registers %u", c.nArgs, s.nParams);
		return false;
	}
	for (uint32_t i = 0; i < c.nArgs; ++i)
	{
		const RemoteArg_t& a = c.args[i];
		const uint8_t t = s.nType[i];
		const bool bRaw = s.nBits[i] >= 32;
		bool bType = false, bRange = true;
		switch (a.nTag)
		{
		case 1:
			bType = t == RP_FLOAT;
			bRange = bRaw || (a.flFloat >= s.flMin[i] && a.flFloat <= s.flMax[i]);
			break;
		case 3:
			bType = t == RP_VECTOR;
			for (int k = 0; k < 3 && !bRaw; ++k)
				bRange = bRange && a.vec[k] >= s.flMin[i] && a.vec[k] <= s.flMax[i];
			break;
		case 5:
			bType = t == RP_INT;
			bRange = a.nInt >= s.nMin[i] && a.nInt <= s.nMax[i];
			break;
		case 6:
			bType = t == RP_BOOL;
			break;
		case 0x28:
		case 0x29: // entity / typed_entity: both reach the script as the entity
			bType = t == RP_ENTITY || t == RP_TYPED_ENTITY;
			break;
		default: // a string: S30 remote functions take none
			break;
		}
		if (!bType)
		{
			sprintf_s(pszWhy, nWhy, "arg %u is dedi type 0x%02X, S30 registers 0x%02X", i, a.nTag, t);
			return false;
		}
		if (!bRange)
		{
			if (a.nTag == 5)
				sprintf_s(pszWhy, nWhy, "arg %u = %d outside the S30 range [%d, %d]", i, a.nInt, s.nMin[i], s.nMax[i]);
			else
				sprintf_s(pszWhy, nWhy, "arg %u outside the S30 range [%g, %g]", i, s.flMin[i], s.flMax[i]);
			return false;
		}
	}
	return true;
}

void S30ScriptRemote_OnFrame(const uint8_t* pFrame, uint32_t nBytes)
{
	if (!v_FindFunction || !v_ExecuteFunction)
		return;
	// The dedi's other frames on this lane (docs/16): its mantle-boost verdict for the
	// owner ('MBV1', [u32 magic][i32 state & 7 | seq << 3 | edict << 7]; S30 predicts the
	// boost natively, the verdict is logged to measure agreement) and the dev portal frame.
	uint32_t nMagic = 0;
	if (nBytes >= 4)
		memcpy(&nMagic, pFrame, 4);
	if (nMagic == kMantleBoostMagic && nBytes == 8)
	{
		int32_t nPacked = 0;
		memcpy(&nPacked, pFrame + 4, 4);
		static const char* const s_pszStates[8] = { "0", "1", "2", "FAILED", "BOOST", "5", "6", "7" };
		S30Log("[MANTLE] dedi verdict: %s (seq %d, edict %d)\n", s_pszStates[nPacked & 7], (nPacked >> 3) & 15, nPacked >> 7);
		return;
	}
	if (nMagic == kPortalMagic)
		return;
	RemoteCall_t& c = s_queue[s_nTail % kQueueSize];
	if (!ScriptRemote_Parse(pFrame, nBytes, c))
	{
		static volatile LONG s_nBad = 0;
		if (InterlockedIncrement(&s_nBad) <= 16)
			S30Log("[SCRIPT-REMOTE] malformed or non-call frame (%u bytes) -- dropped\n", nBytes);
		return;
	}
	// Only calls that match how the S30 scripts registered the function run (a
	// script error on the CLIENT VM drops the connection); the registration
	// decides the VM, not the wire's UI bit.
	RemoteSchema_t s = {};
	AcquireSRWLockShared(&s_lock);
	const auto it = s_schemas.find(c.szName);
	const bool bRegistered = it != s_schemas.end();
	if (bRegistered)
		s = it->second;
	ReleaseSRWLockShared(&s_lock);
	char szWhy[128];
	if (!bRegistered)
		strcpy_s(szWhy, "not a registered client/UI remote function");
	else if (ScriptRemote_Matches(c, s, szWhy, sizeof(szWhy)))
		szWhy[0] = '\0';
	if (szWhy[0])
	{
		// Each (name, reason) once: these are dedi/S30 script differences to fix.
		static std::unordered_set<std::string> s_reported;
		if (s_reported.size() < 256 && s_reported.insert(std::string(c.szName) + szWhy).second)
			S30Log("[SCRIPT-REMOTE] %s: %s -- dropped\n", c.szName, szWhy);
		return;
	}
	c.bUI = s.bUI;
	if (s_nTail - s_nHead >= kQueueSize)
	{
		S30Log("[SCRIPT-REMOTE] queue full -- running the oldest held call early\n");
		ScriptRemote_Run(s_queue[s_nHead++ % kQueueSize]);
	}
	++s_nTail;
}

// sdk_script_call: a no-argument CLIENT VM function, run on the engine thread.
static char s_szPendingCall[65];
static volatile LONG s_bPendingCall = 0;

void S30ScriptRemote_QueueClientCall(const char* pszName)
{
	if (!pszName || !pszName[0] || strlen(pszName) >= sizeof(s_szPendingCall) || InterlockedCompareExchange(&s_bPendingCall, 2, 0) != 0)
	{
		S30Log("[SCRIPT-REMOTE] sdk_script_call: bad name or a call is already pending\n");
		return;
	}
	strcpy_s(s_szPendingCall, pszName);
	InterlockedExchange(&s_bPendingCall, 1);
	S30Log("[SCRIPT-REMOTE] sdk_script_call %s: runs with the next snapshot\n", pszName);
}

void S30ScriptRemote_OnSnapshot(uint32_t nTick)
{
	if (s_bPendingCall == 1 && v_FindFunction)
	{
		RemoteCall_t c = {};
		strcpy_s(c.szName, s_szPendingCall);
		ScriptRemote_Run(c);
		InterlockedExchange(&s_bPendingCall, 0);
	}
	while (s_nHead != s_nTail)
	{
		RemoteCall_t& c = s_queue[s_nHead % kQueueSize];
		if (static_cast<int32_t>(nTick - c.nTick) < 0 && ++c.nHolds < kMaxHolds)
			break; // its snapshot has not arrived yet; later calls keep their order behind it
		++s_nHead;
		ScriptRemote_Run(c);
	}
}

//-----------------------------------------------------------------------------
// Install
//-----------------------------------------------------------------------------
// The host image section named pszName.
static bool ScriptRemote_Section(const char* pszName, uint8_t** ppStart, size_t* pnSize)
{
	const uintptr_t base = S30Image_Base();
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
	const IMAGE_SECTION_HEADER* pSec = IMAGE_FIRST_SECTION(pNt);
	for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i, ++pSec)
	{
		if (!strncmp(reinterpret_cast<const char*>(pSec->Name), pszName, IMAGE_SIZEOF_SHORT_NAME))
		{
			*ppStart = reinterpret_cast<uint8_t*>(base + pSec->VirtualAddress);
			*pnSize = pSec->Misc.VirtualSize;
			return true;
		}
	}
	return false;
}

// The CSquirrelVM* global loaded by `mov rcx, [rip+x]` nBack bytes before the
// one `lea rdx, [rip+string]` that names pszCallback.
static void** ScriptRemote_VMGlobal(const char* pszCallback, int nBack)
{
	uint8_t *pData = nullptr, *pText = nullptr;
	size_t nData = 0, nText = 0;
	if (!ScriptRemote_Section(".rdata", &pData, &nData) || !ScriptRemote_Section(".text", &pText, &nText))
		return nullptr;
	const size_t nLen = strlen(pszCallback) + 1;
	uint8_t* pString = nullptr;
	for (size_t off = 0; off + nLen <= nData && !pString; ++off)
	{
		if (pData[off] == static_cast<uint8_t>(pszCallback[0]) && !memcmp(pData + off, pszCallback, nLen))
			pString = pData + off;
	}
	if (!pString)
		return nullptr;
	uint8_t* pLea = nullptr;
	for (size_t off = static_cast<size_t>(nBack); off + 7 <= nText; ++off)
	{
		uint8_t* const p = pText + off;
		if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15)
			continue;
		int32_t nRel = 0;
		memcpy(&nRel, p + 3, sizeof(nRel));
		if (p + 7 + nRel != pString)
			continue;
		if (pLea)
			return nullptr; // a second reference: not the site this was verified at
		pLea = p;
	}
	if (!pLea)
		return nullptr;
	uint8_t* const pMov = pLea - nBack;
	if (pMov[0] != 0x48 || pMov[1] != 0x8B || pMov[2] != 0x0D)
		return nullptr;
	int32_t nRel = 0;
	memcpy(&nRel, pMov + 3, sizeof(nRel));
	return reinterpret_cast<void**>(pMov + 7 + nRel);
}

bool S30ScriptRemote_Install(void)
{
	int nFind = 0, nExec = 0, nInst = 0, nEnt = 0;
	v_FindFunction = reinterpret_cast<FindFunction_t>(S30Pattern_FindUnique(s_pszPatternFindFunction, &nFind));
	v_ExecuteFunction = reinterpret_cast<ExecuteFunction_t>(S30Pattern_FindUnique(s_pszPatternExecuteFunction, &nExec));
	v_GetScriptInstance = reinterpret_cast<GetScriptInstance_t>(S30Pattern_FindUnique(s_pszPatternGetScriptInstance, &nInst));
	uint8_t* const pEntNative = S30Pattern_FindUnique(s_pszPatternEntityFromEHandle, &nEnt);
	if (pEntNative)
	{
		int32_t nRel = 0;
		memcpy(&nRel, pEntNative + s_nEntityTableLea + 3, sizeof(nRel));
		s_pEntityTable = pEntNative + s_nEntityTableLea + 7 + nRel;
	}
	s_ppClientVM = ScriptRemote_VMGlobal("ClientCodeCallback_MapSpawn", 7);
	s_ppUIVM = ScriptRemote_VMGlobal("UICodeCallback_RunHeadless", 14);
	int nReg = 0;
	v_StoreRemote = reinterpret_cast<StoreRemote_t>(S30Pattern_FindUnique(s_pszPatternStoreRemote, &nReg));
	if (!v_FindFunction || !v_ExecuteFunction || !v_GetScriptInstance || !s_pEntityTable || !s_ppClientVM || !s_ppUIVM || !v_StoreRemote)
	{
		S30Log("[SCRIPT-REMOTE] unresolved (matches: FindFunction %d ExecuteFunction %d GetScriptInstance %d entity native %d"
			" register %d; client VM %p UI VM %p) -- S2C remote calls not run\n", nFind, nExec, nInst, nEnt, nReg,
			static_cast<void*>(s_ppClientVM), static_cast<void*>(s_ppUIVM));
		v_FindFunction = nullptr;
		v_ExecuteFunction = nullptr;
		return false;
	}
	int nServer = 0;
	uint8_t* const pStoreServer = S30Pattern_FindUnique(s_pszPatternStoreServer, &nServer);
	if (pStoreServer && pStoreServer[s_nServerCountRead] == 0x0F && pStoreServer[s_nServerCountRead + 1] == 0xB7
		&& pStoreServer[s_nServerCountRead + 2] == 0x05)
	{
		int32_t nRel = 0;
		memcpy(&nRel, pStoreServer + s_nServerCountRead + 3, sizeof(nRel));
		s_pServerFuncCount = reinterpret_cast<const uint16_t*>(pStoreServer + s_nServerCountRead + 7 + nRel);
		v_StoreServer = reinterpret_cast<StoreServer_t>(pStoreServer);
	}
	else
		S30Log("[SCRIPT-REMOTE] server-function registration unresolved (%d matches) -- client calls to the server dropped\n", nServer);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_StoreRemote), h_StoreRemote);
	if (v_StoreServer)
		DetourAttach(&reinterpret_cast<PVOID&>(v_StoreServer), h_StoreServer);
	const LONG nError = DetourTransactionCommit();
	S30Log("[SCRIPT-REMOTE] FindFunction 0x%llX ExecuteFunction 0x%llX GetScriptInstance 0x%llX entities 0x%llX, "
		"client VM 0x%llX UI VM 0x%llX, registration %s (commit %ld)\n",
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<void*>(v_FindFunction))),
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<void*>(v_ExecuteFunction))),
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<void*>(v_GetScriptInstance))),
		static_cast<unsigned long long>(S30Pattern_Rva(s_pEntityTable)),
		static_cast<unsigned long long>(S30Pattern_Rva(s_ppClientVM)),
		static_cast<unsigned long long>(S30Pattern_Rva(s_ppUIVM)),
		nError == NO_ERROR ? "hooked" : "FAILED", nError);
	if (nError != NO_ERROR)
	{
		v_FindFunction = nullptr;
		v_ExecuteFunction = nullptr;
	}
	return nError == NO_ERROR;
}
