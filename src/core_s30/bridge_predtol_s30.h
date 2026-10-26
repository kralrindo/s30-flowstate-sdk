#ifndef CORE_S30_BRIDGE_PREDTOL_H
#define CORE_S30_BRIDGE_PREDTOL_H
//=============================================================================//
//
// Purpose: prediction error-check tolerances for the S3 dedi (the S21 bridge's
// [PRED-TOL] rows on S30's C_Player prediction map).
//
//=============================================================================//

// After each snapshot: applies the tolerances once the local player's
// optimized prediction map exists.
void S30PredTol_OnSnapshot(void);

#endif // CORE_S30_BRIDGE_PREDTOL_H
