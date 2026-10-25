//=============================================================================//
//
// Purpose: E0 boot probe for the S30 client.
//
// Imported by a patched copy of r5apex_dx12.exe (s30/tools/add_import.py). It
// proves the image runs an added import, keeps the process loopback-only
// (every non-loopback connect/sendto/DNS lookup is refused and logged), and
// logs how far boot gets. It is not the SDK.
//
// Log: probe.log next to the exe.
//
//=============================================================================//
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <psapi.h>
#include <winhttp.h>
#include <wininet.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include "../../src/thirdparty/detours/include/detours.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

static FILE* s_pLog = nullptr;
static CRITICAL_SECTION s_logLock;
static ULONGLONG s_nStartMs = 0;
static volatile LONG s_nBlocked = 0;
static volatile LONG s_nExceptions = 0;

static void Probe_Log(const char* fmt, ...)
{
	if (!s_pLog)
		return;
	EnterCriticalSection(&s_logLock);
	fprintf(s_pLog, "[%8.3f] [%5lu] ", (GetTickCount64() - s_nStartMs) / 1000.0, GetCurrentThreadId());
	va_list args;
	va_start(args, fmt);
	vfprintf(s_pLog, fmt, args);
	va_end(args);
	fflush(s_pLog);
	LeaveCriticalSection(&s_logLock);
}

// Name of the module that owns an address, for blame lines.
static void Probe_ModuleOf(const void* pAddr, char* pszOut, DWORD nOut)
{
	HMODULE hMod = NULL;
	pszOut[0] = '\0';
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(pAddr), &hMod) && hMod)
	{
		char szPath[MAX_PATH];
		if (GetModuleFileNameA(hMod, szPath, MAX_PATH))
		{
			const char* pszBase = strrchr(szPath, '\\');
			snprintf(pszOut, nOut, "%s+0x%llX", pszBase ? pszBase + 1 : szPath,
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pAddr) - reinterpret_cast<uintptr_t>(hMod)));
			return;
		}
	}
	snprintf(pszOut, nOut, "%p", pAddr);
}

//-----------------------------------------------------------------------------
// Loopback policy
//-----------------------------------------------------------------------------
static bool Probe_IsLoopback(const sockaddr* pAddr)
{
	if (!pAddr)
		return true; // connected socket: its connect() was already checked
	if (pAddr->sa_family == AF_INET)
	{
		const sockaddr_in* p4 = reinterpret_cast<const sockaddr_in*>(pAddr);
		return (ntohl(p4->sin_addr.s_addr) >> 24) == 127;
	}
	if (pAddr->sa_family == AF_INET6)
	{
		const sockaddr_in6* p6 = reinterpret_cast<const sockaddr_in6*>(pAddr);
		if (IN6_IS_ADDR_LOOPBACK(&p6->sin6_addr))
			return true;
		if (IN6_IS_ADDR_V4MAPPED(&p6->sin6_addr))
			return p6->sin6_addr.u.Byte[12] == 127;
		return false;
	}
	return false; // AF_UNIX etc.: not expected here, refuse
}

static void Probe_FormatAddr(const sockaddr* pAddr, char* pszOut, DWORD nOut)
{
	pszOut[0] = '\0';
	if (!pAddr)
		return;
	char szHost[INET6_ADDRSTRLEN] = {};
	USHORT nPort = 0;
	if (pAddr->sa_family == AF_INET)
	{
		const sockaddr_in* p4 = reinterpret_cast<const sockaddr_in*>(pAddr);
		inet_ntop(AF_INET, &p4->sin_addr, szHost, sizeof(szHost));
		nPort = ntohs(p4->sin_port);
	}
	else if (pAddr->sa_family == AF_INET6)
	{
		const sockaddr_in6* p6 = reinterpret_cast<const sockaddr_in6*>(pAddr);
		inet_ntop(AF_INET6, &p6->sin6_addr, szHost, sizeof(szHost));
		nPort = ntohs(p6->sin6_port);
	}
	else
	{
		snprintf(pszOut, nOut, "family=%d", pAddr->sa_family);
		return;
	}
	snprintf(pszOut, nOut, "%s:%u", szHost, nPort);
}

static bool Probe_IsLocalHostName(const char* pszName)
{
	return !pszName || !_stricmp(pszName, "localhost") || !strncmp(pszName, "127.", 4) || !strcmp(pszName, "::1");
}

static void Probe_Block(const char* pszWhat, const char* pszTarget, const void* pCaller)
{
	char szCaller[MAX_PATH];
	Probe_ModuleOf(pCaller, szCaller, sizeof(szCaller));
	InterlockedIncrement(&s_nBlocked);
	Probe_Log("[NET-GUARD] BLOCKED %s %s (caller %s)\n", pszWhat, pszTarget, szCaller);
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------
static decltype(&connect) s_pConnect = connect;
static decltype(&WSAConnect) s_pWSAConnect = WSAConnect;
static decltype(&sendto) s_pSendTo = sendto;
static decltype(&WSASendTo) s_pWSASendTo = WSASendTo;
static decltype(&getaddrinfo) s_pGetAddrInfo = getaddrinfo;
static decltype(&GetAddrInfoW) s_pGetAddrInfoW = GetAddrInfoW;
static decltype(&GetAddrInfoExW) s_pGetAddrInfoExW = GetAddrInfoExW;
static decltype(&gethostbyname) s_pGetHostByName = gethostbyname;
static decltype(&WSAIoctl) s_pWSAIoctl = WSAIoctl;
static LPFN_CONNECTEX s_pRealConnectEx = nullptr;

static int WSAAPI Hook_connect(SOCKET s, const sockaddr* name, int namelen)
{
	if (!Probe_IsLoopback(name))
	{
		char sz[96];
		Probe_FormatAddr(name, sz, sizeof(sz));
		Probe_Block("connect", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return SOCKET_ERROR;
	}
	return s_pConnect(s, name, namelen);
}

static int WSAAPI Hook_WSAConnect(SOCKET s, const sockaddr* name, int namelen, LPWSABUF a, LPWSABUF b, LPQOS c, LPQOS d)
{
	if (!Probe_IsLoopback(name))
	{
		char sz[96];
		Probe_FormatAddr(name, sz, sizeof(sz));
		Probe_Block("WSAConnect", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return SOCKET_ERROR;
	}
	return s_pWSAConnect(s, name, namelen, a, b, c, d);
}

static BOOL PASCAL Hook_ConnectEx(SOCKET s, const sockaddr* name, int namelen, PVOID pSend, DWORD nSend, LPDWORD pSent, LPOVERLAPPED pOv)
{
	if (!Probe_IsLoopback(name))
	{
		char sz[96];
		Probe_FormatAddr(name, sz, sizeof(sz));
		Probe_Block("ConnectEx", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return FALSE;
	}
	return s_pRealConnectEx(s, name, namelen, pSend, nSend, pSent, pOv);
}

static int WSAAPI Hook_WSAIoctl(SOCKET s, DWORD code, LPVOID pIn, DWORD nIn, LPVOID pOut, DWORD nOut, LPDWORD pRet, LPWSAOVERLAPPED pOv, LPWSAOVERLAPPED_COMPLETION_ROUTINE pCb)
{
	const int nResult = s_pWSAIoctl(s, code, pIn, nIn, pOut, nOut, pRet, pOv, pCb);
	static const GUID s_connectExGuid = WSAID_CONNECTEX;
	if (nResult == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER && pIn && nIn >= sizeof(GUID) && pOut && nOut >= sizeof(void*)
		&& !memcmp(pIn, &s_connectExGuid, sizeof(GUID)))
	{
		s_pRealConnectEx = *reinterpret_cast<LPFN_CONNECTEX*>(pOut);
		*reinterpret_cast<LPFN_CONNECTEX*>(pOut) = &Hook_ConnectEx;
	}
	return nResult;
}

static int WSAAPI Hook_sendto(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
{
	if (!Probe_IsLoopback(to))
	{
		char sz[96];
		Probe_FormatAddr(to, sz, sizeof(sz));
		Probe_Block("sendto", sz, _ReturnAddress());
		return len; // pretend it left; a refused send would make some callers spin
	}
	return s_pSendTo(s, buf, len, flags, to, tolen);
}

static int WSAAPI Hook_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD pSent, DWORD flags, const sockaddr* to, int tolen, LPWSAOVERLAPPED pOv, LPWSAOVERLAPPED_COMPLETION_ROUTINE pCb)
{
	if (!Probe_IsLoopback(to))
	{
		char sz[96];
		Probe_FormatAddr(to, sz, sizeof(sz));
		Probe_Block("WSASendTo", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return SOCKET_ERROR;
	}
	return s_pWSASendTo(s, bufs, n, pSent, flags, to, tolen, pOv, pCb);
}

static int WSAAPI Hook_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* res)
{
	if (!Probe_IsLocalHostName(node))
	{
		Probe_Block("getaddrinfo", node, _ReturnAddress());
		return EAI_NONAME;
	}
	return s_pGetAddrInfo(node, service, hints, res);
}

static int WSAAPI Hook_GetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* res)
{
	char sz[256] = {};
	if (node)
		WideCharToMultiByte(CP_UTF8, 0, node, -1, sz, sizeof(sz) - 1, NULL, NULL);
	if (!Probe_IsLocalHostName(node ? sz : nullptr))
	{
		Probe_Block("GetAddrInfoW", sz, _ReturnAddress());
		return EAI_NONAME;
	}
	return s_pGetAddrInfoW(node, service, hints, res);
}

static INT WSAAPI Hook_GetAddrInfoExW(PCWSTR node, PCWSTR service, DWORD ns, LPGUID id, const ADDRINFOEXW* hints, PADDRINFOEXW* res,
	timeval* timeout, LPOVERLAPPED pOv, LPLOOKUPSERVICE_COMPLETION_ROUTINE pCb, LPHANDLE pHandle)
{
	char sz[256] = {};
	if (node)
		WideCharToMultiByte(CP_UTF8, 0, node, -1, sz, sizeof(sz) - 1, NULL, NULL);
	if (!Probe_IsLocalHostName(node ? sz : nullptr))
	{
		Probe_Block("GetAddrInfoExW", sz, _ReturnAddress());
		return EAI_NONAME;
	}
	return s_pGetAddrInfoExW(node, service, ns, id, hints, res, timeout, pOv, pCb, pHandle);
}

static hostent* WSAAPI Hook_gethostbyname(const char* name)
{
	if (!Probe_IsLocalHostName(name))
	{
		Probe_Block("gethostbyname", name, _ReturnAddress());
		WSASetLastError(WSAHOST_NOT_FOUND);
		return nullptr;
	}
	return s_pGetHostByName(name);
}

// WinHTTP / WinINet resolve through the system resolver service, not
// getaddrinfo in this process, so they are refused at connect time.
typedef HINTERNET(WINAPI* WinHttpConnect_t)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI* InternetConnectW_t)(HINTERNET, LPCWSTR, INTERNET_PORT, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
typedef HINTERNET(WINAPI* InternetConnectA_t)(HINTERNET, LPCSTR, INTERNET_PORT, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
static WinHttpConnect_t s_pWinHttpConnect = nullptr;
static InternetConnectW_t s_pInternetConnectW = nullptr;
static InternetConnectA_t s_pInternetConnectA = nullptr;

static HINTERNET WINAPI Hook_WinHttpConnect(HINTERNET h, LPCWSTR server, INTERNET_PORT port, DWORD r)
{
	char sz[256] = {};
	if (server)
		WideCharToMultiByte(CP_UTF8, 0, server, -1, sz, sizeof(sz) - 1, NULL, NULL);
	if (!Probe_IsLocalHostName(sz))
	{
		Probe_Block("WinHttpConnect", sz, _ReturnAddress());
		SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED);
		return NULL;
	}
	return s_pWinHttpConnect(h, server, port, r);
}

static HINTERNET WINAPI Hook_InternetConnectW(HINTERNET h, LPCWSTR server, INTERNET_PORT port, LPCWSTR u, LPCWSTR p, DWORD svc, DWORD fl, DWORD_PTR ctx)
{
	char sz[256] = {};
	if (server)
		WideCharToMultiByte(CP_UTF8, 0, server, -1, sz, sizeof(sz) - 1, NULL, NULL);
	if (!Probe_IsLocalHostName(sz))
	{
		Probe_Block("InternetConnectW", sz, _ReturnAddress());
		SetLastError(ERROR_INTERNET_NAME_NOT_RESOLVED);
		return NULL;
	}
	return s_pInternetConnectW(h, server, port, u, p, svc, fl, ctx);
}

static HINTERNET WINAPI Hook_InternetConnectA(HINTERNET h, LPCSTR server, INTERNET_PORT port, LPCSTR u, LPCSTR p, DWORD svc, DWORD fl, DWORD_PTR ctx)
{
	if (!Probe_IsLocalHostName(server))
	{
		Probe_Block("InternetConnectA", server, _ReturnAddress());
		SetLastError(ERROR_INTERNET_NAME_NOT_RESOLVED);
		return NULL;
	}
	return s_pInternetConnectA(h, server, port, u, p, svc, fl, ctx);
}

//-----------------------------------------------------------------------------
// Boot observation
//-----------------------------------------------------------------------------
static LONG CALLBACK Probe_VectoredHandler(PEXCEPTION_POINTERS pInfo)
{
	const DWORD nCode = pInfo->ExceptionRecord->ExceptionCode;
	// Only faults; skip C++/debug-print/thread-naming exceptions the engine raises on purpose.
	if (nCode == 0xC0000005 || nCode == 0xC000001D || nCode == 0xC0000096 || nCode == 0xC0000094 || nCode == 0xC00000FD || nCode == 0xC0000409)
	{
		if (InterlockedIncrement(&s_nExceptions) <= 32)
		{
			char szWhere[MAX_PATH];
			Probe_ModuleOf(pInfo->ExceptionRecord->ExceptionAddress, szWhere, sizeof(szWhere));
			Probe_Log("[EXC] code=0x%08lX at %s info0=0x%llX info1=0x%llX\n", nCode, szWhere,
				static_cast<unsigned long long>(pInfo->ExceptionRecord->ExceptionInformation[0]),
				static_cast<unsigned long long>(pInfo->ExceptionRecord->ExceptionInformation[1]));
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CALLBACK Probe_EnumWindow(HWND hWnd, LPARAM lParam)
{
	DWORD nPid = 0;
	GetWindowThreadProcessId(hWnd, &nPid);
	if (nPid == GetCurrentProcessId() && IsWindowVisible(hWnd))
	{
		char szTitle[256] = {};
		char szClass[128] = {};
		GetWindowTextA(hWnd, szTitle, sizeof(szTitle));
		GetClassNameA(hWnd, szClass, sizeof(szClass));
		RECT rc = {};
		GetClientRect(hWnd, &rc);
		Probe_Log("[BOOT] window \"%s\" class=%s client=%ldx%ld\n", szTitle, szClass, rc.right, rc.bottom);
		++*reinterpret_cast<int*>(lParam);
	}
	return TRUE;
}

static DWORD WINAPI Probe_WatchThread(LPVOID)
{
	DWORD nLastMods = 0;
	for (int nSec = 1;; ++nSec)
	{
		Sleep(1000);
		HMODULE hMods[1024];
		DWORD nNeeded = 0;
		EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &nNeeded);
		const DWORD nMods = nNeeded / sizeof(HMODULE);
		if (nMods != nLastMods)
		{
			for (DWORD i = 0; i < nMods && i < 1024; ++i)
			{
				char szName[MAX_PATH];
				if (GetModuleBaseNameA(GetCurrentProcess(), hMods[i], szName, sizeof(szName)) && i >= nLastMods)
					Probe_Log("[BOOT] module %s\n", szName);
			}
			nLastMods = nMods;
		}
		if (nSec <= 10 || nSec % 10 == 0)
		{
			int nWindows = 0;
			EnumWindows(Probe_EnumWindow, reinterpret_cast<LPARAM>(&nWindows));
			PROCESS_MEMORY_COUNTERS pmc = {};
			GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
			Probe_Log("[BOOT] t=%ds windows=%d modules=%lu ws=%lluMB blocked=%ld faults=%ld\n", nSec, nWindows, nMods,
				static_cast<unsigned long long>(pmc.WorkingSetSize >> 20), s_nBlocked, s_nExceptions);
		}
	}
}

static void Probe_Attach(void)
{
	HMODULE hWinHttp = LoadLibraryW(L"winhttp.dll");
	HMODULE hWinInet = LoadLibraryW(L"wininet.dll");
	if (hWinHttp)
		s_pWinHttpConnect = reinterpret_cast<WinHttpConnect_t>(GetProcAddress(hWinHttp, "WinHttpConnect"));
	if (hWinInet)
	{
		s_pInternetConnectW = reinterpret_cast<InternetConnectW_t>(GetProcAddress(hWinInet, "InternetConnectW"));
		s_pInternetConnectA = reinterpret_cast<InternetConnectA_t>(GetProcAddress(hWinInet, "InternetConnectA"));
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(s_pConnect), Hook_connect);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pWSAConnect), Hook_WSAConnect);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pSendTo), Hook_sendto);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pWSASendTo), Hook_WSASendTo);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pGetAddrInfo), Hook_getaddrinfo);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pGetAddrInfoW), Hook_GetAddrInfoW);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pGetAddrInfoExW), Hook_GetAddrInfoExW);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pGetHostByName), Hook_gethostbyname);
	DetourAttach(&reinterpret_cast<PVOID&>(s_pWSAIoctl), Hook_WSAIoctl);
	if (s_pWinHttpConnect)
		DetourAttach(&reinterpret_cast<PVOID&>(s_pWinHttpConnect), Hook_WinHttpConnect);
	if (s_pInternetConnectW)
		DetourAttach(&reinterpret_cast<PVOID&>(s_pInternetConnectW), Hook_InternetConnectW);
	if (s_pInternetConnectA)
		DetourAttach(&reinterpret_cast<PVOID&>(s_pInternetConnectA), Hook_InternetConnectA);
	const LONG nError = DetourTransactionCommit();
	Probe_Log("[NET-GUARD] %s (detours commit %ld), winhttp=%d wininet=%d\n", nError == NO_ERROR ? "armed" : "FAILED", nError,
		s_pWinHttpConnect != nullptr, s_pInternetConnectW != nullptr);
	if (nError != NO_ERROR)
	{
		// No guard, no run: never let the client reach the network unguarded.
		Probe_Log("[NET-GUARD] terminating\n");
		TerminateProcess(GetCurrentProcess(), 0xE0E0);
	}
}

extern "C" __declspec(dllexport) void DummyExport(void)
{
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD nReason, LPVOID)
{
	if (nReason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hModule);
		s_nStartMs = GetTickCount64();
		InitializeCriticalSection(&s_logLock);

		char szPath[MAX_PATH];
		GetModuleFileNameA(NULL, szPath, MAX_PATH);
		char* pszSlash = strrchr(szPath, '\\');
		if (pszSlash)
			strcpy_s(pszSlash + 1, MAX_PATH - (pszSlash + 1 - szPath), "probe.log");
		s_pLog = _fsopen(szPath, "w", _SH_DENYNO); // readable while the client runs

		const HMODULE hExe = GetModuleHandleA(NULL);
		const IMAGE_NT_HEADERS* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
			reinterpret_cast<const uint8_t*>(hExe) + reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe)->e_lfanew);
		Probe_Log("[PROBE] attached in pid %lu, exe base %p TimeDateStamp=0x%08lX SizeOfImage=0x%lX\n", GetCurrentProcessId(), hExe,
			pNt->FileHeader.TimeDateStamp, pNt->OptionalHeader.SizeOfImage);
		Probe_Log("[PROBE] cmdline: %s\n", GetCommandLineA());

		Probe_Attach();
		AddVectoredExceptionHandler(1, Probe_VectoredHandler);
		CloseHandle(CreateThread(NULL, 0, Probe_WatchThread, NULL, 0, NULL));
	}
	else if (nReason == DLL_PROCESS_DETACH)
	{
		Probe_Log("[PROBE] detach after %.1fs, blocked=%ld faults=%ld\n", (GetTickCount64() - s_nStartMs) / 1000.0, s_nBlocked, s_nExceptions);
	}
	return TRUE;
}
