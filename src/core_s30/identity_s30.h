#ifndef CORE_S30_IDENTITY_H
#define CORE_S30_IDENTITY_H
//=============================================================================//
//
// Purpose: fixed offline platform identity for the S30 client.
//
//=============================================================================//

// Resolves the platform identity globals (early init).
bool S30Identity_Init(void);
// Sets the offline identity once the engine's Origin init has run, and again
// whenever the engine clears it. Call periodically (boot watch).
void S30Identity_Pump(void);
// Logs the current values (sdk_identity).
void S30Identity_Print(void);

#endif // CORE_S30_IDENTITY_H
