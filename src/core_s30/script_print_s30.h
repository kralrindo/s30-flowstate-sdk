#ifndef CORE_S30_SCRIPT_PRINT_H
#define CORE_S30_SCRIPT_PRINT_H
//=============================================================================//
//
// Purpose: capture Squirrel print()/printt() output on the S30 client.
//
//=============================================================================//

// Detours the two Squirrel print callbacks (SERVER/CLIENT and UI) so script
// output reaches the client_s30 log and the SDK console. Returns false if
// either target did not resolve uniquely (nothing is attached then).
bool S30ScriptPrint_Install(void);
// Logs each VM's print callback pointer and the hook call counts (boot watch).
void S30ScriptPrint_Diag(void);

#endif // CORE_S30_SCRIPT_PRINT_H
