#ifndef CORE_S30_ENGINE_LOG_H
#define CORE_S30_ENGINE_LOG_H
//=============================================================================//
//
// Purpose: capture the S30 engine's log (CLoggingSystem::LogDirect).
//
//=============================================================================//

// Detours LogDirect so every engine and script log line also reaches the
// client_s30 log and the SDK console. Returns false if the target did not
// validate (nothing is attached then).
bool S30EngineLog_Install(void);

#endif // CORE_S30_ENGINE_LOG_H
