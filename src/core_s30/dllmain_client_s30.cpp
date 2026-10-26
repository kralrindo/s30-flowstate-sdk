//=============================================================================//
//
// Purpose: client_s30 entry points.
//
// loader.dll calls S30_EarlyInit from its DllMain (before any host code runs)
// and SDK_Init from the LauncherMain gate, the same gate the S21 client uses.
//
//=============================================================================//
#include <windows.h>
#include <psapi.h>
#include "s30_image.h"
#include "s30_log.h"
#include "net_guard_s30.h"
#include "offline_s30.h"
#include "origin_offline_s30.h"
#include "crash_s30.h"
#include "console_s30.h"
#include "engine_log_s30.h"
#include "script_print_s30.h"
#include "sdk_commands_s30.h"
#include "overlay_s30.h"
#include "concommand_s30.h"
#include "net_diag_s30.h"
#include "identity_s30.h"
#include "bridge_signon_s30.h"
#include "bridge_netchan_s30.h"
#include "bridge_persistence_s30.h"
#include "bridge_usercmd_s30.h"
#include "bridge_scriptremote_s30.h"
#include "bridge_usermessage_s30.h"
#include "bridge_activity_s30.h"
#include "bridge_cmdseed_s30.h"
#include "bridge_hud_s30.h"
#include "bridge_animtrace_s30.h"
#include "bridge_predcensus_s30.h"
#include "bridge_prederr_s30.h"
#include "bridge_noclip_s30.h"
#include "bridge_guards_s30.h"
#include "bridge_netvars_s30.h"

#pragma comment(lib, "psapi.lib")

static bool s_bEarlyInitDone = false;
static bool s_bSdkInitDone = false;

//-----------------------------------------------------------------------------
// Boot watch: modules, windows and guard counters once a second for the first
// ten seconds, then every ten. Development aid until the SDK console exists.
//-----------------------------------------------------------------------------
static BOOL CALLBACK BootWatch_EnumWindow(HWND hWnd, LPARAM lParam)
{
	DWORD nPid = 0;
	GetWindowThreadProcessId(hWnd, &nPid);
	if (nPid == GetCurrentProcessId() && IsWindowVisible(hWnd))
	{
		char szTitle[256] = {};
		GetWindowTextA(hWnd, szTitle, sizeof(szTitle));
		RECT rc = {};
		GetClientRect(hWnd, &rc);
		S30LogFile("[BOOT] window \"%s\" %ldx%ld\n", szTitle, rc.right, rc.bottom);
		++*reinterpret_cast<int*>(lParam);
	}
	return TRUE;
}

static DWORD WINAPI BootWatch_Thread(LPVOID)
{
	DWORD nLastModules = 0;
	for (int nSec = 1;; ++nSec)
	{
		Sleep(1000);
		HMODULE hModules[1024];
		DWORD nNeeded = 0;
		EnumProcessModules(GetCurrentProcess(), hModules, sizeof(hModules), &nNeeded);
		const DWORD nModules = nNeeded / sizeof(HMODULE);
		if (nModules != nLastModules)
		{
			for (DWORD i = nLastModules; i < nModules && i < 1024; ++i)
			{
				char szName[MAX_PATH];
				if (GetModuleBaseNameA(GetCurrentProcess(), hModules[i], szName, sizeof(szName)))
					S30LogFile("[BOOT] module %s\n", szName);
			}
			nLastModules = nModules;
		}
		if (nSec == 20 || nSec == 40)
			S30ScriptPrint_Diag();
		S30Identity_Pump();
		// Engine registration waits for the engine's own statics (ICvar ready).
		static bool s_bEngineCommandsDone = false;
		if (!s_bEngineCommandsDone && S30ConCommand_IsReady())
		{
			s_bEngineCommandsDone = true;
			S30ConCommand_RegisterSdkCommands();
			S30ConCommand_WrapConnect();
		}
		if (nSec <= 10 || nSec % 10 == 0)
		{
			int nWindows = 0;
			EnumWindows(BootWatch_EnumWindow, reinterpret_cast<LPARAM>(&nWindows));
			PROCESS_MEMORY_COUNTERS pmc = {};
			GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
			S30LogFile("[BOOT] t=%ds windows=%d modules=%lu ws=%lluMB net_blocked=%ld\n", nSec, nWindows, nModules,
				static_cast<unsigned long long>(pmc.WorkingSetSize >> 20), S30NetGuard_BlockedCount());
		}
	}
}

//-----------------------------------------------------------------------------
// Exports
//-----------------------------------------------------------------------------
extern "C" __declspec(dllexport) bool S30_EarlyInit(void)
{
	if (s_bEarlyInitDone)
		return true;

	S30Log_Init();
	S30Console_Init();
	const S30Image_t* const pImage = S30Image_Get();
	S30Log("[S30] client_s30 early init, host base %p\n", reinterpret_cast<void*>(S30Image_Base()));
	if (!pImage)
	{
		// The loader only picks client_s30 for a known image; reaching this is a loader bug.
		S30Log("[S30] host image is not a supported S30 build -- refusing\n");
		return false;
	}
	S30Log("[S30] host build %s\n", pImage->pszBuild);
	S30Log("[S30] cmdline: %s\n", GetCommandLineA());
	S30Crash_Install();

	if (!S30NetGuard_Install())
		return false;
	if (!S30Offline_Install())
		return false;
	// A failed patch is not a network risk (the spawn watchdog still refuses the
	// EA app); the client then exits on its own, which the log explains.
	S30OriginOffline_Apply();
	S30EngineLog_Install();
	S30ScriptPrint_Install();
	S30Commands_RegisterBuiltins();
	S30AnimTrace_RegisterCommands();
	S30PredCensus_RegisterCommands();
	S30PredErr_RegisterCommands();
	S30ConCommand_Init();
	S30NetDiag_Install();
	S30Signon_Install();
	S30Netchan_Install();
	S30Persistence_Install();
	S30Usercmd_Install();
	S30ScriptRemote_Install();
	S30UserMessage_Install();
	S30Activity_Install();
	S30CmdSeed_Install();
	S30Hud_Install();
	S30PredErr_Install();
	S30Noclip_Install();
	S30Guards_Install();
	S30NetVars_Install();
	S30Identity_Init();
	S30Console_StartInput();

	s_bEarlyInitDone = true;
	return true;
}

extern "C" __declspec(dllexport) void SDK_Init(void)
{
	if (s_bSdkInitDone)
		return;
	s_bSdkInitDone = true;

	if (!s_bEarlyInitDone)
	{
		// The early stage is what keeps the client off the network; without it, stop.
		S30Log("[S30] SDK_Init without early init -- terminating\n");
		TerminateProcess(GetCurrentProcess(), 0xE0E0);
		return;
	}

	S30Log("[S30] SDK_Init at LauncherMain gate\n");
	// Not in early init: creating the vtable probe device under the loader lock is unsafe.
	S30Overlay_Install();
	CloseHandle(CreateThread(NULL, 0, BootWatch_Thread, NULL, 0, NULL));
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD nReason, LPVOID)
{
	if (nReason == DLL_PROCESS_ATTACH)
		DisableThreadLibraryCalls(hModule);
	else if (nReason == DLL_PROCESS_DETACH)
	{
		S30Log("[S30] detach, net_blocked=%ld\n", S30NetGuard_BlockedCount());
		S30Log_Shutdown();
	}
	return TRUE;
}
