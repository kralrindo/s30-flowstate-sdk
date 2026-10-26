//=============================================================================//
//
// Purpose: S30 persistent vars the dedi's pdef does not have.
//
// S30.1 facts (s30/docs/12, verified in the S30 IDB):
//   0xC58BF0  PersistenceGetVarHandle(name) native: checks the datadef is
//             loaded (persistence interface vtable +0x9B8), then looks the name
//             up (vtable +0x9F0: bool(iface, name, int64* out)) and pushes the
//             result as a typed handle; a miss raises "Invalid persistent var
//             name". The interface is the global its mov rcx, [rip+x] at +9 reads.
//   sh_stats_internals.gnut 1430 stores the handle in a var field
//   (StatEntry.persistenceFullKey_Current_Handle); no client script reads it back.
//   0xC34940  GetPersistentVar helper (natives 0x888080, 0xA87BB0) and
//   0xC349E0  GetPersistentVarAsInt helper (natives 0x8880F0, 0xA87C00):
//             name = string arg 2 (0xF64F10(vm, 2, 10, &obj), chars at
//             (obj & 0x1FFFFFFFFFFF) * 8 + 0x24), the same lookup, then a read
//             through the handle and a typed push (int: 0x3A8A40(vm, value)).
//             A miss raises "Invalid persistent var name". These reads cannot be
//             given a dummy handle (it would read another var): for a name the
//             pdef lacks they push int 0 and return 1 themselves.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_set>
#include "thirdparty/detours/include/detours.h"
#include "bridge_persistence_s30.h"
#include "console_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternGetVarHandle =
	"40 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 B8 09 00 00 84 C0 75 16 "
	"48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? B8 FF FF FF FF E9";
static const char* const s_pszPatternGetVarAsInt =
	"48 89 74 24 18 89 54 24 10 57 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 41 8B F8 48 8B F1";
static const char* const s_pszPatternGetVar =
	"48 89 5C 24 08 57 48 83 EC 30 48 8B 05 ?? ?? ?? ?? 41 8B D8 48 8B F9 83 78 5C 00";
static const int s_nIfaceDispOffset = 12; // disp32 of mov rcx, [rip+x] at +9
static const int s_nIfaceNextInsn = 16;
static const int s_nLookupSlot = 0x9F0 / 8;
static const int s_nGetStringCall = 0xA6; // in GetVarAsInt: call 0xF64F10 (get string arg)
static const int s_nPushIntCall = 0x17D;  // in GetVarAsInt: call 0x3A8A40 (push int)

typedef __int64 (*SQNative_t)(void* v);
typedef bool (*PersistLookup_t)(void* pIface, const char* pszName, int64_t* pOut);
typedef __int64 (*PersistRead_t)(void* v, __int64 a2, unsigned int nClient);
typedef char (*GetArg_t)(void* v, int nIndex, int nType, uint64_t** ppObj);
typedef void (*PushInt_t)(void* v, int nValue);

static SQNative_t v_GetVarHandle = nullptr;
static PersistRead_t v_GetVarAsInt = nullptr;
static PersistRead_t v_GetVar = nullptr;
static GetArg_t s_pfnGetArg = nullptr;
static PushInt_t s_pfnPushInt = nullptr;
static void** s_ppIface = nullptr;
static PersistLookup_t v_Lookup = nullptr;
static volatile LONG s_bLookupHooked = 0;
static thread_local bool t_bInGetVarHandle = false;

static SRWLOCK s_lock = SRWLOCK_INIT;
static std::unordered_set<std::string> s_missing;
static std::unordered_set<std::string> s_missingPrefixes;

static void Persistence_NoteMissing(const char* pszName, const char* pszHow)
{
	const char* const pBracket = strchr(pszName, '[');
	const std::string prefix = pBracket ? std::string(pszName, pBracket - pszName) : std::string(pszName);
	AcquireSRWLockExclusive(&s_lock);
	const bool bNew = s_missing.insert(pszName).second;
	const bool bNewPrefix = s_missingPrefixes.insert(prefix).second;
	const size_t nTotal = s_missing.size();
	ReleaseSRWLockExclusive(&s_lock);
	if (!bNew)
		return;
	S30LogFile("[PDEF-MISSING] '%s' not in the dedi's pdef -- %s (#%zu)\n", pszName, pszHow, nTotal);
	if (bNewPrefix)
	{
		char szLine[512];
		snprintf(szLine, sizeof(szLine),
			"[PDEF-MISSING] %s... not in the dedi's pdef (struct cap 40) -- client reads defaults; full list in the log\n",
			prefix.c_str());
		S30Console_Write(szLine, S30ConsoleColor_t::WARNING);
	}
}

static bool h_Lookup(void* pIface, const char* pszName, int64_t* pOut)
{
	if (v_Lookup(pIface, pszName, pOut))
		return true;
	if (!t_bInGetVarHandle || !pszName || !pszName[0])
		return false;
	Persistence_NoteMissing(pszName, "dummy handle");
	if (pOut)
		*pOut = 0; // only stored by the caller's script
	return true;
}

// The interface exists only once the engine is up: its lookup slot is swapped
// on the first PersistenceGetVarHandle call.
static void Persistence_HookLookup(void)
{
	if (s_bLookupHooked || !s_ppIface || !*s_ppIface)
		return;
	void** const pVtable = *static_cast<void***>(*s_ppIface);
	void** const pSlot = &pVtable[s_nLookupSlot];
	if (InterlockedCompareExchange(&s_bLookupHooked, 1, 0) != 0)
		return;
	v_Lookup = reinterpret_cast<PersistLookup_t>(*pSlot);
	DWORD nOld = 0;
	if (!VirtualProtect(pSlot, sizeof(void*), PAGE_READWRITE, &nOld))
	{
		S30Log("[PDEF-MISSING] lookup slot not writable -- missing vars stay script errors\n");
		return;
	}
	*pSlot = reinterpret_cast<void*>(h_Lookup);
	VirtualProtect(pSlot, sizeof(void*), nOld, &nOld);
	S30Log("[PDEF-MISSING] persistence lookup @ 0x%llX wrapped (vtable slot 0x9F0)\n",
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<void*>(v_Lookup))));
}

// The name argument when the dedi's pdef lacks it, else null.
static const char* Persistence_MissingArg(void* v)
{
	if (!s_pfnGetArg || !s_ppIface || !*s_ppIface)
		return nullptr;
	uint64_t* pObj = nullptr;
	if (!s_pfnGetArg(v, 2, 10, &pObj) || !pObj)
		return nullptr;
	const char* const pszName = reinterpret_cast<const char*>((*pObj & 0x1FFFFFFFFFFFull) * 8 + 0x24);
	if (!pszName[0])
		return nullptr;
	void* const pIface = *s_ppIface;
	const PersistLookup_t pfnLookup = (*static_cast<PersistLookup_t**>(pIface))[s_nLookupSlot];
	int64_t nHandle = 0;
	return pfnLookup(pIface, pszName, &nHandle) ? nullptr : pszName;
}

static __int64 Persistence_ReadOrDefault(PersistRead_t pfnRead, void* v, __int64 a2, unsigned int nClient)
{
	const char* const pszName = Persistence_MissingArg(v);
	if (!pszName || !s_pfnPushInt)
		return pfnRead(v, a2, nClient);
	Persistence_NoteMissing(pszName, "read as 0");
	s_pfnPushInt(v, 0);
	return 1;
}

static __int64 h_GetVarAsInt(void* v, __int64 a2, unsigned int nClient)
{
	return Persistence_ReadOrDefault(v_GetVarAsInt, v, a2, nClient);
}

static __int64 h_GetVar(void* v, __int64 a2, unsigned int nClient)
{
	return Persistence_ReadOrDefault(v_GetVar, v, a2, nClient);
}

static void* Persistence_CallTarget(uint8_t* pCall)
{
	if (pCall[0] != 0xE8)
		return nullptr;
	int32_t nRel = 0;
	memcpy(&nRel, pCall + 1, sizeof(nRel));
	return pCall + 5 + nRel;
}

static __int64 h_GetVarHandle(void* v)
{
	Persistence_HookLookup();
	t_bInGetVarHandle = true;
	const __int64 nResult = v_GetVarHandle(v);
	t_bInGetVarHandle = false;
	return nResult;
}

bool S30Persistence_Install(void)
{
	int nMatches = 0;
	uint8_t* const pNative = S30Pattern_FindUnique(s_pszPatternGetVarHandle, &nMatches);
	if (!pNative)
	{
		S30Log("[PDEF-MISSING] PersistenceGetVarHandle unresolved (matches=%d) -- missing vars stay script errors\n", nMatches);
		return false;
	}
	int32_t nDisp = 0;
	memcpy(&nDisp, pNative + s_nIfaceDispOffset, sizeof(nDisp));
	s_ppIface = reinterpret_cast<void**>(pNative + s_nIfaceNextInsn + nDisp);
	v_GetVarHandle = reinterpret_cast<SQNative_t>(pNative);

	int nMatchesAsInt = 0, nMatchesVar = 0;
	uint8_t* const pAsInt = S30Pattern_FindUnique(s_pszPatternGetVarAsInt, &nMatchesAsInt);
	uint8_t* const pVar = S30Pattern_FindUnique(s_pszPatternGetVar, &nMatchesVar);
	if (pAsInt)
	{
		s_pfnGetArg = reinterpret_cast<GetArg_t>(Persistence_CallTarget(pAsInt + s_nGetStringCall));
		s_pfnPushInt = reinterpret_cast<PushInt_t>(Persistence_CallTarget(pAsInt + s_nPushIntCall));
	}
	const bool bReads = pAsInt && pVar && s_pfnGetArg && s_pfnPushInt;
	if (!bReads)
		S30Log("[PDEF-MISSING] persistent var reads unresolved (AsInt %d, Var %d matches) -- missing vars read as script errors\n",
			nMatchesAsInt, nMatchesVar);

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_GetVarHandle), h_GetVarHandle);
	if (bReads)
	{
		v_GetVarAsInt = reinterpret_cast<PersistRead_t>(pAsInt);
		v_GetVar = reinterpret_cast<PersistRead_t>(pVar);
		DetourAttach(&reinterpret_cast<PVOID&>(v_GetVarAsInt), h_GetVarAsInt);
		DetourAttach(&reinterpret_cast<PVOID&>(v_GetVar), h_GetVar);
	}
	const LONG nError = DetourTransactionCommit();
	S30Log("[PDEF-MISSING] PersistenceGetVarHandle @ 0x%llX, GetPersistentVar(AsInt) @ 0x%llX/0x%llX %s (commit %ld), interface @ 0x%llX\n",
		static_cast<unsigned long long>(S30Pattern_Rva(pNative)),
		static_cast<unsigned long long>(bReads ? S30Pattern_Rva(pVar) : 0),
		static_cast<unsigned long long>(bReads ? S30Pattern_Rva(pAsInt) : 0),
		nError == NO_ERROR ? "hooked" : "FAILED", nError, static_cast<unsigned long long>(S30Pattern_Rva(s_ppIface)));
	return nError == NO_ERROR;
}
