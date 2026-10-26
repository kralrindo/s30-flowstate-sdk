#ifndef CORE_S30_BRIDGE_NOCLIP_H
#define CORE_S30_BRIDGE_NOCLIP_H
//=============================================================================//
//
// Purpose: client noclip prediction (the S21 bridge's [NOCLIP-SIM] on S30).
//
//=============================================================================//

// Fills PlayerMove's empty MOVETYPE_NOCLIP case with the S3 dedi's noclip step.
bool S30Noclip_Install(void);

#endif // CORE_S30_BRIDGE_NOCLIP_H
