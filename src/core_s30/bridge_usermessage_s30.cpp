//=============================================================================//
//
// Purpose: the dedi's svc_UserMessage (S3 37) on the S30 client.
//
// S30.1 facts (s30/docs/13):
//   The user message list (CUserMessages::Register 0xBA5A50, in order at
//   0x61DE00) is S21's: the dedi's (S3) list without AchievementEvent,
//   UpdateJalopyRadar, CurrentTimescale and DesiredTimescale (S3 21..24), so an
//   S3 index from 25 on is 4 lower, except the remote function family (S3 52..54).
//   CUserMessages::HookMessage 0xBA5BE0 (this, name, handler); a handler takes the
//   payload's bf_read. Hooks at 0x8A9680: SayText 0x9A16E0, PlayerNotifyDidDamage
//   0x66C3C0, RemoteBulletFired 0x722BB0, RemoteWeaponReload 0x7249E0,
//   WeapProjFireCB 0x720960.
// The S21 bridge runs exactly these five through their handlers and no other.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "thirdparty/detours/include/detours.h"
#include "bridge_usermessage_s30.h"
#include "bridge_bits_s30.h"
#include "bridge_netchan_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternHookMessage =
	"40 56 48 83 EC 20 49 8B F0 E8 ?? ?? ?? ?? 83 F8 FF 0F 84 ?? ?? ?? ?? 48 63 C8 48 8B 05";

typedef void (*HookMessage_t)(void* pThis, const char* pszName, void* pfnHandler);
typedef __int64 (*UserMessageHandler_t)(void* pBf);

static HookMessage_t v_HookMessage = nullptr;

// The dedi's (S3) user messages, in its registration order (UserMessages_t).
static const char* const s_pszS3Names[] =
{
	"Geiger", "Train", "HudText", "SayText", "AnnounceText", "TextMsg", "HudMsg", "ResetHUD", "ItemPickup",
	"ShowMenu", "Shake", "Fade", "VGUIMenu", "Rumble", "Damage", "VoiceMask", "RequestState", "WeapProjFireCB",
	"PredSVEvent", "CreditsMsg", "LogoTimeMsg", "AchievementEvent", "UpdateJalopyRadar", "CurrentTimescale",
	"DesiredTimescale", "CreditsPortalMsg", "InventoryFlash", "IndicatorFlash", "ControlHelperAnimate", "TakePhoto",
	"Flash", "HudPingIndicator", "OpenRadialMenu", "AddLocator", "MPMapCompleted", "MPMapIncomplete",
	"MPMapCompletedData", "MPTauntEarned", "MPTauntUnlocked", "MPTauntLocked", "MPAllTauntsLocked",
	"PortalFX_Surface", "ChangePaintColor", "StartSurvey", "ApplyHitBoxDamageEffect", "SetMixLayerTriggerFactor",
	"TransitionFade", "HudElemSetVisibility", "HudElemLabelSetText", "HudElemImageSet", "HudElemSetColor",
	"HudElemSetColorBG", "RemoteUntypedFunctionCall", "RemoteFunctionCall", "RemoteFunctionCallsChecksum",
	"PlayerNotifyDidDamage", "RemoteBulletFired", "RemoteWeaponReload",
};
static const int kS3Count = static_cast<int>(sizeof(s_pszS3Names) / sizeof(s_pszS3Names[0]));

// The messages that run, and the handler S30 hooked for each.
struct Direct_t
{
	const char* pszName;
	UserMessageHandler_t pfnHandler;
};
static Direct_t s_direct[] =
{
	{ "SayText", nullptr },
	{ "WeapProjFireCB", nullptr },
	{ "PlayerNotifyDidDamage", nullptr },
	{ "RemoteBulletFired", nullptr },
	{ "RemoteWeaponReload", nullptr },
};

static void h_HookMessage(void* pThis, const char* pszName, void* pfnHandler)
{
	v_HookMessage(pThis, pszName, pfnHandler);
	if (!pszName)
		return;
	for (Direct_t& d : s_direct)
	{
		if (!d.pfnHandler && !strcmp(d.pszName, pszName))
		{
			d.pfnHandler = reinterpret_cast<UserMessageHandler_t>(pfnHandler);
			S30Log("[USERMSG] %s handler 0x%llX\n", pszName, static_cast<unsigned long long>(S30Pattern_Rva(pfnHandler)));
		}
	}
}

static bool UserMessage_Call(UserMessageHandler_t pfn, const uint8_t* pPayload, int nBytes)
{
	alignas(16) uint8_t bf[S30BfRead::SIZE];
	S30Netchan_BfInit(bf, pPayload, nBytes);
	__try
	{
		pfn(bf);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

bool S30UserMessage_OnS3(S30BitReader_t& r)
{
	const uint32_t nType = r.Read(8);
	const uint32_t nBits = r.Read(12);
	if (r.m_bOverflow || static_cast<int>(nBits) > r.BitsLeft())
	{
		S30Log("[USERMSG] S3 type %u: %u bits, %d left -- stop\n", nType, nBits, r.BitsLeft());
		return false;
	}
	uint8_t payload[520] = {}; // 12-bit length: at most 512 bytes
	const int nBytes = static_cast<int>((nBits + 7) / 8);
	for (int i = 0; i < nBytes; ++i)
	{
		const uint32_t nLeft = nBits - 8u * i;
		payload[i] = static_cast<uint8_t>(r.Read(nLeft >= 8 ? 8 : static_cast<int>(nLeft)));
	}
	const char* const pszName = nType < static_cast<uint32_t>(kS3Count) ? s_pszS3Names[nType] : nullptr;
	const Direct_t* pDirect = nullptr;
	for (const Direct_t& d : s_direct)
	{
		if (pszName && !strcmp(d.pszName, pszName))
			pDirect = &d;
	}
	if (!pDirect || !pDirect->pfnHandler || !nBytes)
	{
		static bool s_bSeen[256];
		if (!s_bSeen[nType])
		{
			s_bSeen[nType] = true;
			S30Log("[USERMSG] S3 type %u (%s, %u bits): %s -- consumed\n", nType, pszName ? pszName : "?", nBits,
				!pDirect ? "not one the bridge runs" : !pDirect->pfnHandler ? "no S30 handler hooked" : "empty");
		}
		return true;
	}
	const bool bOk = UserMessage_Call(pDirect->pfnHandler, payload, nBytes);
	static volatile LONG s_nLog = 0;
	const LONG n = InterlockedIncrement(&s_nLog);
	if (!bOk || n <= 16 || n % 500 == 0)
		S30LogFile("[USERMSG] #%ld %s (%d bytes)%s\n", n, pszName, nBytes, bOk ? "" : " FAULT");
	return true;
}

bool S30UserMessage_Install(void)
{
	int nMatches = 0;
	v_HookMessage = reinterpret_cast<HookMessage_t>(S30Pattern_FindUnique(s_pszPatternHookMessage, &nMatches));
	if (!v_HookMessage)
	{
		S30Log("[USERMSG] HookMessage unresolved (%d matches) -- user messages consumed only\n", nMatches);
		return false;
	}
	const uintptr_t nRva = S30Pattern_Rva(reinterpret_cast<void*>(v_HookMessage));
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_HookMessage), h_HookMessage);
	const LONG nError = DetourTransactionCommit();
	S30Log("[USERMSG] HookMessage 0x%llX %s\n", static_cast<unsigned long long>(nRva), nError == NO_ERROR ? "hooked" : "FAILED");
	return nError == NO_ERROR;
}
