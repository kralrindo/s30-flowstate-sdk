#ifndef CORE_S30_OFFLINE_H
#define CORE_S30_OFFLINE_H
//=============================================================================//
//
// Purpose: keep the S30 client from starting the EA platform client.
//
//=============================================================================//

// CreateProcessA/W watchdog: platform client launches (EA app, Origin) are
// refused and logged; every other spawn is logged and allowed.
bool S30Offline_Install(void);

#endif // CORE_S30_OFFLINE_H
