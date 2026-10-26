#ifndef CORE_S30_BRIDGE_CMDSEED_H
#define CORE_S30_BRIDGE_CMDSEED_H
//=============================================================================//
//
// Purpose: per-usercmd prediction RNG seed from the command number only, as the
// dedi derives it (bridge_cmd_seed_parity).
//
//=============================================================================//

bool S30CmdSeed_Install(void);

#endif // CORE_S30_BRIDGE_CMDSEED_H
