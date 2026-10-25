//=============================================================================//
//
// Purpose: loopback-only network guard for the S30 client.
//
// The S30 client tries to reach Respawn (stryder stats, fastpatch rules) and EA
// (telemetry, EOS) during boot. This project is local only, so every connect,
// send and name lookup that is not loopback is refused and logged with its
// caller. WinHTTP/WinINet resolve through the system service rather than
// getaddrinfo in this process, so they are refused at connect time.
//
//=============================================================================//
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <winhttp.h>
#include <wininet.h>
#include <intrin.h>
#include <cstdio>
#include "thirdparty/detours/include/detours.h"
#include "s30_log.h"
#include "net_guard_s30.h"

#pragma intrinsic(_ReturnAddress)

static volatile LONG s_nBlocked = 0;

//-----------------------------------------------------------------------------
// Policy
//-----------------------------------------------------------------------------
static bool NetGuard_IsLoopback(const sockaddr* pAddr)
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
	return false;
}

static bool NetGuard_IsLocalName(const char* pszName)
{
	return !pszName || !_stricmp(pszName, "localhost") || !strncmp(pszName, "127.", 4) || !strcmp(pszName, "::1");
}

static void NetGuard_FormatAddr(const sockaddr* pAddr, char* pszOut, size_t nOut)
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

// Per-destination rate limit: the first few hits of a destination are logged,
// then every 500th. Fixed table, no allocation; a hash collision merges counts.
static volatile LONG s_nHitsByKey[512] = {};

static void NetGuard_Block(const char* pszWhat, const char* pszTarget, const void* pCaller)
{
	InterlockedIncrement(&s_nBlocked);
	unsigned int nHash = 2166136261u;
	for (const char* p = pszWhat; *p; ++p)
		nHash = (nHash ^ static_cast<unsigned char>(*p)) * 16777619u;
	// Port excluded: relay pings cycle ports on the same hosts.
	for (const char* p = pszTarget ? pszTarget : ""; *p && *p != ':'; ++p)
		nHash = (nHash ^ static_cast<unsigned char>(*p)) * 16777619u;
	const LONG nHits = InterlockedIncrement(&s_nHitsByKey[nHash % 512]);
	if (nHits > 3 && nHits % 500 != 0)
		return;

	char szCaller[MAX_PATH];
	S30Log_FormatAddress(pCaller, szCaller, sizeof(szCaller));
	S30Log("[NET-GUARD] BLOCKED %s %s (caller %s)%s\n", pszWhat, pszTarget ? pszTarget : "(null)", szCaller,
		nHits == 3 ? " -- further hits of this destination are counted, not logged" : "");
}

static void NetGuard_Narrow(PCWSTR pwszIn, char* pszOut, int nOut)
{
	pszOut[0] = '\0';
	if (pwszIn)
		WideCharToMultiByte(CP_UTF8, 0, pwszIn, -1, pszOut, nOut - 1, NULL, NULL);
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------
static decltype(&connect) v_connect = connect;
static decltype(&WSAConnect) v_WSAConnect = WSAConnect;
static decltype(&sendto) v_sendto = sendto;
static decltype(&WSASendTo) v_WSASendTo = WSASendTo;
static decltype(&getaddrinfo) v_getaddrinfo = getaddrinfo;
static decltype(&GetAddrInfoW) v_GetAddrInfoW = GetAddrInfoW;
static decltype(&GetAddrInfoExW) v_GetAddrInfoExW = GetAddrInfoExW;
static decltype(&gethostbyname) v_gethostbyname = gethostbyname;
static decltype(&WSAIoctl) v_WSAIoctl = WSAIoctl;
static LPFN_CONNECTEX v_ConnectEx = nullptr;

typedef HINTERNET(WINAPI* WinHttpConnect_t)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI* InternetConnectW_t)(HINTERNET, LPCWSTR, INTERNET_PORT, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
typedef HINTERNET(WINAPI* InternetConnectA_t)(HINTERNET, LPCSTR, INTERNET_PORT, LPCSTR, LPCSTR, DWORD, DWORD, DWORD_PTR);
static WinHttpConnect_t v_WinHttpConnect = nullptr;
static InternetConnectW_t v_InternetConnectW = nullptr;
static InternetConnectA_t v_InternetConnectA = nullptr;

static int WSAAPI h_connect(SOCKET s, const sockaddr* pName, int nNameLen)
{
	if (!NetGuard_IsLoopback(pName))
	{
		char sz[96];
		NetGuard_FormatAddr(pName, sz, sizeof(sz));
		NetGuard_Block("connect", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return SOCKET_ERROR;
	}
	return v_connect(s, pName, nNameLen);
}

static int WSAAPI h_WSAConnect(SOCKET s, const sockaddr* pName, int nNameLen, LPWSABUF a, LPWSABUF b, LPQOS c, LPQOS d)
{
	if (!NetGuard_IsLoopback(pName))
	{
		char sz[96];
		NetGuard_FormatAddr(pName, sz, sizeof(sz));
		NetGuard_Block("WSAConnect", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return SOCKET_ERROR;
	}
	return v_WSAConnect(s, pName, nNameLen, a, b, c, d);
}

static BOOL PASCAL h_ConnectEx(SOCKET s, const sockaddr* pName, int nNameLen, PVOID pSend, DWORD nSend, LPDWORD pSent, LPOVERLAPPED pOv)
{
	if (!NetGuard_IsLoopback(pName))
	{
		char sz[96];
		NetGuard_FormatAddr(pName, sz, sizeof(sz));
		NetGuard_Block("ConnectEx", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return FALSE;
	}
	return v_ConnectEx(s, pName, nNameLen, pSend, nSend, pSent, pOv);
}

// ConnectEx is reached through a pointer handed out by WSAIoctl; hand out ours.
static int WSAAPI h_WSAIoctl(SOCKET s, DWORD nCode, LPVOID pIn, DWORD nIn, LPVOID pOut, DWORD nOut, LPDWORD pRet,
	LPWSAOVERLAPPED pOv, LPWSAOVERLAPPED_COMPLETION_ROUTINE pfnCompletion)
{
	const int nResult = v_WSAIoctl(s, nCode, pIn, nIn, pOut, nOut, pRet, pOv, pfnCompletion);
	static const GUID s_connectExGuid = WSAID_CONNECTEX;
	if (nResult == 0 && nCode == SIO_GET_EXTENSION_FUNCTION_POINTER && pIn && nIn >= sizeof(GUID)
		&& pOut && nOut >= sizeof(void*) && !memcmp(pIn, &s_connectExGuid, sizeof(GUID)))
	{
		v_ConnectEx = *reinterpret_cast<LPFN_CONNECTEX*>(pOut);
		*reinterpret_cast<LPFN_CONNECTEX*>(pOut) = &h_ConnectEx;
	}
	return nResult;
}

static int WSAAPI h_sendto(SOCKET s, const char* pBuf, int nLen, int nFlags, const sockaddr* pTo, int nToLen)
{
	if (!NetGuard_IsLoopback(pTo))
	{
		char sz[96];
		NetGuard_FormatAddr(pTo, sz, sizeof(sz));
		NetGuard_Block("sendto", sz, _ReturnAddress());
		return nLen; // reported as sent: a refused datagram makes some callers spin
	}
	return v_sendto(s, pBuf, nLen, nFlags, pTo, nToLen);
}

static int WSAAPI h_WSASendTo(SOCKET s, LPWSABUF pBufs, DWORD nBufs, LPDWORD pSent, DWORD nFlags, const sockaddr* pTo, int nToLen,
	LPWSAOVERLAPPED pOv, LPWSAOVERLAPPED_COMPLETION_ROUTINE pfnCompletion)
{
	if (!NetGuard_IsLoopback(pTo))
	{
		char sz[96];
		NetGuard_FormatAddr(pTo, sz, sizeof(sz));
		NetGuard_Block("WSASendTo", sz, _ReturnAddress());
		WSASetLastError(WSAENETUNREACH);
		return SOCKET_ERROR;
	}
	return v_WSASendTo(s, pBufs, nBufs, pSent, nFlags, pTo, nToLen, pOv, pfnCompletion);
}

static int WSAAPI h_getaddrinfo(PCSTR pszNode, PCSTR pszService, const ADDRINFOA* pHints, PADDRINFOA* ppResult)
{
	if (!NetGuard_IsLocalName(pszNode))
	{
		NetGuard_Block("getaddrinfo", pszNode, _ReturnAddress());
		return EAI_NONAME;
	}
	return v_getaddrinfo(pszNode, pszService, pHints, ppResult);
}

static int WSAAPI h_GetAddrInfoW(PCWSTR pwszNode, PCWSTR pwszService, const ADDRINFOW* pHints, PADDRINFOW* ppResult)
{
	char sz[256];
	NetGuard_Narrow(pwszNode, sz, sizeof(sz));
	if (!NetGuard_IsLocalName(pwszNode ? sz : nullptr))
	{
		NetGuard_Block("GetAddrInfoW", sz, _ReturnAddress());
		return EAI_NONAME;
	}
	return v_GetAddrInfoW(pwszNode, pwszService, pHints, ppResult);
}

static INT WSAAPI h_GetAddrInfoExW(PCWSTR pwszNode, PCWSTR pwszService, DWORD nNameSpace, LPGUID pNspId, const ADDRINFOEXW* pHints,
	PADDRINFOEXW* ppResult, timeval* pTimeout, LPOVERLAPPED pOv, LPLOOKUPSERVICE_COMPLETION_ROUTINE pfnCompletion, LPHANDLE pHandle)
{
	char sz[256];
	NetGuard_Narrow(pwszNode, sz, sizeof(sz));
	if (!NetGuard_IsLocalName(pwszNode ? sz : nullptr))
	{
		NetGuard_Block("GetAddrInfoExW", sz, _ReturnAddress());
		return EAI_NONAME;
	}
	return v_GetAddrInfoExW(pwszNode, pwszService, nNameSpace, pNspId, pHints, ppResult, pTimeout, pOv, pfnCompletion, pHandle);
}

static hostent* WSAAPI h_gethostbyname(const char* pszName)
{
	if (!NetGuard_IsLocalName(pszName))
	{
		NetGuard_Block("gethostbyname", pszName, _ReturnAddress());
		WSASetLastError(WSAHOST_NOT_FOUND);
		return nullptr;
	}
	return v_gethostbyname(pszName);
}

static HINTERNET WINAPI h_WinHttpConnect(HINTERNET hSession, LPCWSTR pwszServer, INTERNET_PORT nPort, DWORD nReserved)
{
	char sz[256];
	NetGuard_Narrow(pwszServer, sz, sizeof(sz));
	if (!NetGuard_IsLocalName(sz))
	{
		NetGuard_Block("WinHttpConnect", sz, _ReturnAddress());
		SetLastError(ERROR_WINHTTP_NAME_NOT_RESOLVED);
		return NULL;
	}
	return v_WinHttpConnect(hSession, pwszServer, nPort, nReserved);
}

static HINTERNET WINAPI h_InternetConnectW(HINTERNET hInternet, LPCWSTR pwszServer, INTERNET_PORT nPort, LPCWSTR pwszUser,
	LPCWSTR pwszPassword, DWORD nService, DWORD nFlags, DWORD_PTR nContext)
{
	char sz[256];
	NetGuard_Narrow(pwszServer, sz, sizeof(sz));
	if (!NetGuard_IsLocalName(sz))
	{
		NetGuard_Block("InternetConnectW", sz, _ReturnAddress());
		SetLastError(ERROR_INTERNET_NAME_NOT_RESOLVED);
		return NULL;
	}
	return v_InternetConnectW(hInternet, pwszServer, nPort, pwszUser, pwszPassword, nService, nFlags, nContext);
}

static HINTERNET WINAPI h_InternetConnectA(HINTERNET hInternet, LPCSTR pszServer, INTERNET_PORT nPort, LPCSTR pszUser,
	LPCSTR pszPassword, DWORD nService, DWORD nFlags, DWORD_PTR nContext)
{
	if (!NetGuard_IsLocalName(pszServer))
	{
		NetGuard_Block("InternetConnectA", pszServer, _ReturnAddress());
		SetLastError(ERROR_INTERNET_NAME_NOT_RESOLVED);
		return NULL;
	}
	return v_InternetConnectA(hInternet, pszServer, nPort, pszUser, pszPassword, nService, nFlags, nContext);
}

//-----------------------------------------------------------------------------
bool S30NetGuard_Install(void)
{
	const HMODULE hWinHttp = LoadLibraryW(L"winhttp.dll");
	const HMODULE hWinInet = LoadLibraryW(L"wininet.dll");
	if (hWinHttp)
		v_WinHttpConnect = reinterpret_cast<WinHttpConnect_t>(GetProcAddress(hWinHttp, "WinHttpConnect"));
	if (hWinInet)
	{
		v_InternetConnectW = reinterpret_cast<InternetConnectW_t>(GetProcAddress(hWinInet, "InternetConnectW"));
		v_InternetConnectA = reinterpret_cast<InternetConnectA_t>(GetProcAddress(hWinInet, "InternetConnectA"));
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_connect), h_connect);
	DetourAttach(&reinterpret_cast<PVOID&>(v_WSAConnect), h_WSAConnect);
	DetourAttach(&reinterpret_cast<PVOID&>(v_sendto), h_sendto);
	DetourAttach(&reinterpret_cast<PVOID&>(v_WSASendTo), h_WSASendTo);
	DetourAttach(&reinterpret_cast<PVOID&>(v_getaddrinfo), h_getaddrinfo);
	DetourAttach(&reinterpret_cast<PVOID&>(v_GetAddrInfoW), h_GetAddrInfoW);
	DetourAttach(&reinterpret_cast<PVOID&>(v_GetAddrInfoExW), h_GetAddrInfoExW);
	DetourAttach(&reinterpret_cast<PVOID&>(v_gethostbyname), h_gethostbyname);
	DetourAttach(&reinterpret_cast<PVOID&>(v_WSAIoctl), h_WSAIoctl);
	if (v_WinHttpConnect)
		DetourAttach(&reinterpret_cast<PVOID&>(v_WinHttpConnect), h_WinHttpConnect);
	if (v_InternetConnectW)
		DetourAttach(&reinterpret_cast<PVOID&>(v_InternetConnectW), h_InternetConnectW);
	if (v_InternetConnectA)
		DetourAttach(&reinterpret_cast<PVOID&>(v_InternetConnectA), h_InternetConnectA);
	const LONG nError = DetourTransactionCommit();

	S30Log("[NET-GUARD] %s (commit %ld) winhttp=%d wininet=%d\n", nError == NO_ERROR ? "armed" : "FAILED", nError,
		v_WinHttpConnect != nullptr, v_InternetConnectW != nullptr);
	return nError == NO_ERROR;
}

long S30NetGuard_BlockedCount(void)
{
	return s_nBlocked;
}
