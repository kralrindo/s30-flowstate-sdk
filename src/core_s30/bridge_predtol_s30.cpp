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
	{ "m_currentFramePlayer.timeBase", 0.06f }, // ticks + remainder: the same clock bookkeeping
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
// Client-owned values ([PRED-UNFED] shape)
//
// Predicted fields the dedi never authors for S30. The error check compares the
// predicted record (ctx +0x10) with the server record (ctx +0x18, the one a
// restore installs); a received value cannot fix these, the server record only
// refreshes when the dedi resends the field. So, as the S21 bridge does, the
// predicted value is copied into the server record just before the check:
//   predictableFlags (C_Player): a per-slot bit field. The dedi's copy is widened
//     to 8 bits with 8-slot masks for the S21 client ([PFLAGS8] in its
//     server.dll), but nothing on it sets slot 7 for S30 (client 0xF6, dedi 0x76):
//     the first failing field of 54 % of all errors. Only bit 7 is taken, and only
//     when it is the whole difference.
//   m_lastSprintPressTime, m_currentFrameLocalPlayer.m_duckTransitionRemainderMsec
//     (C_Player), m_flTimeWeaponIdle (CWeaponX): the dedi acks 0 against the
//     client's live value (measured; the S21 bridge masks the first two too).
//     Taken only while the server's value is 0.
//-----------------------------------------------------------------------------
static const int CTX_RECORD0 = 0x10;
static const int CTX_RECORD1 = 0x18;
static const int CTX_INDEX0 = 0x20;
static const int CTX_INDEX1 = 0x24;
static const int TD_FLAT = 0x70;

enum ClientValueRule_t
{
	RULE_BIT7,        // predictableFlags: the client's bit 7 when it is the only difference
	RULE_SERVER_ZERO, // the client's value while the server's is 0
};
struct ClientValue_t
{
	const char* pszClass;
	const char* pszField;
	ClientValueRule_t eRule;
	int nBytes;
};
static const ClientValue_t s_clientValues[] =
{
	{ "C_Player", "predictableFlags", RULE_BIT7, 4 },
	{ "C_Player", "m_lastSprintPressTime", RULE_SERVER_ZERO, 4 },
	{ "C_Player", "m_currentFrameLocalPlayer.m_duckTransitionRemainderMsec", RULE_SERVER_ZERO, 4 },
	{ "CWeaponX", "m_flTimeWeaponIdle", RULE_SERVER_ZERO, 4 },
};
static const int kClientValues = sizeof(s_clientValues) / sizeof(s_clientValues[0]);
static const uint32_t kPredictableFlagsClientBits = 0x80;

struct ClientValueCache_t
{
	uintptr_t pMap;
	int nBlock;
	const uint8_t* pTd[kClientValues]; // nullptr: not in that block
};
static ClientValueCache_t s_cache[32];
static int s_nCache = 0;
static volatile LONG s_nTaken[kClientValues];

static const ClientValueCache_t* PredTol_Lookup(uint8_t* pMap, int nBlock, const char* pszClass)
{
	for (int i = 0; i < s_nCache; ++i)
	{
		if (s_cache[i].pMap == reinterpret_cast<uintptr_t>(pMap) && s_cache[i].nBlock == nBlock)
			return &s_cache[i];
	}
	if (s_nCache >= static_cast<int>(sizeof(s_cache) / sizeof(s_cache[0])))
		return nullptr;
	ClientValueCache_t& c = s_cache[s_nCache++];
	c.pMap = reinterpret_cast<uintptr_t>(pMap);
	c.nBlock = nBlock;
	memset(c.pTd, 0, sizeof(c.pTd));
	uint8_t* const pOpt = *reinterpret_cast<uint8_t**>(pMap + DMAP_OPTIMIZED);
	if (!pOpt || nBlock < 0 || nBlock > 3)
		return &c;
	uint8_t* const pBlock = pOpt + nBlock * OPT_BLOCK_STRIDE;
	const uint8_t* const pFields = *reinterpret_cast<uint8_t* const*>(pBlock + OPT_BLOCK_FIELDS);
	const int nCount = *reinterpret_cast<const int*>(pBlock + OPT_BLOCK_COUNT);
	for (int i = 0; pFields && i < nCount && i < 8192; ++i)
	{
		const uint8_t* const pTd = pFields + static_cast<size_t>(i) * TD_STRIDE;
		const char* const pszName = *reinterpret_cast<const char* const*>(pTd + TD_NAME);
		for (int k = 0; pszName && k < kClientValues; ++k)
		{
			if (!strcmp(s_clientValues[k].pszClass, pszClass) && !strcmp(pszName, s_clientValues[k].pszField))
				c.pTd[k] = pTd;
		}
	}
	return &c;
}

void S30PredTol_BeforeErrorCheck(uint8_t* pCtx, uint8_t* pMap, int nBlock)
{
	const char* const pszClass = *reinterpret_cast<const char* const*>(pMap + DMAP_CLASSNAME);
	if (!pszClass || (strcmp(pszClass, "C_Player") && strcmp(pszClass, "CWeaponX")))
		return;
	const ClientValueCache_t* const pCache = PredTol_Lookup(pMap, nBlock, pszClass);
	uint8_t* const pPredicted = *reinterpret_cast<uint8_t**>(pCtx + CTX_RECORD0);
	uint8_t* const pServer = *reinterpret_cast<uint8_t**>(pCtx + CTX_RECORD1);
	const int nIdx0 = *reinterpret_cast<const int*>(pCtx + CTX_INDEX0);
	const int nIdx1 = *reinterpret_cast<const int*>(pCtx + CTX_INDEX1);
	if (!pCache || !pPredicted || !pServer || nIdx0 < 0 || nIdx0 > 3 || nIdx1 < 0 || nIdx1 > 3)
		return;
	for (int k = 0; k < kClientValues; ++k)
	{
		const uint8_t* const pTd = pCache->pTd[k];
		if (!pTd)
			continue;
		uint32_t* const pA = reinterpret_cast<uint32_t*>(pPredicted + *reinterpret_cast<const int*>(pTd + TD_FLAT + 4 * nIdx0));
		uint32_t* const pB = reinterpret_cast<uint32_t*>(pServer + *reinterpret_cast<const int*>(pTd + TD_FLAT + 4 * nIdx1));
		bool bTaken = false;
		if (s_clientValues[k].eRule == RULE_BIT7)
		{
			if ((*pA ^ *pB) == kPredictableFlagsClientBits)
			{
				*pA |= kPredictableFlagsClientBits;
				*pB |= kPredictableFlagsClientBits;
				bTaken = true;
			}
		}
		else if (!*pB && *pA)
		{
			*pB = *pA;
			bTaken = true;
		}
		if (bTaken)
		{
			const LONG nTaken = InterlockedIncrement(&s_nTaken[k]);
			if (nTaken == 1 || nTaken % 1000 == 0)
				S30LogFile("[PRED-UNFED] %s.%s: client value kept on the server record (#%ld)\n", pszClass,
					s_clientValues[k].pszField, nTaken);
		}
	}
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
