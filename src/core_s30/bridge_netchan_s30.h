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
// A DataBlock transfer completed: queue a wake datagram for the engine socket so
// it is dispatched on the engine thread without waiting for a netchannel packet.
void S30Netchan_WakeForDataBlock(void);
// The dedi's tick interval from ServerInfo (0 until it arrived).
float S30Netchan_TickInterval(void);
// An S3 clc_Move message (7-bit type included) for the next C2S packet; flushes now.
bool S30Netchan_QueueMove(const uint8_t* pBits, int nBits);

// A relayed client message (7-bit S3 type included) sent nSends times, nSpacingMs
// apart, in the next C2S packets; flushes now. False when the queue is full.
bool S30Netchan_QueueC2S(const uint8_t* pBits, int nBits, int nSends, int nSpacingMs);

// An S30 bf_read (S30BfRead::SIZE bytes at pBf) over pData, at bit 0.
void S30Netchan_BfInit(uint8_t* pBf, const uint8_t* pData, int nBytes);

// sdk_netchan
void S30Netchan_PrintStatus(void);

#endif // CORE_S30_BRIDGE_NETCHAN_H
