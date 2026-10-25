#ifndef CORE_S30_CONCOMMAND_H
#define CORE_S30_CONCOMMAND_H
//=============================================================================//
//
// Purpose: S30 ICvar access and engine ConCommand registration.
//
//=============================================================================//

// Resolves g_pCVar and the ConCommand template. Call once (early init).
bool S30ConCommand_Init(void);
// True once the engine's static commands are registered (ICvar usable).
bool S30ConCommand_IsReady(void);
// Registers every SDK command (sdk_commands_s30) as an engine ConCommand, so
// binds, cfg files and the engine console see them. Idempotent; returns the
// number registered by this call.
int S30ConCommand_RegisterSdkCommands(void);

// ICvar lookups (null when not found or not ready).
void* S30ConCommand_FindCommandBase(const char* pszName);
// Flags of a command/convar, -1 if not found.
int S30ConCommand_GetFlags(const char* pszName);
// Clears FCVAR_DEVELOPMENTONLY and FCVAR_HIDDEN on every registered command and
// convar (the S21 SDK does this under -devsdk). Returns the number changed.
int S30ConCommand_UnlockDevelopmentOnly(void);

#endif // CORE_S30_CONCOMMAND_H
