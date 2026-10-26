//=============================================================================//
//
// Purpose: client_s30's own console commands.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "s30_log.h"
#include "s30_image.h"
#include "console_s30.h"
#include "net_guard_s30.h"
#include "overlay_s30.h"
#include "concommand_s30.h"
#include "identity_s30.h"
#include "bridge_handshake_s30.h"
#include "bridge_signon_s30.h"
#include "sdk_commands_s30.h"

struct S30Command_t
{
	const char* pszName;
	const char* pszHelp;
	S30CommandFn_t pfnCommand;
};

static const int s_nMaxCommands = 64;
static S30Command_t s_commands[s_nMaxCommands];
static int s_nCommands = 0;

void S30Commands_Register(const char* pszName, const char* pszHelp, S30CommandFn_t pfnCommand)
{
	if (s_nCommands >= s_nMaxCommands)
	{
		S30Log("[CMD] command table full, %s not registered\n", pszName);
		return;
	}
	s_commands[s_nCommands++] = { pszName, pszHelp, pfnCommand };
}

bool S30Commands_Dispatch(const char* pszLine)
{
	// Tokenize into a local copy: whitespace-separated, "quoted" args kept whole.
	char szCopy[1024];
	strncpy_s(szCopy, pszLine, _TRUNCATE);
	const char* ppszArgs[32];
	int nArgs = 0;
	for (char* p = szCopy; *p && nArgs < 32;)
	{
		while (*p == ' ' || *p == '\t')
			++p;
		if (!*p)
			break;
		if (*p == '"')
		{
			ppszArgs[nArgs++] = ++p;
			while (*p && *p != '"')
				++p;
		}
		else
		{
			ppszArgs[nArgs++] = p;
			while (*p && *p != ' ' && *p != '\t')
				++p;
		}
		if (*p)
			*p++ = '\0';
	}
	if (!nArgs)
		return false;

	for (int i = 0; i < s_nCommands; ++i)
	{
		if (!_stricmp(s_commands[i].pszName, ppszArgs[0]))
		{
			s_commands[i].pfnCommand(nArgs, ppszArgs);
			return true;
		}
	}
	return false;
}

int S30Commands_List(const char** ppszNames, const char** ppszHelp, int nMax)
{
	int nOut = 0;
	for (int i = 0; i < s_nCommands && nOut < nMax; ++i, ++nOut)
	{
		ppszNames[nOut] = s_commands[i].pszName;
		ppszHelp[nOut] = s_commands[i].pszHelp;
	}
	return nOut;
}

int S30Commands_Complete(const char* pszPrefix, const char** ppszOut, int nMax)
{
	const size_t nLen = strlen(pszPrefix);
	int nOut = 0;
	for (int i = 0; i < s_nCommands && nOut < nMax; ++i)
	{
		if (!_strnicmp(s_commands[i].pszName, pszPrefix, nLen))
			ppszOut[nOut++] = s_commands[i].pszName;
	}
	return nOut;
}

//-----------------------------------------------------------------------------
// Built-ins
//-----------------------------------------------------------------------------
static void Cmd_Help(int, const char* const*)
{
	S30Log("SDK commands (everything else goes to the engine):\n");
	for (int i = 0; i < s_nCommands; ++i)
		S30Log("  %-18s %s\n", s_commands[i].pszName, s_commands[i].pszHelp);
}

static void Cmd_Status(int, const char* const*)
{
	const S30Image_t* const pImage = S30Image_Get();
	S30Log("client_s30 on %s\n", pImage ? pImage->pszBuild : "(unknown image)");
	S30Log("  network guard: %ld outbound attempts refused\n", S30NetGuard_BlockedCount());
	S30Log("  overlay: %s, %lld frames presented\n", S30Overlay_IsVisible() ? "open" : "closed", S30Overlay_PresentCount());
	S30Log("  S3 bridge: %s\n", S30Bridge_StateName());
}

static void Cmd_Overlay(int nArgs, const char* const* ppszArgs)
{
	S30Overlay_SetVisible(nArgs > 1 ? atoi(ppszArgs[1]) != 0 : !S30Overlay_IsVisible());
}

static void Cmd_Clear(int, const char* const*)
{
	S30Console_ClearLines();
}

static void Cmd_Screenshot(int nArgs, const char* const* ppszArgs)
{
	char szPath[MAX_PATH];
	if (nArgs > 1)
		strncpy_s(szPath, ppszArgs[1], _TRUNCATE);
	else
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		CreateDirectoryA("platform\\screenshots", NULL);
		snprintf(szPath, sizeof(szPath), "platform\\screenshots\\s30_%04u%02u%02u_%02u%02u%02u.bmp",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	}
	S30Overlay_RequestScreenshot(szPath);
}

static void Cmd_Cvar(int nArgs, const char* const* ppszArgs)
{
	if (nArgs < 2)
	{
		S30Log("usage: sdk_cvar <name>\n");
		return;
	}
	if (!S30ConCommand_IsReady())
	{
		S30Log("ICvar not ready yet\n");
		return;
	}
	const int nFlags = S30ConCommand_GetFlags(ppszArgs[1]);
	if (nFlags < 0)
		S30Log("%s: not registered\n", ppszArgs[1]);
	else
		S30Log("%s: registered, flags 0x%08X%s%s\n", ppszArgs[1], nFlags, (nFlags & 2) ? " DEVELOPMENTONLY" : "",
			(nFlags & (1 << 19)) ? " RELEASE" : "");
}

static void Cmd_UnlockDevCvars(int, const char* const*)
{
	if (!S30ConCommand_IsReady())
	{
		S30Log("ICvar not ready yet\n");
		return;
	}
	S30ConCommand_UnlockDevelopmentOnly();
}

static void Cmd_Cbuf(int nArgs, const char* const* ppszArgs)
{
	char szText[1024] = {};
	for (int i = 1; i < nArgs; ++i)
	{
		if (i > 1)
			strncat_s(szText, " ", _TRUNCATE);
		strncat_s(szText, ppszArgs[i], _TRUNCATE);
	}
	if (!S30Console_SendToEngine(szText))
		S30Log("Cbuf_AddText unresolved\n");
}

static void Cmd_Identity(int, const char* const*)
{
	S30Identity_Print();
}

static void Cmd_BridgeConnect(int nArgs, const char* const* ppszArgs)
{
	S30Bridge_Connect(nArgs > 1 ? ppszArgs[1] : "127.0.0.1:37015");
}

static void Cmd_BridgeDisconnect(int, const char* const*)
{
	S30Bridge_Disconnect();
}

static void Cmd_ClientState(int, const char* const*)
{
	S30Signon_PrintClientState();
}

void S30Commands_RegisterBuiltins(void)
{
	S30Commands_Register("bridge_connect", "bridge_connect [ip:port] -- S3 dedi handshake (default 127.0.0.1:37015)", Cmd_BridgeConnect);
	S30Commands_Register("bridge_disconnect", "reset the S3 bridge handshake", Cmd_BridgeDisconnect);
	S30Commands_Register("sdk_clstate", "client state 0: signon, server address, netchannel, bridge routes", Cmd_ClientState);
	S30Commands_Register("sdk_identity", "show the offline platform identity (Nucleus id, tokens)", Cmd_Identity);
	S30Commands_Register("sdk_cbuf", "sdk_cbuf <text> -- send text to the engine command buffer, skipping SDK commands", Cmd_Cbuf);
	S30Commands_Register("sdk_cvar", "sdk_cvar <name> -- whether an engine command/convar exists, and its flags", Cmd_Cvar);
	S30Commands_Register("sdk_unlock_devcvars", "clear DEVELOPMENTONLY/HIDDEN on all engine commands and convars", Cmd_UnlockDevCvars);
	S30Commands_Register("sdk_help", "list SDK commands", Cmd_Help);
	S30Commands_Register("sdk_status", "SDK build, network guard and overlay state", Cmd_Status);
	S30Commands_Register("sdk_overlay", "sdk_overlay [0|1] -- show or hide the overlay (also ` or F10)", Cmd_Overlay);
	S30Commands_Register("clear", "clear the overlay console", Cmd_Clear);
	S30Commands_Register("sdk_screenshot", "sdk_screenshot [path.bmp] -- save the next frame, overlay included", Cmd_Screenshot);
}
