#ifndef CORE_S30_DEVMODE_H
#define CORE_S30_DEVMODE_H
//=============================================================================//
//
// Purpose: -dev / -developer: S30's #if DEV script code and dev script lists.
//
//=============================================================================//

// Early init (before LauncherMain reads the command line). True when dev mode is on.
bool S30DevMode_Install(void);
// Once ICvar is up: the dev-only convars the DEV script code reads. No-op without dev mode.
void S30DevMode_RegisterConVars(void);

#endif // CORE_S30_DEVMODE_H
