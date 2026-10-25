#ifndef CORE_S30_CRASH_H
#define CORE_S30_CRASH_H
//=============================================================================//
//
// Purpose: fault capture for the S30 client.
//
//=============================================================================//

// Vectored handler: logs fault exceptions (module+RVA, registers, return
// addresses found on the stack) and writes a minidump for the first one to
// platform/logs/client_s30/. Observe-only; it never handles the exception.
void S30Crash_Install(void);

#endif // CORE_S30_CRASH_H
