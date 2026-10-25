#ifndef CORE_S30_NET_DIAG_H
#define CORE_S30_NET_DIAG_H
//=============================================================================//
//
// Purpose: -s30_netdiag observers on the S30 client connect path.
//
//=============================================================================//

// Observe-only hooks on the challenge/resend routine and the engine packet
// send, for mapping the S30 connect path. No-op without -s30_netdiag.
void S30NetDiag_Install(void);

#endif // CORE_S30_NET_DIAG_H
