#ifndef CORE_S30_BRIDGE_PERSISTENCE_H
#define CORE_S30_BRIDGE_PERSISTENCE_H
//=============================================================================//
//
// Purpose: S30 persistent vars the dedi's pdef does not have.
//
// The dedi sends its own pdef (svc_PersistenceDefFile). S30's stats code asks
// PersistenceGetVarHandle for every stat it registers, and S30-only stat
// structs (characterClasses, metachallenge, ...) are not in the dedi's pdef: its
// parser holds 40 struct types. The dedi's scripts never write those stats, so
// the client gets a dummy handle for them instead of a script error, and every
// such name is logged as [PDEF-MISSING] (console: one line per top-level path).
// See s30/docs/12.
//
//=============================================================================//

// Resolves and hooks PersistenceGetVarHandle.
bool S30Persistence_Install(void);

#endif // CORE_S30_BRIDGE_PERSISTENCE_H
