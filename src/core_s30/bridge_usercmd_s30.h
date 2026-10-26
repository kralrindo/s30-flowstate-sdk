#ifndef CORE_S30_BRIDGE_USERCMD_H
#define CORE_S30_BRIDGE_USERCMD_H
//=============================================================================//
//
// Purpose: the engine's clc_Move to the S3 dedi.
//
// The engine's own C2S datagrams are dropped (bridge_signon); its clc_Move is
// caught in CNetChan::SendNetMsg before it is written, each user command is
// parsed in S30's format and emitted in the dedi's (the S21 bridge's
// S21Bridge_ParseS21Cmd / S21Bridge_EmitS3Cmd), and the S3 clc_Move rides in
// the bridge's next C2S packet. See s30/docs/12.
//
//=============================================================================//

// Resolves and hooks CNetChan::SendNetMsg and the clc_Move vtable.
bool S30Usercmd_Install(void);

#endif // CORE_S30_BRIDGE_USERCMD_H
