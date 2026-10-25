#ifndef CORE_S30_CONSOLE_H
#define CORE_S30_CONSOLE_H
//=============================================================================//
//
// Purpose: SDK terminal console for the S30 client.
//
//=============================================================================//

enum class S30ConsoleColor_t : int
{
	DEFAULT = 0,
	SDK,      // client_s30's own lines
	WARNING,
	FAULT,
	SCRIPT,
};

// Opens the console window (skipped with -s30_noconsole).
void S30Console_Init(void);
// Starts the input thread; lines go to the engine through Cbuf_AddText.
// Call once Cbuf_AddText is resolved.
void S30Console_StartInput(void);
void S30Console_Write(const char* pszText, S30ConsoleColor_t eColor);

// Everything written through S30Console_Write is also kept in a ring of the last
// lines, which the overlay console draws. Visit lines oldest-first under the
// ring lock; returns the total number of lines ever written (to detect new ones).
typedef void (*S30ConsoleVisit_t)(const char* pszLine, S30ConsoleColor_t eColor, void* pUser);
unsigned long long S30Console_VisitLines(S30ConsoleVisit_t pfnVisit, void* pUser);
void S30Console_ClearLines(void);
// Submits a command line the way the console window does (SDK command or Cbuf_AddText).
void S30Console_Submit(const char* pszLine);
// Straight to the engine command buffer, skipping the SDK dispatcher.
bool S30Console_SendToEngine(const char* pszText);

#endif // CORE_S30_CONSOLE_H
