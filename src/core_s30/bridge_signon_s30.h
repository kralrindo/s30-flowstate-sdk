#ifndef CORE_S30_BRIDGE_SIGNON_H
#define CORE_S30_BRIDGE_SIGNON_H
//=============================================================================//
//
// Purpose: S3 dedi connection, stage 2: hand the dedi to the S30 engine.
//
// The engine runs its native connect to the dedi's address (server address and
// signon 1 in CClientState); its own encrypted connectionless sends to the dedi
// are dropped, the bridge speaks S3 on its own socket. On CONNACCEPT the engine
// gets its native "server map" reply (connectionless type 12) as if from the
// dedi: that handler runs FullConnect (the netchannel) and starts the level
// load. See s30/docs/12.
//
// No winsock include here (callers include <windows.h> first): sockets are
// passed as uintptr_t (SOCKET is UINT_PTR).
//
//=============================================================================//
#include <cstdint>

struct sockaddr;
struct sockaddr_in6;

// Resolves NET_DecodeDatagram and the client state array, hooks the former.
bool S30Signon_Install(void);

// Routes datagrams between the engine and the dedi at dest (bridge_connect);
// hBridgeSocket is the bridge's own socket, whose sends are left alone.
void S30Signon_Begin(const sockaddr_in6& dest, uintptr_t hBridgeSocket);
void S30Signon_End(void);
// Queues the native type-12 reply naming pszMap for the engine.
void S30Signon_OnConnAccept(const char* pszMap);

// net_guard hooks. FilterSendTo returns true when the datagram went to the dedi
// from the engine and was dropped; InjectRecv returns a queued datagram's length
// for the engine's dedi socket, or 0 to let the real recvfrom run.
bool S30Signon_FilterSendTo(uintptr_t s, const char* pBuf, int nLen, const sockaddr* pTo, int nToLen);
int S30Signon_InjectRecv(uintptr_t s, char* pBuf, int nLen, sockaddr* pFrom, int* pnFromLen);

// Queues a datagram from the dedi for the engine socket (bridge poll thread).
bool S30Signon_QueueForEngine(const uint8_t* pData, int nLen);
// True if an engine netadr (type +0, address +4, port +20) is the dedi.
bool S30Signon_IsDestNetadr(const uint8_t* pNetadr);

// sdk_clstate: signon state, server address and netchannel of client state 0.
void S30Signon_PrintClientState(void);

#endif // CORE_S30_BRIDGE_SIGNON_H
