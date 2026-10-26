#ifndef CORE_S30_BRIDGE_C2S_H
#define CORE_S30_BRIDGE_C2S_H
//=============================================================================//
//
// Purpose: the S30 client's own client-to-server messages, relayed to the S3 dedi.
//
// The engine's packets never reach the dedi (the bridge builds every C2S
// packet), so each message the client sends through CNetChan::SendNetMsg is
// serialized with its own WriteToBuffer, transcoded to the dedi's message and
// queued in the bridge's C2S stream, as the S21 bridge does
// (S21Bridge_C2S_MsgRelayS3Type, net_bridge_c2s_reliable.cpp):
//   net_StringCmd    S30 3  -> S3 3,  "brq <seq> " prefix, sent 3 times
//   Remote_ServerCallFunction S30 4 -> S3 68, by name ("C2RS" frame), sent 3 times
//   net_SetConVar    S30 5  -> S3 4,  sent twice
//   ClaimClientSidePickup S30 70 -> S3 57
//   ClientSayText    S30 73 -> S3 61, "brc <seq> " prefix, sent 3 times
//   GamepadMsg       S30 82 -> S3 65
// The dedi drops repeats by sequence (CClient::VProcessStringCmd,
// CServerGameDLL::OnReceivedSayTextMessage).
//
//=============================================================================//

// pMsg is an INetMessage the engine is sending. True when the bridge relayed it.
bool S30C2S_OnEngineMessage(void* pMsg);
// Disconnect / new connection: sequences restart.
void S30C2S_Reset(void);

#endif // CORE_S30_BRIDGE_C2S_H
