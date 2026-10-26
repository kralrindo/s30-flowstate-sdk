#ifndef CORE_S30_BRIDGE_HANDSHAKE_H
#define CORE_S30_BRIDGE_HANDSHAKE_H
//=============================================================================//
//
// Purpose: S3 dedi connection handshake for the S30 client (bridge stage 1).
//
//=============================================================================//

// Starts the S3 handshake to pszHostPort ("ip:port", loopback only for now):
// C2S_CHALLENGE -> S2C_CHALLENGE -> C2S_CONNECT -> CONNACCEPT, on the bridge's
// own socket. Returns false if a handshake is already running or the address
// does not parse.
bool S30Bridge_Connect(const char* pszHostPort);
// True (once) for each engine `connect` the bridge queued itself after its handshake.
bool S30Bridge_TakeInternalConnect(void);
void S30Bridge_Disconnect(void);
// One-line state for sdk_status / the overlay.
const char* S30Bridge_StateName(void);

#endif // CORE_S30_BRIDGE_HANDSHAKE_H
