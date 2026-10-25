#ifndef CORE_S30_ORIGIN_OFFLINE_H
#define CORE_S30_ORIGIN_OFFLINE_H
//=============================================================================//
//
// Purpose: S30 port of the S21 offline platform patches (ebisusdk/origin_offline.cpp).
//
//=============================================================================//

// Byte patches D, E, A1, A2 and C on the statically linked Origin SDK init, so
// the client neither requires nor launches the EA app and treats the platform as
// started. All sites are validated first; nothing is written unless all pass.
bool S30OriginOffline_Apply(void);

#endif // CORE_S30_ORIGIN_OFFLINE_H
