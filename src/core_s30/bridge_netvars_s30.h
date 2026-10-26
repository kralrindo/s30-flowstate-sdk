#ifndef CORE_S30_BRIDGE_NETVARS_H
#define CORE_S30_BRIDGE_NETVARS_H
//=============================================================================//
//
// Purpose: the S30 client's script netvar table, sent to the dedi by name.
//
//=============================================================================//

bool S30NetVars_Install(void);
// After a snapshot: sends the table once registration has settled.
void S30NetVars_OnSnapshot(void);
// New connection: the table goes out again.
void S30NetVars_Reset(void);

#endif // CORE_S30_BRIDGE_NETVARS_H
