//=============================================================================//
//
// Purpose: SDK terminal console for the S30 client.
//
// A Win32 console window, as the S21 client opens in SDK_SetupConsole. Output
// is engine log lines (engine_log_s30) and client_s30's own lines; input is
// read with ReadConsoleA (std::cin misbehaves on injected threads, see S21).
// Every line also goes into a fixed ring the overlay console draws.
//
// Input goes through S30Console_Submit: SDK commands (sdk_commands_s30.cpp)
// first, everything else to the engine's Cbuf_AddText, which takes its own lock.
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
static SRWLOCK s_conLock = SRWLOCK_INIT;
static bool s_bConsole = false;
static Cbuf_AddText_t v_Cbuf_AddText = nullptr;

// Overlay ring: the last s_nRingLines lines, fixed storage, no allocation.
static const int s_nRingLines = 2000;
static const int s_nRingLineLen = 512;
static char s_szRing[s_nRingLines][s_nRingLineLen];
static S30ConsoleColor_t s_eRingColor[s_nRingLines];
static unsigned long long s_nRingWritten = 0;
static SRWLOCK s_ringLock = SRWLOCK_INIT;
// A partial line (no newline yet) is held back until it completes.
static char s_szPending[s_nRingLineLen];
static size_t s_nPending = 0;

// SDK commands first (sdk_commands_s30.cpp), then the engine's command buffer.
extern bool S30Commands_Dispatch(const char* pszLine);

static void Console_RingPush(const char* pszText, S30ConsoleColor_t eColor)
{
	AcquireSRWLockExclusive(&s_ringLock);
	for (const char* p = pszText; *p; ++p)
	{
		if (*p == '\r')
			continue;
		if (*p != '\n')
		{
			if (s_nPending < sizeof(s_szPending) - 1)
				s_szPending[s_nPending++] = *p; // an overlong line keeps its head
			continue;
		}
		s_szPending[s_nPending] = '\0';
		const int nSlot = static_cast<int>(s_nRingWritten % s_nRingLines);
		memcpy(s_szRing[nSlot], s_szPending, s_nPending + 1);
		s_eRingColor[nSlot] = eColor;
		++s_nRingWritten;
		s_nPending = 0;
	}
	ReleaseSRWLockExclusive(&s_ringLock);
}

unsigned long long S30Console_VisitLines(S30ConsoleVisit_t pfnVisit, void* pUser)
{
	AcquireSRWLockShared(&s_ringLock);
	const unsigned long long nTotal = s_nRingWritten;
	const unsigned long long nFirst = nTotal > s_nRingLines ? nTotal - s_nRingLines : 0;
	for (unsigned long long i = nFirst; i < nTotal; ++i)
	{
		const int nSlot = static_cast<int>(i % s_nRingLines);
		pfnVisit(s_szRing[nSlot], s_eRingColor[nSlot], pUser);
	}
	ReleaseSRWLockShared(&s_ringLock);
	return nTotal;
}

void S30Console_ClearLines(void)
{
	AcquireSRWLockExclusive(&s_ringLock);
	s_nRingWritten = 0;
	s_nPending = 0;
	ReleaseSRWLockExclusive(&s_ringLock);
}

void S30Console_Init(void)
{
	if (s_bConsole)
		return;
	if (strstr(GetCommandLineA(), "-s30_noconsole"))
		return;

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
	if (!pszText)
		return;
	Console_RingPush(pszText, eColor);
	if (!s_bConsole)
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

	AcquireSRWLockExclusive(&s_conLock);
	DWORD nWritten = 0;
	if (pszColor[0])
		WriteConsoleA(s_hConOut, pszColor, static_cast<DWORD>(strlen(pszColor)), &nWritten, NULL);
	WriteConsoleA(s_hConOut, pszText, static_cast<DWORD>(strlen(pszText)), &nWritten, NULL);
	if (pszColor[0])
		WriteConsoleA(s_hConOut, "\x1b[0m", 4, &nWritten, NULL);
	ReleaseSRWLockExclusive(&s_conLock);
}

bool S30Console_SendToEngine(const char* pszText)
{
	if (!v_Cbuf_AddText)
		return false;
	char szLine[1024];
	snprintf(szLine, sizeof(szLine), "%s\n", pszText);
	v_Cbuf_AddText(0 /*CBUF_FIRST_PLAYER*/, szLine, 0 /*kCommandSrcCode*/);
	return true;
}

void S30Console_Submit(const char* pszLine)
{
	char szLine[1024];
	size_t n = strlen(pszLine);
	if (n > sizeof(szLine) - 2)
		n = sizeof(szLine) - 2;
	memcpy(szLine, pszLine, n);
	while (n > 0 && (szLine[n - 1] == '\n' || szLine[n - 1] == '\r' || szLine[n - 1] == ' '))
		--n;
	if (n == 0)
		return;
	szLine[n] = '\0';

	S30Log("] %s\n", szLine);
	if (S30Commands_Dispatch(szLine))
		return;
	if (!v_Cbuf_AddText)
	{
		S30Log("[CONSOLE] Cbuf_AddText unresolved -- command not sent\n");
		return;
	}
	szLine[n] = '\n';
	szLine[n + 1] = '\0';
	v_Cbuf_AddText(0 /*CBUF_FIRST_PLAYER*/, szLine, 0 /*kCommandSrcCode*/);
}

static DWORD WINAPI Console_InputThread(LPVOID)
{
	const HANDLE hIn = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	char szLine[1024];
	for (;;)
	{
		DWORD nRead = 0;
		if (!ReadConsoleA(hIn, szLine, sizeof(szLine) - 1, &nRead, NULL))
			break;
		szLine[nRead] = '\0';
		S30Console_Submit(szLine);
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
	S30Console_Submit("sdk_status");
	S30Console_Submit("echo [S30-SELFTEST] console command reached the engine");
	Sleep(5000);
	S30Console_Submit("quit");
	return 0;
}

// Command drop file: every 500 ms, platform/s30_commands.txt (if present) is
// read, deleted, and each line submitted like console input. Lets tools on this
// machine drive the client without window focus.
static DWORD WINAPI Console_DropFileThread(LPVOID)
{
	static const char s_szDropFile[] = "platform\\s30_commands.txt";
	static const char s_szTaken[] = "platform\\s30_commands.taken";
	for (;;)
	{
		Sleep(500);
		// Rename first, so a writer still filling the file never loses lines.
		if (!MoveFileExA(s_szDropFile, s_szTaken, MOVEFILE_REPLACE_EXISTING))
			continue;
		FILE* pFile = nullptr;
		if (fopen_s(&pFile, s_szTaken, "r") || !pFile)
			continue;
		char szLine[1024];
		while (fgets(szLine, sizeof(szLine), pFile))
			S30Console_Submit(szLine);
		fclose(pFile);
		DeleteFileA(s_szTaken);
	}
}

void S30Console_StartInput(void)
{
	CloseHandle(CreateThread(NULL, 0, Console_DropFileThread, NULL, 0, NULL));

	int nMatches = 0;
	v_Cbuf_AddText = reinterpret_cast<Cbuf_AddText_t>(S30Pattern_FindUnique(s_pszPatternCbufAddText, &nMatches));
	if (!v_Cbuf_AddText)
	{
		S30Log("[CONSOLE] Cbuf_AddText unresolved (matches=%d) -- console is output-only\n", nMatches);
		return;
	}
	S30Log("[CONSOLE] Cbuf_AddText @ 0x%llX\n",
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<void*>(v_Cbuf_AddText))));
	if (s_bConsole)
		CloseHandle(CreateThread(NULL, 0, Console_InputThread, NULL, 0, NULL));
	if (strstr(GetCommandLineA(), "-s30_selftest"))
		CloseHandle(CreateThread(NULL, 0, Console_SelfTestThread, NULL, 0, NULL));
}
