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

#endif // CORE_S30_CONSOLE_H
