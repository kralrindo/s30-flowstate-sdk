#ifndef CORE_S30_LOG_H
#define CORE_S30_LOG_H
//=============================================================================//
//
// Purpose: client_s30 log file (platform/logs/client_s30/<time>.log).
//
//=============================================================================//

void S30Log_Init(void);
void S30Log_Shutdown(void);
void S30Log(const char* pszFormat, ...);

// Module + offset of an address ("r5apex_dx12.exe+0x588C50"), for blame lines.
void S30Log_FormatAddress(const void* pAddress, char* pszOut, unsigned long nOut);

#endif // CORE_S30_LOG_H
