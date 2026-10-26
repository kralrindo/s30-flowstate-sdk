#ifndef CORE_S30_BRIDGE_PREDTOL_H
#define CORE_S30_BRIDGE_PREDTOL_H
#include <cstdint>
//=============================================================================//
//
// Purpose: prediction error-check tolerances for the S3 dedi (the S21 bridge's
// [PRED-TOL] rows on S30's C_Player prediction map).
//
//=============================================================================//

// After each snapshot: applies the tolerances once the local player's
// optimized prediction map exists.
void S30PredTol_OnSnapshot(void);
// Before S30's prediction error check (bridge_prederr's hook): gives the server
// record the client-owned bits the dedi never sets, so they neither flag an
// error nor get restored away.
void S30PredTol_BeforeErrorCheck(uint8_t* pCtx, uint8_t* pMap, int nBlock);

#endif // CORE_S30_BRIDGE_PREDTOL_H
