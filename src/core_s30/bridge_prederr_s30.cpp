//=============================================================================//
//
// Purpose: prediction error report (diagnostic).
//
// S30.1 0xA95A70 is the prediction copy's error check, ErrorCheck(ctx, datamap,
// block): it walks the optimized map's block (datamap +0x38, 0x68-byte blocks:
// fields +0x00, count +0x18) and compares each 0x88-byte typedescription's value
// in two records -- ctx +0x10 and +0x18 are the records, ctx +0x20 / +0x24 the
// flat-offset index each one uses (td +0x70 + 4 * index), ctx +0x28 the error
// flag. Fields with FTYPEDESC_SKIP (0x400) are not compared, floats use the
// tolerance at td +0x6C, and the walk stops at the first field that differs.
//
// The check itself is left alone. When it flags an error, this walks the same
// fields again with the same rules to name the first one that differs, and
// every 5 s logs the fields with the most errors and their last two values.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "thirdparty/detours/include/detours.h"
#include "bridge_prederr_s30.h"
#include "bridge_predtol_s30.h"
#include "sdk_commands_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternErrorCheck =
	"40 55 41 55 41 56 48 83 EC 70 49 63 C0 4C 8D 1D ?? ?? ?? ?? 4C 6B C0 68 8B 05 ?? ?? ?? ?? 48 8B E9 4C 03 42 38 4D 8B 28";

static const int CTX_RECORD0 = 0x10;
static const int CTX_RECORD1 = 0x18;
static const int CTX_INDEX0 = 0x20;
static const int CTX_INDEX1 = 0x24;
static const int CTX_ERRORS = 0x28;
static const int DMAP_CLASSNAME = 0x10;
static const int DMAP_OPTIMIZED = 0x38;
static const int OPT_BLOCK_STRIDE = 0x68;
static const int OPT_BLOCK_COUNT = 0x18;
static const int TD_STRIDE = 0x88;
static const int TD_TYPE = 0x00;
static const int TD_NAME = 0x08;
static const int TD_COUNT = 0x14;
static const int TD_FLAGS = 0x18;
static const int TD_BYTES = 0x50; // total size (weapons: 12 x 4 = 48)
static const int TD_TOLERANCE = 0x6C;
static const int TD_FLAT = 0x70;
static const int FTYPEDESC_SKIP = 0x400;

typedef uint64_t (*ErrorCheck_t)(uint8_t* pCtx, uint8_t* pMap, int nBlock);
static ErrorCheck_t v_ErrorCheck = nullptr;

struct ErrField_t
{
	const uint8_t* pTd;
	const char* pszClass;
	LONG nErrors;
	uint8_t a[16], b[16];
	int nBytes;
	int nElement; // first differing array element, -1 for a scalar
};
static ErrField_t s_fields[256];
static int s_nFields = 0;
static volatile LONG s_nChecks = 0, s_nErrorChecks = 0, s_nUnnamed = 0;
static volatile LONG s_bEnabled = 1;
static ULONGLONG s_nNextReportMs = 0;
static SRWLOCK s_lock = SRWLOCK_INIT;

// S30 field types holding 4-byte floats (float, vector, quaternion-ish, time).
static bool PredErr_IsFloatType(int nType)
{
	return nType == 1 || nType == 2 || nType == 3 || nType == 4 || nType == 16;
}

static void PredErr_Report(void)
{
	AcquireSRWLockExclusive(&s_lock);
	int order[256];
	for (int i = 0; i < s_nFields; ++i)
		order[i] = i;
	for (int i = 1; i < s_nFields; ++i)
	{
		const int v = order[i];
		int j = i - 1;
		while (j >= 0 && s_fields[order[j]].nErrors < s_fields[v].nErrors)
		{
			order[j + 1] = order[j];
			--j;
		}
		order[j + 1] = v;
	}
	S30LogFile("[PREDERR] %ld checks, %ld with an error (%ld unnamed); first failing field:\n", s_nChecks, s_nErrorChecks, s_nUnnamed);
	for (int k = 0; k < s_nFields && k < 20; ++k)
	{
		const ErrField_t& f = s_fields[order[k]];
		const int nType = *reinterpret_cast<const int*>(f.pTd + TD_TYPE);
		char szA[96] = "", szB[96] = "";
		if (PredErr_IsFloatType(nType))
		{
			for (int o = 0; o + 4 <= f.nBytes && o < 12; o += 4)
			{
				float x, y;
				memcpy(&x, f.a + o, 4);
				memcpy(&y, f.b + o, 4);
				char t[32];
				snprintf(t, sizeof(t), "%s%.4f", o ? " " : "", x);
				strncat_s(szA, t, _TRUNCATE);
				snprintf(t, sizeof(t), "%s%.4f", o ? " " : "", y);
				strncat_s(szB, t, _TRUNCATE);
			}
		}
		else
		{
			for (int o = 0; o < f.nBytes && o < 16; ++o)
			{
				char t[8];
				snprintf(t, sizeof(t), "%02X", f.a[o]);
				strncat_s(szA, t, _TRUNCATE);
				snprintf(t, sizeof(t), "%02X", f.b[o]);
				strncat_s(szB, t, _TRUNCATE);
			}
		}
		S30LogFile("[PREDERR]   %ld x %s.%s[%d] (type %d, tol %g): %s | %s\n", f.nErrors, f.pszClass,
			*reinterpret_cast<const char* const*>(f.pTd + TD_NAME), f.nElement, nType,
			*reinterpret_cast<const float*>(f.pTd + TD_TOLERANCE), szA, szB);
	}
	ReleaseSRWLockExclusive(&s_lock);
}

static bool PredErr_Differs(const uint8_t* pTd, const uint8_t* pA, const uint8_t* pB, int nBytes)
{
	if (!memcmp(pA, pB, nBytes))
		return false;
	const int nType = *reinterpret_cast<const int*>(pTd + TD_TYPE);
	if (!PredErr_IsFloatType(nType))
		return true;
	float flTol = *reinterpret_cast<const float*>(pTd + TD_TOLERANCE);
	if (nType == 16 && flTol < 0.025f)
		flTol = 0.025f; // FIELD_TIME: half a tick
	for (int o = 0; o + 4 <= nBytes; o += 4)
	{
		float x, y;
		memcpy(&x, pA + o, 4);
		memcpy(&y, pB + o, 4);
		if (x != y && !(fabsf(x - y) <= flTol))
			return true;
	}
	return false;
}

static void PredErr_Name(uint8_t* pCtx, uint8_t* pMap, int nBlock)
{
	uint8_t* const pOpt = *reinterpret_cast<uint8_t**>(pMap + DMAP_OPTIMIZED);
	if (!pOpt)
		return;
	uint8_t* const pBlock = pOpt + nBlock * OPT_BLOCK_STRIDE;
	const uint8_t* const pFields = *reinterpret_cast<uint8_t* const*>(pBlock);
	const int nCount = *reinterpret_cast<const int*>(pBlock + OPT_BLOCK_COUNT);
	const uint8_t* const pRec0 = *reinterpret_cast<uint8_t* const*>(pCtx + CTX_RECORD0);
	const uint8_t* const pRec1 = *reinterpret_cast<uint8_t* const*>(pCtx + CTX_RECORD1);
	const int nIdx0 = *reinterpret_cast<const int*>(pCtx + CTX_INDEX0);
	const int nIdx1 = *reinterpret_cast<const int*>(pCtx + CTX_INDEX1);
	if (!pFields || nCount <= 0 || nCount > 8192 || !pRec0 || !pRec1 || nIdx0 < 0 || nIdx0 > 3 || nIdx1 < 0 || nIdx1 > 3)
		return;
	for (int i = 0; i < nCount; ++i)
	{
		const uint8_t* const pTd = pFields + static_cast<size_t>(i) * TD_STRIDE;
		if (*reinterpret_cast<const int*>(pTd + TD_FLAGS) & FTYPEDESC_SKIP)
			continue;
		if (*reinterpret_cast<const int*>(pTd + TD_TYPE) == 0x21)
			continue; // embedded pointer: not followed here
		const int nBytes = *reinterpret_cast<const int*>(pTd + TD_BYTES); // the whole field, all elements
		const int nN = *reinterpret_cast<const uint16_t*>(pTd + TD_COUNT);
		const int nElem = nN > 1 ? nBytes / nN : nBytes;
		if (nBytes <= 0 || nBytes > 4096 || nElem <= 0)
			continue;
		const uint8_t* pA = pRec0 + *reinterpret_cast<const int*>(pTd + TD_FLAT + 4 * nIdx0);
		const uint8_t* pB = pRec1 + *reinterpret_cast<const int*>(pTd + TD_FLAT + 4 * nIdx1);
		if (!PredErr_Differs(pTd, pA, pB, nBytes))
			continue;
		// Arrays: keep the first differing element (the report shows 16 bytes).
		int nShown = nBytes;
		int nElement = -1;
		if (nN > 1)
		{
			for (int e = 0; e < nN; ++e)
			{
				if (PredErr_Differs(pTd, pA + e * nElem, pB + e * nElem, nElem))
				{
					nElement = e;
					break;
				}
			}
			if (nElement >= 0)
			{
				pA += nElement * nElem;
				pB += nElement * nElem;
				nShown = nElem;
			}
		}
		AcquireSRWLockExclusive(&s_lock);
		ErrField_t* pF = nullptr;
		for (int k = 0; k < s_nFields; ++k)
		{
			if (s_fields[k].pTd == pTd)
				pF = &s_fields[k];
		}
		if (!pF && s_nFields < 256)
		{
			pF = &s_fields[s_nFields++];
			memset(pF, 0, sizeof(*pF));
			pF->pTd = pTd;
			pF->pszClass = *reinterpret_cast<const char* const*>(pMap + DMAP_CLASSNAME);
		}
		if (pF)
		{
			++pF->nErrors;
			pF->nElement = nElement;
			pF->nBytes = nShown < 16 ? nShown : 16;
			memcpy(pF->a, pA, pF->nBytes);
			memcpy(pF->b, pB, pF->nBytes);
		}
		ReleaseSRWLockExclusive(&s_lock);
		return;
	}
	InterlockedIncrement(&s_nUnnamed);
}

static uint64_t h_ErrorCheck(uint8_t* pCtx, uint8_t* pMap, int nBlock)
{
	const int nBefore = pCtx ? *reinterpret_cast<const int*>(pCtx + CTX_ERRORS) : 0;
	if (pCtx && pMap)
	{
		// Not diagnostic: the client-owned bits the dedi never sets (bridge_predtol).
		__try
		{
			S30PredTol_BeforeErrorCheck(pCtx, pMap, nBlock);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
	const uint64_t nResult = v_ErrorCheck(pCtx, pMap, nBlock);
	if (!s_bEnabled || !pCtx || !pMap)
		return nResult;
	InterlockedIncrement(&s_nChecks);
	if (!nBefore && *reinterpret_cast<const int*>(pCtx + CTX_ERRORS))
	{
		InterlockedIncrement(&s_nErrorChecks);
		__try
		{
			PredErr_Name(pCtx, pMap, nBlock);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
	const ULONGLONG nNow = GetTickCount64();
	if (nNow >= s_nNextReportMs)
	{
		const bool bFirst = !s_nNextReportMs;
		s_nNextReportMs = nNow + 5000;
		if (!bFirst && s_nErrorChecks)
			PredErr_Report();
	}
	return nResult;
}

bool S30PredErr_Install(void)
{
	int nMatches = 0;
	v_ErrorCheck = reinterpret_cast<ErrorCheck_t>(S30Pattern_FindUnique(s_pszPatternErrorCheck, &nMatches));
	if (!v_ErrorCheck)
	{
		S30Log("[PREDERR] error check unresolved (%d matches) -- no prediction error report\n", nMatches);
		return false;
	}
	const uintptr_t nRva = S30Pattern_Rva(reinterpret_cast<void*>(v_ErrorCheck));
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ErrorCheck), h_ErrorCheck);
	const LONG nError = DetourTransactionCommit();
	S30Log("[PREDERR] error check 0x%llX %s\n", static_cast<unsigned long long>(nRva), nError == NO_ERROR ? "hooked" : "FAILED");
	return nError == NO_ERROR;
}

static void Cmd_PredErr(int nArgs, const char* const* ppszArgs)
{
	if (nArgs >= 2 && !strcmp(ppszArgs[1], "reset"))
	{
		AcquireSRWLockExclusive(&s_lock);
		s_nFields = 0;
		s_nChecks = s_nErrorChecks = s_nUnnamed = 0;
		ReleaseSRWLockExclusive(&s_lock);
	}
	else if (nArgs >= 2)
		InterlockedExchange(&s_bEnabled, atoi(ppszArgs[1]) != 0);
	PredErr_Report();
}

void S30PredErr_RegisterCommands(void)
{
	S30Commands_Register("sdk_prederr", "sdk_prederr [0|1|reset] -- which predicted field fails the error check", Cmd_PredErr);
}
