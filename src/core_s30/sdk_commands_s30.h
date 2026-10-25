#ifndef CORE_S30_SDK_COMMANDS_H
#define CORE_S30_SDK_COMMANDS_H
//=============================================================================//
//
// Purpose: client_s30's own console commands.
//
// Handled before a line reaches the engine (S30Console_Submit). Until the S30
// ConVar/ConCommand ABI is verified these live only in the SDK consoles; they
// are not engine ConCommands, so binds and cfg files do not see them.
//
//=============================================================================//

typedef void (*S30CommandFn_t)(int nArgs, const char* const* ppszArgs);

void S30Commands_Register(const char* pszName, const char* pszHelp, S30CommandFn_t pfnCommand);
// Runs an SDK command if the line's first token names one; false otherwise.
bool S30Commands_Dispatch(const char* pszLine);
// All registered commands (for engine registration). Returns the count written.
int S30Commands_List(const char** ppszNames, const char** ppszHelp, int nMax);
// Names starting with pszPrefix, for console completion. Returns the count written.
int S30Commands_Complete(const char* pszPrefix, const char** ppszOut, int nMax);
void S30Commands_RegisterBuiltins(void);

#endif // CORE_S30_SDK_COMMANDS_H
