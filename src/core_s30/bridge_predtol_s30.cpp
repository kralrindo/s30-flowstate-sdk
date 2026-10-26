//=============================================================================//
//
// Purpose: prediction error-check tolerances for the S3 dedi.
//
// The dedi runs S3's movement code; the client predicts with S30's. Their float
// results drift by fractions of a unit, and S30 compares m_localOrigin and
// m_vecVelocity exactly: every ack then counts as a prediction error, and an error
// restores the server state and replays every predicted command -- the movement
// corrections (and the prediction-error icon) on the S30 client. The S21 bridge
// gives these fields the tolerances below ([PRED-TOL], game/client/pred_authority);
// genuine multi-unit mispredictions still flag.
//
// S30.1 layout (live memory, local player):
//   C_Player vtable slot 14  GetPredDescMap -> datamap (class name +0x10)
//   datamap +0x38            optimized map; per-copy blocks of 0x68 bytes:
//                            fields +0x00, count +0x18 (block 0: everything,
//                            block 1: networked fields, the error check's list)
//   typedescription          0x88 bytes: type +0, name +0x08, tolerance +0x6C
// S30's own tolerances there: m_flFallVelocity 0.5, m_groundNormal 0.1,
// m_lastUCmdSimulationRemainderTime 0.0001, m_localOrigin/m_vecVelocity 0.
// A tolerance is only ever raised.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "bridge_predtol_s30.h"
#include "s30_image.h"
#include "s30_log.h"

static const uintptr_t kEntityHandleTable = 0x58D4A18; // 32-byte slots (bridge_scriptremote)
static const int kLocalPlayerSlot = 1;
static const int kVidxGetPredDescMap = 14;
static const int DMAP_CLASSNAME = 0x10;
static const int DMAP_OPTIMIZED = 0x38;
static const int OPT_BLOCK_STRIDE = 0x68;
static const int OPT_BLOCK_FIELDS = 0x00;
static const int OPT_BLOCK_COUNT = 0x18;
static const int OPT_BLOCKS = 2;
static const int TD_STRIDE = 0x88;
static const int TD_NAME = 0x08;
static const int TD_TOLERANCE = 0x6C;

struct PredTol_t
{
	const char* pszField;
	float flTolerance;
};
// The S21 bridge's defaults (sdk_pred_tol_origin / _velocity / _fallvelocity /
// _time / _groundnormal).
static const PredTol_t s_tolerances[] =
{
	{ "m_localOrigin", 0.60f },
	{ "m_vecVelocity", 2.0f },
	{ "m_vecAbsVelocity", 2.0f },
	{ "m_flFallVelocity", 3.0f },
	{ "m_lastUCmdSimulationRemainderTime", 0.06f },
	{ "m_airMoveBlockPlaneTime", 0.06f },
	{ "m_groundNormal", 0.06f },
};

static uintptr_t s_pAppliedMap = 0;

static int PredTol_ApplyBlock(uint8_t* pFields, int nCount, int nBlock)
{
	int nChanged = 0;
	for (int i = 0; i < nCount; ++i)
	{
		uint8_t* const pTd = pFields + static_cast<size_t>(i) * TD_STRIDE;
		const char* const pszName = *reinterpret_cast<const char* const*>(pTd + TD_NAME);
		if (!pszName)
			continue;
		for (const PredTol_t& t : s_tolerances)
		{
			if (strcmp(pszName, t.pszField))
				continue;
			float& flTol = *reinterpret_cast<float*>(pTd + TD_TOLERANCE);
			if (flTol < t.flTolerance)
			{
				S30LogFile("[PRED-TOL] block %d %s: tolerance %g -> %g\n", nBlock, pszName, flTol, t.flTolerance);
				flTol = t.flTolerance;
				++nChanged;
			}
		}
	}
	return nChanged;
}

static void PredTol_Apply(void)
{
	const uintptr_t nBase = S30Image_Base();
	uint8_t* const pPlayer = *reinterpret_cast<uint8_t**>(nBase + kEntityHandleTable + 0x20 * kLocalPlayerSlot);
	if (!pPlayer)
		return;
	void** const pVtable = *reinterpret_cast<void***>(pPlayer);
	const uintptr_t pMap = reinterpret_cast<uintptr_t>(reinterpret_cast<void* (*)(void*)>(pVtable[kVidxGetPredDescMap])(pPlayer));
	if (!pMap || pMap == s_pAppliedMap)
		return;
	const char* const pszClass = *reinterpret_cast<const char* const*>(pMap + DMAP_CLASSNAME);
	uint8_t* const pOpt = *reinterpret_cast<uint8_t**>(pMap + DMAP_OPTIMIZED);
	if (!pszClass || strcmp(pszClass, "C_Player") || !pOpt)
		return; // not the player yet, or its map is not optimized yet
	int nChanged = 0;
	for (int b = 0; b < OPT_BLOCKS; ++b)
	{
		uint8_t* const pBlock = pOpt + b * OPT_BLOCK_STRIDE;
		uint8_t* const pFields = *reinterpret_cast<uint8_t**>(pBlock + OPT_BLOCK_FIELDS);
		const int nCount = *reinterpret_cast<const int*>(pBlock + OPT_BLOCK_COUNT);
		if (pFields && nCount > 0 && nCount < 8192)
			nChanged += PredTol_ApplyBlock(pFields, nCount, b);
	}
	s_pAppliedMap = pMap;
	S30Log("[PRED-TOL] %s prediction map: %d tolerances raised for the S3 dedi\n", pszClass, nChanged);
}

//-----------------------------------------------------------------------------
// Client-owned bits ([PRED-UNFED] shape)
//
// predictableFlags is a per-slot bit field. The dedi's copy is widened to 8 bits
// with 8-slot set/clear masks for the S21 client ([PFLAGS8] in its server.dll),
// but nothing on the dedi sets slot 7 for S30: the client predicts 0xF6, the dedi
// acks 0x76. The error check hook found it the first failing field of 54 % of all
// prediction errors, each a full restore + replay -- the movement corrections. A
// received value cannot fix it: the server record only refreshes when the dedi
// resends the field, and its value never changes. So, as the S21 bridge does for
// fields the dedi never feeds, the predicted bit is copied into the server record
// just before the check, when that bit is the only difference; the other seven
// bits stay server-authored and compared.
//-----------------------------------------------------------------------------
static const int CTX_RECORD0 = 0x10;
static const int CTX_RECORD1 = 0x18;
static const int CTX_INDEX0 = 0x20;
static const int CTX_INDEX1 = 0x24;
static const int TD_FLAT = 0x70;
static const uint32_t kPredictableFlagsClientBits = 0x80;

struct ClientBitsField_t
{
	uintptr_t pMap;
	int nBlock;
	const uint8_t* pTd; // nullptr: not in that block
};
static ClientBitsField_t s_clientBits[8];
static int s_nClientBits = 0;

static const uint8_t* PredTol_FindField(uint8_t* pMap, int nBlock, const char* pszField)
{
	for (int i = 0; i < s_nClientBits; ++i)
	{
		if (s_clientBits[i].pMap == reinterpret_cast<uintptr_t>(pMap) && s_clientBits[i].nBlock == nBlock)
			return s_clientBits[i].pTd;
	}
	const uint8_t* pFound = nullptr;
	uint8_t* const pOpt = *reinterpret_cast<uint8_t**>(pMap + DMAP_OPTIMIZED);
	if (pOpt && nBlock >= 0 && nBlock < 4)
	{
		uint8_t* const pBlock = pOpt + nBlock * OPT_BLOCK_STRIDE;
		const uint8_t* const pFields = *reinterpret_cast<uint8_t* const*>(pBlock + OPT_BLOCK_FIELDS);
		const int nCount = *reinterpret_cast<const int*>(pBlock + OPT_BLOCK_COUNT);
		for (int i = 0; pFields && i < nCount && i < 8192; ++i)
		{
			const uint8_t* const pTd = pFields + static_cast<size_t>(i) * TD_STRIDE;
			const char* const pszName = *reinterpret_cast<const char* const*>(pTd + TD_NAME);
			if (pszName && !strcmp(pszName, pszField))
			{
				pFound = pTd;
				break;
			}
		}
	}
	if (s_nClientBits < static_cast<int>(sizeof(s_clientBits) / sizeof(s_clientBits[0])))
		s_clientBits[s_nClientBits++] = { reinterpret_cast<uintptr_t>(pMap), nBlock, pFound };
	return pFound;
}

void S30PredTol_BeforeErrorCheck(uint8_t* pCtx, uint8_t* pMap, int nBlock)
{
	const char* const pszClass = *reinterpret_cast<const char* const*>(pMap + DMAP_CLASSNAME);
	if (!pszClass || strcmp(pszClass, "C_Player"))
		return;
	const uint8_t* const pTd = PredTol_FindField(pMap, nBlock, "predictableFlags");
	uint8_t* const pRec0 = *reinterpret_cast<uint8_t**>(pCtx + CTX_RECORD0);
	uint8_t* const pRec1 = *reinterpret_cast<uint8_t**>(pCtx + CTX_RECORD1);
	const int nIdx0 = *reinterpret_cast<const int*>(pCtx + CTX_INDEX0);
	const int nIdx1 = *reinterpret_cast<const int*>(pCtx + CTX_INDEX1);
	if (!pTd || !pRec0 || !pRec1 || nIdx0 < 0 || nIdx0 > 3 || nIdx1 < 0 || nIdx1 > 3)
		return;
	uint32_t* const pA = reinterpret_cast<uint32_t*>(pRec0 + *reinterpret_cast<const int*>(pTd + TD_FLAT + 4 * nIdx0));
	uint32_t* const pB = reinterpret_cast<uint32_t*>(pRec1 + *reinterpret_cast<const int*>(pTd + TD_FLAT + 4 * nIdx1));
	if ((*pA ^ *pB) != kPredictableFlagsClientBits)
		return;
	*pA |= kPredictableFlagsClientBits;
	*pB |= kPredictableFlagsClientBits;
	static volatile LONG s_nMasked = 0;
	const LONG n = InterlockedIncrement(&s_nMasked);
	if (n == 1 || n % 1000 == 0)
		S30LogFile("[PRED-UNFED] predictableFlags bit 7 kept on the server record (#%ld)\n", n);
}

void S30PredTol_OnSnapshot(void)
{
	__try
	{
		PredTol_Apply();
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		static volatile LONG s_nFaults = 0;
		if (InterlockedIncrement(&s_nFaults) <= 3)
			S30Log("[PRED-TOL] apply faulted (0x%08lX)\n", GetExceptionCode());
	}
}
