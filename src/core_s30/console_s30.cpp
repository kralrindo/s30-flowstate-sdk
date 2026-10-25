//=============================================================================//
//
// Purpose: SDK terminal console for the S30 client.
//
// A Win32 console window, as the S21 client opens in SDK_SetupConsole. Output
// is engine log lines (engine_log_s30) and client_s30's own lines; input is
// read with ReadConsoleA (std::cin misbehaves on injected threads, see S21) and
// handed to the engine's Cbuf_AddText, which takes its own lock.
//
// Cbuf_AddText on S30.1 is 0x248760, verified against S21 DX12 0x223D90: same
// body, same lock-protected buffer, same 9872-byte per-target stride.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "s30_log.h"
#include "s30_pattern.h"
#include "console_s30.h"

typedef void (*Cbuf_AddText_t)(int nTarget, const char* pszText, int nSource);

static const char* const s_pszPatternCbufAddText =
	"48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC 20 48 63 D9 41 8B F8 48 8D 0D ?? ?? ?? ?? 48 8B F2 FF 15 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 41 B9 ?? ?? ?? ??";

static HANDLE s_hConOut = NULL;
static CRITICAL_SECTION s_conLock;
static bool s_bConsole = false;
static Cbuf_AddText_t v_Cbuf_AddText = nullptr;

void S30Console_Init(void)
{
	if (s_bConsole)
		return;
	if (strstr(GetCommandLineA(), "-s30_noconsole"))
		return;

	InitializeCriticalSection(&s_conLock);
	FreeConsole();
	if (!AllocConsole())
		return;
	SetConsoleTitleA("R5F S30 Client");
	s_hConOut = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	DWORD nMode = 0;
	if (GetConsoleMode(s_hConOut, &nMode))
		SetConsoleMode(s_hConOut, nMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	s_bConsole = true;
	S30Console_Write("R5F S30 Client console -- engine log below, type commands to run them.\n", S30ConsoleColor_t::SDK);
}

void S30Console_Write(const char* pszText, S30ConsoleColor_t eColor)
{
	if (!s_bConsole || !pszText)
		return;

	const char* pszColor = "";
	switch (eColor)
	{
	case S30ConsoleColor_t::SDK:     pszColor = "\x1b[96m"; break; // bright cyan
	case S30ConsoleColor_t::WARNING: pszColor = "\x1b[93m"; break; // bright yellow
	case S30ConsoleColor_t::FAULT:   pszColor = "\x1b[91m"; break; // bright red
	case S30ConsoleColor_t::SCRIPT:  pszColor = "\x1b[92m"; break; // bright green
	default: break;
	}

	EnterCriticalSection(&s_conLock);
	DWORD nWritten = 0;
	if (pszColor[0])
		WriteConsoleA(s_hConOut, pszColor, static_cast<DWORD>(strlen(pszColor)), &nWritten, NULL);
	WriteConsoleA(s_hConOut, pszText, static_cast<DWORD>(strlen(pszText)), &nWritten, NULL);
	if (pszColor[0])
		WriteConsoleA(s_hConOut, "\x1b[0m", 4, &nWritten, NULL);
	LeaveCriticalSection(&s_conLock);
}

static DWORD WINAPI Console_InputThread(LPVOID)
{
	const HANDLE hIn = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	char szLine[1024];
	for (;;)
	{
		DWORD nRead = 0;
		if (!ReadConsoleA(hIn, szLine, sizeof(szLine) - 2, &nRead, NULL))
			break;
		while (nRead > 0 && (szLine[nRead - 1] == '\n' || szLine[nRead - 1] == '\r'))
			--nRead;
		if (nRead == 0)
			continue;
		szLine[nRead] = '\n';
		szLine[nRead + 1] = '\0';

		S30Log("[CONSOLE] ] %s", szLine);
		v_Cbuf_AddText(0 /*CBUF_FIRST_PLAYER*/, szLine, 0 /*kCommandSrcCode*/);
	}
	return 0;
}

// -s30_selftest: 20 s after start, run commands through the same path the
// console uses. Retail S30 compiles echo's body out, so the observable check
// is quit: the [EXIT] line in the log (with the dispatch frames) proves that
// a console line reached the engine and ran.
static DWORD WINAPI Console_SelfTestThread(LPVOID)
{
	Sleep(20000);
	static const char s_szEcho[] = "echo [S30-SELFTEST] console command reached the engine\n";
	S30Log("[CONSOLE] selftest: %s", s_szEcho);
	v_Cbuf_AddText(0 /*CBUF_FIRST_PLAYER*/, s_szEcho, 0 /*kCommandSrcCode*/);
	Sleep(5000);
	static const char s_szQuit[] = "quit\n";
	S30Log("[CONSOLE] selftest: %s", s_szQuit);
	v_Cbuf_AddText(0 /*CBUF_FIRST_PLAYER*/, s_szQuit, 0 /*kCommandSrcCode*/);
	return 0;
}

void S30Console_StartInput(void)
{
	if (!s_bConsole)
		return;

	int nMatches = 0;
	v_Cbuf_AddText = reinterpret_cast<Cbuf_AddText_t>(S30Pattern_FindUnique(s_pszPatternCbufAddText, &nMatches));
	if (!v_Cbuf_AddText)
	{
		S30Log("[CONSOLE] Cbuf_AddText unresolved (matches=%d) -- console is output-only\n", nMatches);
		return;
	}
	S30Log("[CONSOLE] Cbuf_AddText @ 0x%llX, input thread started\n",
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<void*>(v_Cbuf_AddText))));
	CloseHandle(CreateThread(NULL, 0, Console_InputThread, NULL, 0, NULL));
	if (strstr(GetCommandLineA(), "-s30_selftest"))
		CloseHandle(CreateThread(NULL, 0, Console_SelfTestThread, NULL, 0, NULL));
}
