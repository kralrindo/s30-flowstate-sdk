//=============================================================================//
//
// Purpose: S3 dedi connection, stage 2: hand the dedi to the S30 engine.
//
// S30.1 facts (s30/docs/12, verified in the S30 IDB):
//   0x2A5A90  NET_ReceiveDatagram: recvfrom on the socket, sender (sockaddr_in6)
//             copied into the netpacket (+0 type 3, +4 address, +20 port),
//             then NET_DecodeDatagram.
//   0x2A5870  NET_DecodeDatagram(sock, packet, bEncrypted, buf, len, pDrop):
//             bEncrypted set -> 28-byte header decrypted, else copied as is.
//             NET_ProcessSocket passes 1 for the client sockets unless a Valve
//             relay runs with valve_relay_encrypt_traffic 0.
//   0x3EF530  connectionless dispatcher, case 12 (S21 case 4): needs signon 1
//             (CClientState+196) and the sender == the server address
//             (+136 type, +140 address, +156 port, +159 flag byte); reads three
//             strings and a bit, runs FullConnect (0x2FEDF0: NET_CreateNetChannel
//             "ClientToServer", netchannel at +104) and starts the level load.
//   0x1E1BD60 client state array (stride 0x69378), vtable 0x17F1828.
//
//=============================================================================//
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include "bridge_signon_s30.h"
#include "bridge_netchan_s30.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "thirdparty/detours/include/detours.h"

#include "s30_log.h"
#include "s30_pattern.h"

// NET_DecodeDatagram prologue up to its argument moves (unique in S30.1).
static const char* const s_pszPatternDecodeDatagram =
	"48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 56 48 81 EC 80 00 00 00 "
	"48 63 BC 24 C0 00 00 00 4D 8B F1 41 0F B6 E8";
// connect handler: imul rcx, 0x69378 ; lea rax, [g_ClientStates] ; add rcx, rax ; call
static const char* const s_pszPatternClientStates =
	"48 63 C8 33 D2 48 69 C9 78 93 06 00 48 8D 05 ?? ?? ?? ?? 48 03 C8 E8";
static const int s_nClientStatesDispOffset = 15; // disp32 of the lea
static const int s_nClientStatesNextInsn = 19;
static const uintptr_t s_nClientStateVtableRva = 0x17F1828;

// CClientState fields (S30.1)
static const int CL_NETCHAN = 104;
static const int CL_SERVER_ADR_TYPE = 136;
static const int CL_SERVER_ADR = 140;     // 16 bytes
static const int CL_SERVER_PORT = 156;    // network order
static const int CL_SERVER_ADR_FLAG = 159;
static const int CL_SIGNON = 196;
static const int CL_LEVEL_FILE = 412;

// netpacket sender
static const int PKT_ADR_TYPE = 0;
static const int PKT_ADR = 4;
static const int PKT_PORT = 20;
static const int NA_IP = 3;

typedef char (*NET_DecodeDatagram_t)(unsigned int nSock, uint8_t* pPacket, char bEncrypted, void* pBuf, int nLen, uint8_t* pDrop);
static NET_DecodeDatagram_t v_NET_DecodeDatagram = nullptr;
static uintptr_t s_pClientStates = 0;

static volatile LONG s_bActive = 0;
static sockaddr_in6 s_dest = {};
static volatile uintptr_t s_hEngineSocket = INVALID_SOCKET;
static uintptr_t s_hBridgeSocket = INVALID_SOCKET;
static volatile LONG s_nDroppedSends = 0;
static volatile LONG s_nPlaintextPackets = 0;

// Engine-bound datagrams (the type-12 reply, then the dedi's netchannel packets).
// NET_ReceiveDatagram (0x2A5A90) receives into 1264 bytes; longer ones are dropped.
static const int kEngineRecvMax = 1264;
static const int kQueueSlots = 128;
struct QueuedDatagram_t
{
	int nLen;
	uint8_t data[kEngineRecvMax];
};
static SRWLOCK s_queueLock = SRWLOCK_INIT;
static QueuedDatagram_t s_queue[kQueueSlots];
static int s_nQueueHead = 0;
static volatile LONG s_nQueued = 0;
static volatile LONG s_nQueueDrops = 0;

static bool Signon_Enqueue(const uint8_t* pData, int nLen)
{
	if (nLen <= 0 || nLen > kEngineRecvMax)
	{
		const LONG n = InterlockedIncrement(&s_nQueueDrops);
		if (n <= 5)
			S30Log("[SIGNON] datagram of %d bytes does not fit the engine's receive buffer -- dropped (#%ld)\n", nLen, n);
		return false;
	}
	bool bOk = false;
	AcquireSRWLockExclusive(&s_queueLock);
	if (s_nQueued < kQueueSlots)
	{
		QueuedDatagram_t& slot = s_queue[(s_nQueueHead + s_nQueued) % kQueueSlots];
		memcpy(slot.data, pData, nLen);
		slot.nLen = nLen;
		++s_nQueued;
		bOk = true;
	}
	ReleaseSRWLockExclusive(&s_queueLock);
	if (!bOk)
	{
		const LONG n = InterlockedIncrement(&s_nQueueDrops);
		if (n <= 5 || n % 100 == 0)
			S30Log("[SIGNON] engine queue full -- datagram dropped (#%ld)\n", n);
	}
	return bOk;
}

static bool Signon_SameAddr(const sockaddr* pAddr, int nLen)
{
	if (!pAddr)
		return false;
	if (pAddr->sa_family == AF_INET6 && nLen >= static_cast<int>(sizeof(sockaddr_in6)))
	{
		const auto* p6 = reinterpret_cast<const sockaddr_in6*>(pAddr);
		return p6->sin6_port == s_dest.sin6_port && !memcmp(&p6->sin6_addr, &s_dest.sin6_addr, sizeof(in6_addr));
	}
	if (pAddr->sa_family == AF_INET && nLen >= static_cast<int>(sizeof(sockaddr_in)))
	{
		const auto* p4 = reinterpret_cast<const sockaddr_in*>(pAddr);
		return p4->sin_port == s_dest.sin6_port && !memcmp(&p4->sin_addr, &s_dest.sin6_addr.u.Byte[12], 4);
	}
	return false;
}

static bool Signon_PacketFromDest(const uint8_t* pPacket)
{
	return *reinterpret_cast<const int*>(pPacket + PKT_ADR_TYPE) == NA_IP
		&& !memcmp(pPacket + PKT_ADR, &s_dest.sin6_addr, sizeof(in6_addr))
		&& *reinterpret_cast<const uint16_t*>(pPacket + PKT_PORT) == s_dest.sin6_port;
}

static char h_NET_DecodeDatagram(unsigned int nSock, uint8_t* pPacket, char bEncrypted, void* pBuf, int nLen, uint8_t* pDrop)
{
	if (s_bActive && bEncrypted && nLen > 0 && Signon_PacketFromDest(pPacket))
	{
		const LONG n = InterlockedIncrement(&s_nPlaintextPackets);
		if (n <= 5)
			S30Log("[SIGNON] datagram from the dedi taken as plaintext (sock %u, %d bytes, #%ld)\n", nSock, nLen, n);
		bEncrypted = 0;
	}
	return v_NET_DecodeDatagram(nSock, pPacket, bEncrypted, pBuf, nLen, pDrop);
}

bool S30Signon_FilterSendTo(uintptr_t s, const char* pBuf, int nLen, const sockaddr* pTo, int nToLen)
{
	// The bridge's own sends to the dedi pass through this hook too.
	if (!s_bActive || s == s_hBridgeSocket || !Signon_SameAddr(pTo, nToLen))
		return false;
	if (s_hEngineSocket != s)
	{
		s_hEngineSocket = s;
		S30Log("[SIGNON] engine socket to the dedi: %llu\n", static_cast<unsigned long long>(s));
	}
	const LONG n = InterlockedIncrement(&s_nDroppedSends);
	if (n <= 5 || n % 200 == 0)
	{
		char szHex[16 * 3 + 1] = {};
		for (int i = 0; i < nLen && i < 16; ++i)
			snprintf(szHex + i * 3, 4, "%02X ", static_cast<unsigned char>(pBuf[i]));
		S30Log("[SIGNON] engine datagram to the dedi dropped (%d bytes, #%ld): %s\n", nLen, n, szHex);
	}
	S30Netchan_OnEngineSend(); // the bridge's own C2S goes in its place
	return true;
}

int S30Signon_InjectRecv(uintptr_t s, char* pBuf, int nLen, sockaddr* pFrom, int* pnFromLen)
{
	if (!s_bActive || s != s_hEngineSocket || !s_nQueued)
		return 0;
	AcquireSRWLockExclusive(&s_queueLock);
	int nCopied = 0;
	if (s_nQueued)
	{
		const QueuedDatagram_t& slot = s_queue[s_nQueueHead];
		if (slot.nLen <= nLen)
		{
			memcpy(pBuf, slot.data, slot.nLen);
			nCopied = slot.nLen;
		}
		s_nQueueHead = (s_nQueueHead + 1) % kQueueSlots;
		--s_nQueued;
	}
	ReleaseSRWLockExclusive(&s_queueLock);
	if (!nCopied)
		return 0;
	if (pFrom && pnFromLen && *pnFromLen >= static_cast<int>(sizeof(sockaddr_in6)))
	{
		memcpy(pFrom, &s_dest, sizeof(sockaddr_in6));
		*pnFromLen = sizeof(sockaddr_in6);
	}
	static volatile LONG s_nDelivered = 0;
	const LONG n = InterlockedIncrement(&s_nDelivered);
	if (n <= 3 || n % 1000 == 0)
		S30Log("[SIGNON] delivered a %d-byte datagram to the engine socket (#%ld)\n", nCopied, n);
	return nCopied;
}

void S30Signon_Begin(const sockaddr_in6& dest, uintptr_t hBridgeSocket)
{
	s_dest = dest;
	s_hBridgeSocket = hBridgeSocket;
	s_hEngineSocket = INVALID_SOCKET;
	s_nQueued = 0;
	s_nQueueHead = 0;
	InterlockedExchange(&s_nDroppedSends, 0);
	InterlockedExchange(&s_nPlaintextPackets, 0);
	InterlockedExchange(&s_bActive, 1);
}

void S30Signon_End(void)
{
	InterlockedExchange(&s_bActive, 0);
	s_hEngineSocket = INVALID_SOCKET;
	s_nQueued = 0;
}

void S30Signon_OnConnAccept(const char* pszMap)
{
	// ffffffff 0C <map>\0 <string>\0 <string>\0 <bit> -- the two other strings
	// empty and the bit clear, as the S21 bridge sends its type 4.
	const size_t nMap = strnlen(pszMap, 63);
	uint8_t reply[5 + 64 + 3];
	memset(reply, 0, sizeof(reply));
	reply[0] = reply[1] = reply[2] = reply[3] = 0xFF;
	reply[4] = 0x0C;
	memcpy(reply + 5, pszMap, nMap);
	Signon_Enqueue(reply, static_cast<int>(5 + nMap + 1 + 1 + 1 + 1));
	S30Log("[SIGNON] queued native server-map reply (type 12, map '%.*s') for the engine socket %s\n",
		static_cast<int>(nMap), pszMap, s_hEngineSocket != INVALID_SOCKET ? "(known)" : "(not seen yet)");
}

bool S30Signon_QueueForEngine(const uint8_t* pData, int nLen)
{
	return s_bActive && Signon_Enqueue(pData, nLen);
}

bool S30Signon_IsDestNetadr(const uint8_t* pNetadr)
{
	return s_bActive && pNetadr && Signon_PacketFromDest(pNetadr);
}

void S30Signon_PrintClientState(void)
{
	if (!s_pClientStates)
	{
		S30Log("client state array unresolved\n");
		return;
	}
	const uint8_t* const cl = reinterpret_cast<const uint8_t*>(s_pClientStates);
	const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(cl) - S30Image_Base();
	char szAdr[3 * 16 + 1] = {};
	for (int i = 0; i < 16; ++i)
		snprintf(szAdr + i * 3, 4, "%02X ", cl[CL_SERVER_ADR + i]);
	S30Log("client state 0 @ 0x%llX: vtable rva 0x%llX%s signon %d netchan %p\n",
		static_cast<unsigned long long>(s_pClientStates), static_cast<unsigned long long>(vt),
		vt == s_nClientStateVtableRva ? "" : " (UNEXPECTED)",
		*reinterpret_cast<const int*>(cl + CL_SIGNON), *reinterpret_cast<void* const*>(cl + CL_NETCHAN));
	S30Log("  server adr type %d, %s port %u flag %u, level '%.64s'\n",
		*reinterpret_cast<const int*>(cl + CL_SERVER_ADR_TYPE), szAdr,
		ntohs(*reinterpret_cast<const uint16_t*>(cl + CL_SERVER_PORT)), cl[CL_SERVER_ADR_FLAG],
		reinterpret_cast<const char*>(cl + CL_LEVEL_FILE));
	S30Log("  bridge routes %s, engine socket %lld, dropped sends %ld, plaintext packets %ld\n",
		s_bActive ? "on" : "off", s_hEngineSocket == INVALID_SOCKET ? -1LL : static_cast<long long>(s_hEngineSocket),
		s_nDroppedSends, s_nPlaintextPackets);
}

bool S30Signon_Install(void)
{
	int nMatches = 0;
	uint8_t* const pDecode = S30Pattern_FindUnique(s_pszPatternDecodeDatagram, &nMatches);
	if (!pDecode)
	{
		S30Log("[SIGNON] NET_DecodeDatagram unresolved (matches=%d) -- stage 2 off\n", nMatches);
		return false;
	}
	uint8_t* const pRef = S30Pattern_FindUnique(s_pszPatternClientStates, &nMatches);
	if (pRef)
	{
		const int32_t nDisp = *reinterpret_cast<const int32_t*>(pRef + s_nClientStatesDispOffset);
		// Early init runs before the host's static constructors, so the vtable is
		// only checked when the client state is read (PrintClientState).
		s_pClientStates = reinterpret_cast<uintptr_t>(pRef) + s_nClientStatesNextInsn + nDisp;
	}
	else
		S30Log("[SIGNON] client state array unresolved (matches=%d)\n", nMatches);

	v_NET_DecodeDatagram = reinterpret_cast<NET_DecodeDatagram_t>(pDecode);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_NET_DecodeDatagram), h_NET_DecodeDatagram);
	const LONG nError = DetourTransactionCommit();
	S30Log("[SIGNON] NET_DecodeDatagram @ 0x%llX %s (commit %ld), client states @ 0x%llX\n",
		static_cast<unsigned long long>(S30Pattern_Rva(pDecode)), nError == NO_ERROR ? "hooked" : "FAILED", nError,
		static_cast<unsigned long long>(s_pClientStates ? s_pClientStates - S30Image_Base() : 0));
	return nError == NO_ERROR;
}
