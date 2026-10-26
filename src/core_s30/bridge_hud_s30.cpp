//=============================================================================//
//
// Purpose: the dedi SDK's chat lanes and the server playlist on the S30 client.
//
// S30.1 (s30/docs/16):
//   CBaseHudChat PrintSystem(message, prefix, channel) 0x9A2000 (S21 0x8F2AE0).
//   Playlists_SetCurrentByName(name) 0x3DD340: the E8 at 0x2FF44C in
//   CClientState::SetSignonState (S21 0x2D682B -> 0x3AD7A0).
// svc_ChatBuilder (S3 70) is shown as plain text through PrintSystem: segment
// colours and fades are not drawn yet (S21 renders them into the RichText).
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "bridge_hud_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternPrintSystem =
	"48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 81 EC ?? ?? ?? ?? 48 8B 1D";
static const char* const s_pszPatternSetPlaylistCall =
	"E8 ?? ?? ?? ?? 84 C0 75 ?? 4D 8D 86 ?? ?? ?? ?? B9";

typedef void (*PrintSystem_t)(const char* pszMessage, const char* pszPrefix, int nChannel);
typedef char (*SetPlaylist_t)(const char* pszName);

static PrintSystem_t v_PrintSystem = nullptr;
static SetPlaylist_t v_SetPlaylist = nullptr;

static void Hud_Print(const char* pszMessage, const char* pszPrefix)
{
	S30Log("[CHAT] %s%s%s\n", pszPrefix, pszPrefix[0] ? " " : "", pszMessage);
	if (!v_PrintSystem)
		return;
	__try
	{
		v_PrintSystem(pszMessage, pszPrefix, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		S30Log("[CHAT] PrintSystem faulted\n");
	}
}

void S30Hud_SystemSayText(const char* pszPrefix, const char* pszMessage, bool bAdmin)
{
	char szPrefix[600];
	snprintf(szPrefix, sizeof(szPrefix), "%s%s", bAdmin ? "[ADMIN] " : "", pszPrefix ? pszPrefix : "");
	Hud_Print(pszMessage ? pszMessage : "", szPrefix);
}

void S30Hud_ChatBuilder(const uint8_t* p, int nBytes)
{
	// [8 flags (1 admin, 2 player chat)][8 segments <= 8][8 sender slot], then per segment
	// [8 op][8 flags (1 newline)][8 r][8 g][8 b][16 sustain ms][16 fade ms][8 length <= 64][text]
	if (!p || nBytes < 3)
		return;
	const uint8_t nFlags = p[0];
	const uint8_t nSegments = p[1];
	int off = 3;
	char szText[8 * 66 + 16] = "";
	size_t nLen = 0;
	for (int i = 0; i < nSegments && i < 8; ++i)
	{
		if (off + 10 > nBytes)
			return;
		const uint8_t nSegFlags = p[off + 1];
		const uint8_t nTextLen = p[off + 9];
		off += 10;
		if (nTextLen > 64 || off + nTextLen > nBytes)
			return;
		if ((nSegFlags & 1) && nLen && nLen + 1 < sizeof(szText))
			szText[nLen++] = ' ';
		memcpy(szText + nLen, p + off, nTextLen);
		nLen += nTextLen;
		szText[nLen] = '\0';
		off += nTextLen;
	}
	Hud_Print(szText, (nFlags & 1) ? "[ADMIN]" : "");
}

void S30Hud_PlaylistChange(const char* pszName)
{
	// S21Bridge_ApplyServerPlaylist: [A-Za-z0-9_]{1,63}, each new name once, at most 8 per session.
	static char s_szApplied[65];
	static int s_nApplies = 0;
	const size_t nLen = pszName ? strlen(pszName) : 0;
	if (!nLen || nLen > 63)
		return;
	for (size_t i = 0; i < nLen; ++i)
	{
		const char c = pszName[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
		{
			S30Log("[PLAYLIST] server playlist name '%s' rejected\n", pszName);
			return;
		}
	}
	if (!strcmp(s_szApplied, pszName) || s_nApplies >= 8 || !v_SetPlaylist)
		return;
	char bOk = 0;
	__try
	{
		bOk = v_SetPlaylist(pszName);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		bOk = 0;
	}
	++s_nApplies;
	if (bOk)
		strcpy_s(s_szApplied, pszName);
	S30Log("[PLAYLIST] server playlist '%s' %s\n", pszName, bOk ? "applied" : "not in the client's table (yet)");
}

void S30Hud_Install(void)
{
	int nPrint = 0, nSet = 0;
	v_PrintSystem = reinterpret_cast<PrintSystem_t>(S30Pattern_FindUnique(s_pszPatternPrintSystem, &nPrint));
	uint8_t* const pCall = S30Pattern_FindUnique(s_pszPatternSetPlaylistCall, &nSet);
	if (pCall)
	{
		int32_t nRel = 0;
		memcpy(&nRel, pCall + 1, sizeof(nRel));
		v_SetPlaylist = reinterpret_cast<SetPlaylist_t>(pCall + 5 + nRel);
	}
	S30Log("[CHAT] PrintSystem 0x%llX (%d matches), Playlists_SetCurrentByName 0x%llX (%d matches)%s\n",
		static_cast<unsigned long long>(v_PrintSystem ? S30Pattern_Rva(reinterpret_cast<void*>(v_PrintSystem)) : 0), nPrint,
		static_cast<unsigned long long>(v_SetPlaylist ? S30Pattern_Rva(reinterpret_cast<void*>(v_SetPlaylist)) : 0), nSet,
		v_PrintSystem ? "" : " -- server chat only in the log");
}
