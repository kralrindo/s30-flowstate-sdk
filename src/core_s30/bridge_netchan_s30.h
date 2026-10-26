#ifndef CORE_S30_BRIDGE_NETCHAN_H
#define CORE_S30_BRIDGE_NETCHAN_H
//=============================================================================//
//
// Purpose: S3 dedi connection, stage 2: the netchannel.
//
// S2C: the dedi's netchannel packets reach the engine socket (bridge_signon),
// and CNetChan::ProcessPacket is replaced for them: S3 header, nonce and
// subchannel parsing, then each S3 message is dispatched into S30's message
// array (docs/11 type map, S21 bridge format bridges). The signon DataBlock is
// dispatched the same way on the engine thread.
// C2S: the bridge builds the S3 dedi's packets itself (S21 bridge
// S21Bridge_BuildS3Packet): nonce ack, net_SignonState rungs and UserInfo.
//
//=============================================================================//
#include <cstdint>

typedef int (*S30NetchanSend_t)(const uint8_t* pData, int nLen);

// Resolves and hooks CNetChan::ProcessPacket and CClientState::SetSignonState.
bool S30Netchan_Install(void);

// pfnSend sends on the bridge socket.
void S30Netchan_Begin(S30NetchanSend_t pfnSend);
void S30Netchan_End(void);
// CONNACCEPT: net_SignonState(CONNECTED) then UserInfo, as the S21 bridge does.
void S30Netchan_OnConnAccept(void);
// Bridge poll thread: keepalive flush (the engine may be stalled in a load).
void S30Netchan_Pump(void);
// The engine sent a datagram to the dedi (dropped): flush C2S, rate-limited.
void S30Netchan_OnEngineSend(void);

// sdk_netchan
void S30Netchan_PrintStatus(void);

#endif // CORE_S30_BRIDGE_NETCHAN_H
