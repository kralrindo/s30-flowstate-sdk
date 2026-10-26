#ifndef CORE_S30_BRIDGE_PREDERR_H
#define CORE_S30_BRIDGE_PREDERR_H
//=============================================================================//
//
// Purpose: prediction error report (diagnostic): which predicted field fails
// S30's error check against the server's copy.
//
//=============================================================================//

// Hooks the error check (early init).
bool S30PredErr_Install(void);
// sdk_prederr [0|1|reset]
void S30PredErr_RegisterCommands(void);

#endif // CORE_S30_BRIDGE_PREDERR_H
