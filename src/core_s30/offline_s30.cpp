//=============================================================================//
//
// Purpose: keep the S30 client from starting the EA platform client.
//
// The S21 product does this with byte patches on the Origin launcher path
// (ebisusdk/origin_offline.cpp). S30 does not load OriginSDK at all; it brings
// up EOS instead, so the spawn itself is what is guarded here. Platform-client
// names match origin_offline.cpp's list.
//
//=============================================================================//
#include <windows.h>
#include <shlwapi.h>
#include <intrin.h>
#include <cstring>
#include "thirdparty/detours/include/detours.h"
#include "s30_log.h"
#include "offline_s30.h"

#pragma intrinsic(_ReturnAddress)

static decltype(&CreateProcessA) v_CreateProcessA = CreateProcessA;
static decltype(&CreateProcessW) v_CreateProcessW = CreateProcessW;

static bool Offline_IsPlatformClient(const char* pszDisplay)
{
	static const char* const s_ppszNames[] =
	{
		"EADesktop",
		"EALaunchHelper",
		"EABackgroundService",
		"EACoreServer",
		"EALocalHostSvc",
		"Origin.exe",
		"OriginClient",
		"EAAppLauncher",
	};
	if (!pszDisplay || !pszDisplay[0])
		return false;
	for (const char* pszName : s_ppszNames)
	{
		if (StrStrIA(pszDisplay, pszName))
			return true;
	}
	return false;
}

static BOOL WINAPI h_CreateProcessA(LPCSTR pszApp, LPSTR pszCmd, LPSECURITY_ATTRIBUTES pProcAttr, LPSECURITY_ATTRIBUTES pThreadAttr,
	BOOL bInherit, DWORD nFlags, LPVOID pEnv, LPCSTR pszDir, LPSTARTUPINFOA pStartup, LPPROCESS_INFORMATION pInfo)
{
	char szDisplay[512] = {};
	const char* pszSrc = pszApp ? pszApp : pszCmd;
	if (pszSrc)
		strncpy_s(szDisplay, pszSrc, _TRUNCATE);

	char szCaller[MAX_PATH];
	S30Log_FormatAddress(_ReturnAddress(), szCaller, sizeof(szCaller));
	if (Offline_IsPlatformClient(szDisplay))
	{
		S30Log("[OFFLINE] BLOCKED platform client spawn: %s (caller %s)\n", szDisplay, szCaller);
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	S30Log("[OFFLINE] spawn: %s (caller %s)\n", szDisplay, szCaller);
	return v_CreateProcessA(pszApp, pszCmd, pProcAttr, pThreadAttr, bInherit, nFlags, pEnv, pszDir, pStartup, pInfo);
}

static BOOL WINAPI h_CreateProcessW(LPCWSTR pwszApp, LPWSTR pwszCmd, LPSECURITY_ATTRIBUTES pProcAttr, LPSECURITY_ATTRIBUTES pThreadAttr,
	BOOL bInherit, DWORD nFlags, LPVOID pEnv, LPCWSTR pwszDir, LPSTARTUPINFOW pStartup, LPPROCESS_INFORMATION pInfo)
{
	char szDisplay[512] = {};
	const wchar_t* pwszSrc = pwszApp ? pwszApp : pwszCmd;
	if (pwszSrc)
		WideCharToMultiByte(CP_UTF8, 0, pwszSrc, -1, szDisplay, sizeof(szDisplay) - 1, NULL, NULL);

	char szCaller[MAX_PATH];
	S30Log_FormatAddress(_ReturnAddress(), szCaller, sizeof(szCaller));
	if (Offline_IsPlatformClient(szDisplay))
	{
		S30Log("[OFFLINE] BLOCKED platform client spawn: %s (caller %s)\n", szDisplay, szCaller);
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	S30Log("[OFFLINE] spawn: %s (caller %s)\n", szDisplay, szCaller);
	return v_CreateProcessW(pwszApp, pwszCmd, pProcAttr, pThreadAttr, bInherit, nFlags, pEnv, pwszDir, pStartup, pInfo);
}

bool S30Offline_Install(void)
{
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_CreateProcessA), h_CreateProcessA);
	DetourAttach(&reinterpret_cast<PVOID&>(v_CreateProcessW), h_CreateProcessW);
	const LONG nError = DetourTransactionCommit();
	S30Log("[OFFLINE] CreateProcess watchdog %s (commit %ld)\n", nError == NO_ERROR ? "armed" : "FAILED", nError);
	return nError == NO_ERROR;
}
