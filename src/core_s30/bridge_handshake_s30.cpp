//=============================================================================//
//
// Purpose: S3 dedi connection handshake for the S30 client (bridge stage 1).
//
// Port of the S21 bridge handshake (engine/client/net_bridge_install.cpp
// S21Bridge_BuildS3Challenge / StartHandshake, net_bridge_process.cpp
// S21Bridge_OnS2CChallenge). The S3 dedi speaks its own connectionless
// protocol, so the bridge owns the socket:
//   C2S_CHALLENGE  ff ff ff ff 48 "connect\0" <platform user id u64>
//   S2C_CHALLENGE  ff ff ff ff 49 <challenge u32> ...
//   C2S_CONNECT    ff ff ff ff 41 <bitstream, >= 256 bytes, see below>
//   CONNACCEPT     ff ff ff ff 4A <map>\0<mode>\0
// The C2S_CONNECT field layout is the S3 dedi's and is copied as-is from the
// S21 bridge, which is proven against this dedi.
//
// After CONNACCEPT the dedi's netchannel goes to the S30 engine (bridge_signon,
// bridge_netchan).
//
//=============================================================================//
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "s30_log.h"
#include "bridge_handshake_s30.h"
#include "bridge_signon_s30.h"
#include "bridge_datablock_s30.h"
#include "bridge_netchan_s30.h"
#include "console_s30.h"
#include "concommand_s30.h"

#define HOST_PROOF_MAPPING_FMT "Local\\R5F_HostProof_%d" // engine/shared/host_proof.h
#define HOST_PROOF_CONVAR      "cl_hostProof"
#define HOST_PROOF_HEX_LEN     64

enum class BridgeStage_t : int
{
	IDLE = 0,
	CHALLENGE_SENT,
	CONNECT_SENT,
	ACCEPTED,
};

static volatile LONG s_eStage = static_cast<LONG>(BridgeStage_t::IDLE);
static SOCKET s_hSocket = INVALID_SOCKET;
static sockaddr_in6 s_dest = {};
static HANDLE s_hPollThread = NULL;
static volatile LONG s_bStopPoll = 0;
static ULONGLONG s_nStageDeadline = 0;
static char s_szAcceptedMap[64] = {};
static char s_szAcceptedMode[64] = {};
static volatile LONG s_nNetchanPackets = 0;
static volatile LONG s_nBadFragments = 0;

static const uint64_t s_nPlatformUserId = 9990000; // the offline Nucleus id (identity_s30)
static const char s_szPersona[] = "s30player";

const char* S30Bridge_StateName(void)
{
	switch (static_cast<BridgeStage_t>(s_eStage))
	{
	case BridgeStage_t::CHALLENGE_SENT: return "challenge sent";
	case BridgeStage_t::CONNECT_SENT:   return "connect sent";
	case BridgeStage_t::ACCEPTED:       return "accepted";
	default:                            return "idle";
	}
}

static int Bridge_Send(const uint8_t* pData, int nLen)
{
	return sendto(s_hSocket, reinterpret_cast<const char*>(pData), nLen, 0, reinterpret_cast<const sockaddr*>(&s_dest), sizeof(s_dest));
}

// The local dedi publishes a host proof in a named mapping; only ever sent to a
// loopback peer (same rule as the S21 bridge).
static bool Bridge_ReadHostProof(char (&szProof)[HOST_PROOF_HEX_LEN + 1])
{
	szProof[0] = '\0';
	static const uint8_t s_v4Mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
	const uint8_t* const pAddr = s_dest.sin6_addr.u.Byte;
	const bool bLoopback = (!memcmp(pAddr, s_v4Mapped, sizeof(s_v4Mapped)) && pAddr[12] == 127) || IN6_IS_ADDR_LOOPBACK(&s_dest.sin6_addr);
	if (!bLoopback)
		return false;

	char szName[64];
	snprintf(szName, sizeof(szName), HOST_PROOF_MAPPING_FMT, static_cast<int>(ntohs(s_dest.sin6_port)));
	const HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, szName);
	if (!hMap)
		return false;
	const char* const pView = static_cast<const char*>(MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, HOST_PROOF_HEX_LEN + 1));
	if (pView)
	{
		memcpy(szProof, pView, HOST_PROOF_HEX_LEN);
		szProof[HOST_PROOF_HEX_LEN] = '\0';
		UnmapViewOfFile(pView);
	}
	CloseHandle(hMap);
	return strlen(szProof) == HOST_PROOF_HEX_LEN;
}

//-----------------------------------------------------------------------------
// C2S_CONNECT bitstream (S3 dedi layout)
//-----------------------------------------------------------------------------
struct BitWriter_t
{
	uint32_t dwords[1152 / 4];
	int nBitPos;
	bool bOverflow;

	void Bits(uint64_t nValue, int nBits)
	{
		if (bOverflow || nBits <= 0)
			return;
		if (nBitPos + nBits > static_cast<int>(sizeof(dwords) * 8))
		{
			bOverflow = true;
			return;
		}
		uint64_t v = nValue & (nBits >= 64 ? ~0ULL : ((1ULL << nBits) - 1));
		int nRemaining = nBits;
		while (nRemaining > 0)
		{
			const int nIdx = nBitPos / 32;
			const int nOff = nBitPos % 32;
			const int nTake = nRemaining < 32 - nOff ? nRemaining : 32 - nOff;
			const uint32_t nMask = nTake >= 32 ? 0xFFFFFFFFu : ((1u << nTake) - 1);
			dwords[nIdx] |= static_cast<uint32_t>(v & nMask) << nOff;
			nBitPos += nTake;
			v >>= nTake;
			nRemaining -= nTake;
		}
	}
	void U8(uint8_t v) { Bits(v, 8); }
	void U32(uint32_t v) { Bits(v, 32); }
	void U64(uint64_t v) { Bits(v & 0xFFFFFFFF, 32); Bits(v >> 32, 32); }
	void Str(const char* psz)
	{
		for (const char* p = psz ? psz : ""; *p; ++p)
			U8(static_cast<uint8_t>(*p));
		U8(0);
	}
};

static void Bridge_OnS2CChallenge(uint32_t nChallenge)
{
	S30Log("[BRIDGE] S2C_CHALLENGE received, challenge=0x%08X\n", nChallenge);

	static BitWriter_t s_bw;
	memset(&s_bw, 0, sizeof(s_bw));
	BitWriter_t& bw = s_bw;

	bw.U32(529);                          // protocol
	bw.U32(2001);                         // authChallenge
	bw.U32(nChallenge);                   // challenge echo
	bw.U32(0);                            // reservation
	bw.U8(2);                             // authProtocol (< 8)
	bw.U64(s_nPlatformUserId);            // platform user id (Nucleus)
	bw.Str(s_szPersona);                  // persona name
	bw.Str("");                           // password tag (no sv_password)
	bw.U64(0);                            // nonce1
	bw.U64(0);                            // nonce2
	bw.U32(0);                            // clientToken
	bw.U64(0xA28012AACDFB8F7EULL);        // sendtableHash: CRC64 the S3 dedi expects
	bw.U64(0);                            // signature blob (64 bits)
	static const uint8_t s_nDlcCount = 31; // must equal the dedi's entitlement slot count
	bw.U8(s_nDlcCount);
	for (int i = 0; i < s_nDlcCount; ++i)
		bw.Bits(0, 16);
	bw.Bits(0, 1); bw.Bits(0, 1); bw.Bits(0, 1); bw.Bits(0, 1); // flags
	bw.Bits(1, 1);                        // hasAuthString
	bw.Str("");                           // authString
	bw.U32(74565);                        // marker 0x12345

	char szHostProof[HOST_PROOF_HEX_LEN + 1];
	const bool bHostProof = Bridge_ReadHostProof(szHostProof);
	bw.U8(bHostProof ? 6 : 5);            // plugin/convar count; each: 6-bit name index (0 = inline), name, value
	bw.Bits(0, 6); bw.Str("nucleus_id"); bw.Str("0");
	bw.Bits(0, 6); bw.Str("nucleus_pid"); bw.Str("0");
	bw.Bits(0, 6); bw.Str("cl_onlineAuthToken"); bw.Str("");
	bw.Bits(0, 6); bw.Str("cl_onlineAuthTokenSignature1"); bw.Str("");
	bw.Bits(0, 6); bw.Str("cl_onlineAuthTokenSignature2"); bw.Str("");
	if (bHostProof)
	{
		bw.Bits(0, 6); bw.Str(HOST_PROOF_CONVAR); bw.Str(szHostProof);
		SecureZeroMemory(szHostProof, sizeof(szHostProof));
	}
	if (bw.bOverflow)
	{
		S30Log("[BRIDGE] C2S_CONNECT bitstream overflow -- connect abandoned\n");
		return;
	}

	int nBytes = (bw.nBitPos + 7) / 8;
	if (nBytes < 256)
		nBytes = 256; // the dedi requires at least 256 payload bytes
	static uint8_t s_packet[5 + 1152];
	memset(s_packet, 0, sizeof(s_packet));
	s_packet[0] = s_packet[1] = s_packet[2] = s_packet[3] = 0xFF;
	s_packet[4] = 0x41; // C2S_CONNECT
	memcpy(s_packet + 5, bw.dwords, nBytes);
	const int nSent = Bridge_Send(s_packet, 5 + nBytes);
	S30Log("[BRIDGE] C2S_CONNECT sent %d/%d bytes (host proof %s)\n", nSent, 5 + nBytes, bHostProof ? "yes" : "no");
	InterlockedExchange(&s_eStage, static_cast<LONG>(BridgeStage_t::CONNECT_SENT));
	s_nStageDeadline = GetTickCount64() + 15000;
}

static void Bridge_OnConnAccept(const uint8_t* pBody, int nBody)
{
	s_szAcceptedMap[0] = s_szAcceptedMode[0] = '\0';
	int n = 0;
	while (n < nBody && n < static_cast<int>(sizeof(s_szAcceptedMap)) - 1 && pBody[n])
	{
		s_szAcceptedMap[n] = static_cast<char>(pBody[n]);
		++n;
	}
	s_szAcceptedMap[n] = '\0';
	int m = 0;
	for (int i = n + 1; i < nBody && m < static_cast<int>(sizeof(s_szAcceptedMode)) - 1 && pBody[i]; ++i)
		s_szAcceptedMode[m++] = static_cast<char>(pBody[i]);
	s_szAcceptedMode[m] = '\0';
	InterlockedExchange(&s_eStage, static_cast<LONG>(BridgeStage_t::ACCEPTED));
	S30Log("[BRIDGE] CONNACCEPT: map '%s' mode '%s' -- the dedi has the client as connected\n", s_szAcceptedMap, s_szAcceptedMode);
	S30Signon_OnConnAccept(s_szAcceptedMap);
	S30Netchan_OnConnAccept();
}

static DWORD WINAPI Bridge_PollThread(LPVOID)
{
	static uint8_t s_buf[65536];
	while (!s_bStopPoll)
	{
		S30Netchan_Pump();
		sockaddr_in6 from = {};
		int nFromLen = sizeof(from);
		const int nRecv = recvfrom(s_hSocket, reinterpret_cast<char*>(s_buf), sizeof(s_buf), 0, reinterpret_cast<sockaddr*>(&from), &nFromLen);
		if (nRecv <= 0)
		{
			const BridgeStage_t eStage = static_cast<BridgeStage_t>(s_eStage);
			if ((eStage == BridgeStage_t::CHALLENGE_SENT || eStage == BridgeStage_t::CONNECT_SENT) && GetTickCount64() > s_nStageDeadline)
			{
				S30Log("[BRIDGE] handshake timed out in stage '%s'\n", S30Bridge_StateName());
				InterlockedExchange(&s_eStage, static_cast<LONG>(BridgeStage_t::IDLE));
			}
			Sleep(2);
			continue;
		}

		const bool bOob = nRecv >= 5 && *reinterpret_cast<const uint32_t*>(s_buf) == 0xFFFFFFFFu;
		if (bOob && s_buf[4] == 0x49 && nRecv >= 9 && s_eStage == static_cast<LONG>(BridgeStage_t::CHALLENGE_SENT))
		{
			uint32_t nChallenge = 0;
			memcpy(&nChallenge, s_buf + 5, sizeof(nChallenge));
			Bridge_OnS2CChallenge(nChallenge);
		}
		else if (bOob && s_buf[4] == 0x4A && s_eStage == static_cast<LONG>(BridgeStage_t::CONNECT_SENT))
		{
			Bridge_OnConnAccept(s_buf + 5, nRecv - 5);
		}
		else if (bOob && s_buf[4] == 0x4F && s_eStage == static_cast<LONG>(BridgeStage_t::ACCEPTED))
		{
			if (!S30DataBlock_OnFragment(s_buf, nRecv))
			{
				const LONG n = InterlockedIncrement(&s_nBadFragments);
				if (n <= 5)
					S30Log("[BRIDGE] malformed DataBlock fragment (%d bytes, #%ld)\n", nRecv, n);
			}
			if (S30DataBlock_TakeWake())
				S30Netchan_WakeForDataBlock();
		}
		else if (bOob && s_buf[4] == 0x4A)
		{
			// CONNACCEPT repeats until the dedi sees the client's signon traffic.
		}
		else if (bOob)
		{
			char szText[96] = {};
			for (int i = 5; i < nRecv && i < 5 + 80; ++i)
				szText[i - 5] = (s_buf[i] >= 0x20 && s_buf[i] < 0x7F) ? static_cast<char>(s_buf[i]) : '.';
			S30Log("[BRIDGE] OOB 0x%02X len=%d in stage '%s': %s\n", s_buf[4], nRecv, S30Bridge_StateName(), szText);
		}
		else if (s_eStage == static_cast<LONG>(BridgeStage_t::ACCEPTED))
		{
			// Netchannel traffic from the dedi: to the engine socket, where the
			// ProcessPacket detour (bridge_netchan) parses it.
			const LONG nCount = InterlockedIncrement(&s_nNetchanPackets);
			// Split fragment: [-2][32 request][8 count][8 number][16 split size][payload];
			// the engine reassembles it (NET_ReceiveSplit 0x2A5650, split size 564..1188).
			if (nRecv >= 12 && *reinterpret_cast<const uint32_t*>(s_buf) == 0xFFFFFFFEu)
			{
				static volatile LONG s_nSplitFragments = 0;
				const LONG n = InterlockedIncrement(&s_nSplitFragments);
				if (n <= 8 || n % 500 == 0)
					S30Log("[BRIDGE] split fragment #%ld: request %d, %u/%u, split size %d, %d bytes\n", n,
						*reinterpret_cast<const int32_t*>(s_buf + 4), s_buf[9], s_buf[8],
						*reinterpret_cast<const int16_t*>(s_buf + 10), nRecv);
			}
			if (!S30Signon_QueueForEngine(s_buf, nRecv) && nCount <= 20)
				S30Log("[BRIDGE] netchan packet #%ld (%d bytes) not queued for the engine\n", nCount, nRecv);
		}
		else
		{
			// Netchannel traffic before CONNACCEPT: not routed.
			// S3 header: [32 seq][32 ack][8 flags] (S21 bridge S21Bridge_Hook_ProcessPacket).
			const LONG nCount = InterlockedIncrement(&s_nNetchanPackets);
			if ((nCount <= 20 || nCount % 500 == 0) && nRecv >= 9)
			{
				int32_t nSeq, nAck;
				memcpy(&nSeq, s_buf, 4);
				memcpy(&nAck, s_buf + 4, 4);
				S30LogFile("[BRIDGE] netchan packet #%ld len=%d seq=%d ack=%d flags=0x%02X (not yet consumed)\n",
					nCount, nRecv, nSeq, nAck, s_buf[8]);
			}
		}
	}
	return 0;
}

// The engine `connect` the bridge itself queues after the handshake: the wrapped
// engine command lets these through (S30Bridge_TakeInternalConnect).
static volatile LONG s_nInternalConnects = 0;

bool S30Bridge_TakeInternalConnect(void)
{
	LONG n = s_nInternalConnects;
	while (n > 0)
	{
		const LONG nSeen = InterlockedCompareExchange(&s_nInternalConnects, n - 1, n);
		if (nSeen == n)
			return true;
		n = nSeen;
	}
	return false;
}

bool S30Bridge_Connect(const char* pszHostPort)
{
	// A new connect replaces a live or half-done bridge (S21 Hook_Connect_Worker).
	if (s_eStage != static_cast<LONG>(BridgeStage_t::IDLE))
	{
		S30Log("[BRIDGE] %s -- tearing it down for the new connect\n", S30Bridge_StateName());
		S30Bridge_Disconnect();
	}

	char szHost[128];
	strncpy_s(szHost, pszHostPort, _TRUNCATE);
	int nPort = 37015;
	char* const pszColon = strrchr(szHost, ':');
	if (pszColon && !strchr(pszColon + 1, ']'))
	{
		nPort = atoi(pszColon + 1);
		*pszColon = '\0';
	}
	sockaddr_in addr4 = {};
	if (!_stricmp(szHost, "localhost"))
		strcpy_s(szHost, "127.0.0.1");
	if (inet_pton(AF_INET, szHost, &addr4.sin_addr) != 1)
	{
		// A host name: its IPv4 address (the network guard decides what may resolve).
		addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		addrinfo* pResult = nullptr;
		if (getaddrinfo(szHost, nullptr, &hints, &pResult) == 0 && pResult)
		{
			addr4.sin_addr = reinterpret_cast<const sockaddr_in*>(pResult->ai_addr)->sin_addr;
			inet_ntop(AF_INET, &addr4.sin_addr, szHost, sizeof(szHost));
			freeaddrinfo(pResult);
		}
		else
		{
			S30Log("[BRIDGE] cannot resolve '%s'\n", szHost);
			return false;
		}
	}
	if (nPort <= 0 || nPort > 65535)
	{
		S30Log("[BRIDGE] cannot parse '%s' (want host:port)\n", pszHostPort);
		return false;
	}
	memset(&s_dest, 0, sizeof(s_dest));
	s_dest.sin6_family = AF_INET6;
	s_dest.sin6_port = htons(static_cast<u_short>(nPort));
	s_dest.sin6_addr.u.Byte[10] = 0xFF;
	s_dest.sin6_addr.u.Byte[11] = 0xFF;
	memcpy(&s_dest.sin6_addr.u.Byte[12], &addr4.sin_addr, 4);

	if (s_hSocket == INVALID_SOCKET)
	{
		s_hSocket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
		if (s_hSocket == INVALID_SOCKET)
		{
			S30Log("[BRIDGE] socket() failed (%d)\n", WSAGetLastError());
			return false;
		}
		DWORD nV6Only = 0;
		setsockopt(s_hSocket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&nV6Only), sizeof(nV6Only));
		u_long nNonBlocking = 1;
		ioctlsocket(s_hSocket, FIONBIO, &nNonBlocking);
	}
	if (!s_hPollThread)
	{
		s_bStopPoll = 0;
		s_hPollThread = CreateThread(NULL, 0, Bridge_PollThread, NULL, 0, NULL);
	}

	// Stage 2: the engine connects natively to the same address (server address
	// and signon 1 in CClientState); its own datagrams to the dedi are dropped and
	// the dedi's connection is handed to it on CONNACCEPT (bridge_signon_s30).
	// net_usesocketsforloopback 1: without it a 127.0.0.1 connect never touches a
	// socket (DEVELOPMENTONLY in S30.1). cl_strict_transport_checks 0: the
	// connectionless wrapper (0x3F2FB0) otherwise drops a reply that did not come
	// through DTLS; it is read nowhere else. Both are restored on disconnect.
	S30Signon_Begin(s_dest, s_hSocket);
	S30DataBlock_Reset(Bridge_Send);
	S30Netchan_Begin(Bridge_Send);
	static const char* const s_pszBridgeVars[] = { "net_usesocketsforloopback", "cl_strict_transport_checks" };
	for (const char* pszVar : s_pszBridgeVars)
	{
		const int nOld = S30ConCommand_ClearFlags(pszVar, 0x2 /*DEVELOPMENTONLY*/ | 0x10 /*HIDDEN*/);
		S30Log("[BRIDGE] %s: flags 0x%X%s\n", pszVar, nOld, nOld < 0 ? " (not found)" : "");
	}
	char szConnect[192];
	snprintf(szConnect, sizeof(szConnect),
		"net_usesocketsforloopback 1; cl_strict_transport_checks 0; connect %s:%d", szHost, nPort);
	InterlockedIncrement(&s_nInternalConnects);
	if (!S30Console_SendToEngine(szConnect))
	{
		InterlockedDecrement(&s_nInternalConnects);
		S30Log("[BRIDGE] Cbuf_AddText unresolved -- native connect not started\n");
	}

	uint8_t challenge[21];
	challenge[0] = challenge[1] = challenge[2] = challenge[3] = 0xFF;
	challenge[4] = 0x48; // C2S_CHALLENGE
	memcpy(challenge + 5, "connect", 8);
	memcpy(challenge + 13, &s_nPlatformUserId, 8);
	s_nStageDeadline = GetTickCount64() + 15000;
	InterlockedExchange(&s_eStage, static_cast<LONG>(BridgeStage_t::CHALLENGE_SENT));
	const int nSent = Bridge_Send(challenge, sizeof(challenge));
	S30Log("[BRIDGE] C2S_CHALLENGE sent %d/%d bytes to %s:%d\n", nSent, static_cast<int>(sizeof(challenge)), szHost, nPort);
	return nSent == static_cast<int>(sizeof(challenge));
}

// Local reset only: the dedi times the client out on its own (no C2S
// net_Disconnect yet).
void S30Bridge_Disconnect(void)
{
	InterlockedExchange(&s_eStage, static_cast<LONG>(BridgeStage_t::IDLE));
	S30Netchan_End();
	S30Signon_End();
	S30Console_SendToEngine("disconnect; cl_strict_transport_checks 1; net_usesocketsforloopback 0");
	S30Log("[BRIDGE] reset to idle\n");
}
