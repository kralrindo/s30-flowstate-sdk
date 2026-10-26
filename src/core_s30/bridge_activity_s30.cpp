//=============================================================================//
//
// Purpose: the dedi's queued player activity (S3 ids) on the S30 client.
//
// The S21 client's Hook_ActivitySelector, ported (s30/docs/14): the dedi sends the
// player's queued activity in its own (S3) numbering, so the selector reads e.g.
// S3 693 JUMP_START as another activity (the jump T-pose). Around the selector the
// value is translated S3 -> S30 by name (activity_s3_to_s30_table.h) and restored
// after, so the next frame translates from the same wire value. Values S30 writes
// itself (0x6176D0) are already S30 ids and are left alone.
//
// S30.1: selector 0x619E30 (S21 0x5B55F0); anim state -> player +488 (S21 +464);
// queued flag player +8336, queued activity (u16) +8346 (S21 +8108 / +8118).
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include "thirdparty/detours/include/detours.h"
#include "bridge_activity_s30.h"
#include "activity_s3_to_s30_table.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternActivitySelector =
	"40 55 57 48 8B EC 48 83 EC ?? 48 8B 81 E8 01 00 00 48 8B F9 48 85 C0 74 ?? F6 80 ?? ?? ?? ?? 03";

static const int ANIMSTATE_PLAYER = 488;
static const int PLAYER_QUEUED_FLAG = 8336;
static const int PLAYER_QUEUED_ACT = 8346;

typedef __int16 (*ActivitySelector_t)(__int64 pAnimState);
static ActivitySelector_t v_ActivitySelector = nullptr;
static volatile LONG s_nTranslated = 0;

static bool Activity_IsLocalQueued(uint16_t nAct)
{
	for (const short nLocal : kS30LocalQueuedActivities)
	{
		if (nAct == static_cast<uint16_t>(nLocal))
			return true;
	}
	return false;
}

static int Activity_S3ToS30(int nS3)
{
	if (nS3 <= 0 || nS3 >= static_cast<int>(sizeof(kActS3ToS30) / sizeof(kActS3ToS30[0])))
		return nS3;
	const int nS30 = kActS3ToS30[nS3];
	return nS30 > 0 ? nS30 : 0; // no S30 counterpart: the unbound sentinel, as the dedi's policy
}

static __int16 h_ActivitySelector(__int64 pAnimState)
{
	uint8_t* pPlayer = nullptr;
	uint16_t nOld = 0;
	bool bTranslated = false;
	__try
	{
		pPlayer = pAnimState ? *reinterpret_cast<uint8_t**>(pAnimState + ANIMSTATE_PLAYER) : nullptr;
		if (pPlayer && pPlayer[PLAYER_QUEUED_FLAG])
		{
			nOld = *reinterpret_cast<uint16_t*>(pPlayer + PLAYER_QUEUED_ACT);
			if (!Activity_IsLocalQueued(nOld))
			{
				const int nNew = Activity_S3ToS30(nOld);
				if (nNew != nOld)
				{
					*reinterpret_cast<uint16_t*>(pPlayer + PLAYER_QUEUED_ACT) = static_cast<uint16_t>(nNew);
					bTranslated = true;
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		bTranslated = false;
	}
	const __int16 nResult = v_ActivitySelector(pAnimState);
	if (bTranslated)
	{
		*reinterpret_cast<uint16_t*>(pPlayer + PLAYER_QUEUED_ACT) = nOld;
		if (InterlockedIncrement(&s_nTranslated) <= 16)
			S30LogFile("[ACT-XLAT] queued activity S3 %u -> S30 %d\n", nOld, Activity_S3ToS30(nOld));
	}
	return nResult;
}

bool S30Activity_Install(void)
{
	int nMatches = 0;
	v_ActivitySelector = reinterpret_cast<ActivitySelector_t>(S30Pattern_FindUnique(s_pszPatternActivitySelector, &nMatches));
	if (!v_ActivitySelector)
	{
		S30Log("[ACT-XLAT] activity selector unresolved (%d matches) -- queued activities stay in S3 numbering\n", nMatches);
		return false;
	}
	const uintptr_t nRva = S30Pattern_Rva(reinterpret_cast<void*>(v_ActivitySelector));
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ActivitySelector), h_ActivitySelector);
	const LONG nError = DetourTransactionCommit();
	S30Log("[ACT-XLAT] activity selector 0x%llX %s\n", static_cast<unsigned long long>(nRva), nError == NO_ERROR ? "hooked" : "FAILED");
	return nError == NO_ERROR;
}
