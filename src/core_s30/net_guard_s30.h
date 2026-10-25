#ifndef CORE_S30_NET_GUARD_H
#define CORE_S30_NET_GUARD_H
//=============================================================================//
//
// Purpose: loopback-only network guard for the S30 client.
//
//=============================================================================//

// Refuses every non-loopback connect / send / name lookup made by any module in
// the process and logs it. Returns false when the hooks could not be committed;
// the caller must then not let the client run.
bool S30NetGuard_Install(void);
long S30NetGuard_BlockedCount(void);

#endif // CORE_S30_NET_GUARD_H
