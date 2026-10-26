//=============================================================================//
//
// Purpose: capture the S30 engine's log (CLoggingSystem::LogDirect).
//
// S30.1 0x5C6C40, LogDirect(pSystem, channel, severity, color, pszText): writes
// to stdout (+0xFE49 -logtostdout), the -logtofile handle (+0xFE40) and
// OutputDebugStringA (+0xFE38 -logtodebugoutput), then calls every registered
// listener; severity 3 ends the process. *(int*)pSystem is the channel count
// and the channels are 56-byte LoggingChannel_t records from +8
// (id, flags, minimum severity, color, name[32], next), so a channel's name is
// at pSystem + 24 + 56 * channel.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include "thirdparty/detours/include/detours.h"
#include "s30_log.h"
#include "s30_pattern.h"
#include "console_s30.h"
#include "engine_log_s30.h"

typedef int (*LogDirect_t)(void* pSystem, int nChannel, int nSeverity, int nColor, const char* pszText);

static const char* const s_pszPatternLogDirect =
	"48 89 5C 24 10 48 89 6C 24 18 44 89 4C 24 20 56 57 41 57 48 83 EC 50 48 8B F9 48 63 F2 8B 0D";
// cmp [rdi+0FE49h], r15b -- the -logtostdout check, at +97 from the entry.
static const uint8_t s_expectStdoutCheck[7] = { 0x44, 0x38, 0xBF, 0x49, 0xFE, 0x00, 0x00 };
static const int s_nStdoutCheckOffset = 97;

static LogDirect_t v_LogDirect = nullptr;

// S30.1 0xD39C30, the engine's print dispatcher CoreMsgV(type, fmt, args), behind
// the variadic CoreMsg at 0x269B40 (241 references). Types 0-2 go on to
// LoggingSystem_LogV (0x5C7680), which drops anything below the channel's
// minimum severity -- in retail that is nearly everything; 3/4 are formatted
// and dropped; 5 is a fatal Error(); 9 is ignored. Capturing here sees the text
// before any of that filtering.
typedef void (*CoreMsgV_t)(char nType, const char* pszFormat, va_list args);
static const char* const s_pszPatternCoreMsgV =
	"48 89 5C 24 10 48 89 6C 24 18 56 B8 40 27 00 00 E8 ?? ?? ?? ?? 48 2B E0 0F B6 D9 49 8B F0 48 8B EA 80 FB 09 0F 84";
static CoreMsgV_t v_CoreMsgV = nullptr;
// Set while inside CoreMsgV, so its own trip through LogDirect is not logged twice.
static thread_local bool s_bInCoreMsgV = false;

enum : int
{
	LS_MESSAGE = 0,
	LS_WARNING = 1,
	LS_ASSERT = 2,
	LS_ERROR = 3,
};

static const char* EngineLog_ChannelName(void* pSystem, int nChannel)
{
	const int nChannels = *reinterpret_cast<const int*>(pSystem);
	if (nChannel < 0 || nChannel >= nChannels || nChannels > 1024)
		return "?";
	const char* const pszName = reinterpret_cast<const char*>(pSystem) + 24 + 56 * nChannel;
	// Only trust a short, printable, terminated name.
	for (int i = 0; i < 32; ++i)
	{
		if (pszName[i] == '\0')
			return i ? pszName : "?";
		if (pszName[i] < 0x20 || pszName[i] > 0x7E)
			return "?";
	}
	return "?";
}

static void h_CoreMsgV(char nType, const char* pszFormat, va_list args)
{
	if (pszFormat && nType != 9)
	{
		char szText[4096];
		vsnprintf(szText, sizeof(szText), pszFormat, args); // MSVC x64 va_list is a pointer: args stays valid below
		const size_t nLen = strlen(szText);
		const bool bNewline = nLen && szText[nLen - 1] == '\n';
		S30LogFile("[ENGINE][t%d] %s%s", nType, szText, bNewline ? "" : "\n");

		S30ConsoleColor_t eColor = S30ConsoleColor_t::DEFAULT;
		if (nType == 1)
			eColor = S30ConsoleColor_t::WARNING;
		else if (nType == 5 || nType == 6)
			eColor = S30ConsoleColor_t::FAULT;
		char szLine[4200];
		snprintf(szLine, sizeof(szLine), "%s%s", szText, bNewline ? "" : "\n");
		S30Console_Write(szLine, eColor);
	}
	s_bInCoreMsgV = true;
	v_CoreMsgV(nType, pszFormat, args);
	s_bInCoreMsgV = false;
}

static int h_LogDirect(void* pSystem, int nChannel, int nSeverity, int nColor, const char* pszText)
{
	if (pszText && !s_bInCoreMsgV)
	{
		const char* const pszChannel = EngineLog_ChannelName(pSystem, nChannel);
		const size_t nLen = strlen(pszText);
		const bool bNewline = nLen && pszText[nLen - 1] == '\n';
		S30LogFile("[ENGINE][%s]%s %s%s", pszChannel, nSeverity == LS_WARNING ? "[W]" : nSeverity >= LS_ASSERT ? "[E]" : "",
			pszText, bNewline ? "" : "\n");

		S30ConsoleColor_t eColor = S30ConsoleColor_t::DEFAULT;
		if (nSeverity == LS_WARNING)
			eColor = S30ConsoleColor_t::WARNING;
		else if (nSeverity >= LS_ASSERT)
			eColor = S30ConsoleColor_t::FAULT;
		else if (strstr(pszChannel, "SCRIPT") || !strcmp(pszChannel, "UI") || !strcmp(pszChannel, "CLIENT"))
			eColor = S30ConsoleColor_t::SCRIPT;
		char szLine[4096];
		snprintf(szLine, sizeof(szLine), "[%s] %s%s", pszChannel, pszText, bNewline ? "" : "\n");
		S30Console_Write(szLine, eColor);
	}
	return v_LogDirect(pSystem, nChannel, nSeverity, nColor, pszText);
}

// S30.1 0x4165E0 Host_Error(fmt, ...): formats into a stack buffer, runs
// Host_Disconnect (0x271EE0) and longjmps back to the frame loop -- retail
// prints the text nowhere, so it is logged here.
typedef void (*HostError_t)(const char* pszFormat, ...);
static const char* const s_pszPatternHostError =
	"48 89 4C 24 08 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 53 57 48 81 EC 38 04 00 00 80 3D ?? ?? ?? ?? 00 48 8B D9 74 14 48 8D 0D";
static HostError_t v_HostError = nullptr;

static void h_HostError(const char* pszFormat, ...)
{
	char szText[1024];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szText, sizeof(szText), pszFormat ? pszFormat : "", args);
	va_end(args);
	S30Log("[ENGINE] Host_Error: %s\n", szText);
	v_HostError("%s", szText);
}

// S30.1 0x4164D0 Host_EndGame(bShowMainMenu, fmt, ...): the same, with the
// reason going only to the disconnect printer (0x24C700).
typedef void (*HostEndGame_t)(unsigned char bShowMainMenu, const char* pszFormat, ...);
static const char* const s_pszPatternHostEndGame =
	"48 8B C4 48 89 50 10 4C 89 40 18 4C 89 48 20 53 56 57 48 81 EC 30 04 00 00 48 8B DA 48 8D 70 18 0F B6 F9 E8";
static HostEndGame_t v_HostEndGame = nullptr;

static void h_HostEndGame(unsigned char bShowMainMenu, const char* pszFormat, ...)
{
	char szText[1024];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szText, sizeof(szText), pszFormat ? pszFormat : "", args);
	va_end(args);
	S30Log("[ENGINE] Host_EndGame: %s\n", szText);
	v_HostEndGame(bShowMainMenu, "%s", szText);
}

// S30.1 0x251220: the datatable code's warning ("No matching RecvTable ...",
// "SetupClientSendTableHierarchy: ..."), formatted on the stack and dropped in retail.
typedef int (*DataTableWarning_t)(const char* pszFormat, ...);
static const char* const s_pszPatternDataTableWarning =
	"48 89 4C 24 08 48 89 54 24 10 4C 89 44 24 18 4C 89 4C 24 20 53 57 B8 38 10 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B D9 48 8D BC 24 58 10 00 00 E8";
static DataTableWarning_t v_DataTableWarning = nullptr;

static int h_DataTableWarning(const char* pszFormat, ...)
{
	char szText[1024];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szText, sizeof(szText), pszFormat ? pszFormat : "", args);
	va_end(args);
	const size_t nLen = strlen(szText);
	S30Log("[ENGINE] DataTable: %s%s", szText, nLen && szText[nLen - 1] == '\n' ? "" : "\n");
	return v_DataTableWarning("%s", szText);
}

bool S30EngineLog_Install(void)
{
	int nMatches = 0;
	uint8_t* const pTarget = S30Pattern_FindUnique(s_pszPatternLogDirect, &nMatches);
	if (!pTarget || memcmp(pTarget + s_nStdoutCheckOffset, s_expectStdoutCheck, sizeof(s_expectStdoutCheck)) != 0)
	{
		S30Log("[ENGINE-LOG] LogDirect unresolved or body mismatch (matches=%d) -- engine log not captured\n", nMatches);
		return false;
	}

	uint8_t* const pCoreMsgV = S30Pattern_FindUnique(s_pszPatternCoreMsgV, &nMatches);
	if (!pCoreMsgV)
		S30Log("[ENGINE-LOG] CoreMsgV unresolved (matches=%d) -- only LogDirect captured\n", nMatches);

	v_HostError = reinterpret_cast<HostError_t>(S30Pattern_FindUnique(s_pszPatternHostError, &nMatches));
	if (!v_HostError)
		S30Log("[ENGINE-LOG] Host_Error unresolved (matches=%d)\n", nMatches);
	v_HostEndGame = reinterpret_cast<HostEndGame_t>(S30Pattern_FindUnique(s_pszPatternHostEndGame, &nMatches));
	if (!v_HostEndGame)
		S30Log("[ENGINE-LOG] Host_EndGame unresolved (matches=%d)\n", nMatches);
	v_DataTableWarning = reinterpret_cast<DataTableWarning_t>(S30Pattern_FindUnique(s_pszPatternDataTableWarning, &nMatches));
	if (!v_DataTableWarning)
		S30Log("[ENGINE-LOG] DataTable warning unresolved (matches=%d)\n", nMatches);

	v_LogDirect = reinterpret_cast<LogDirect_t>(pTarget);
	v_CoreMsgV = reinterpret_cast<CoreMsgV_t>(pCoreMsgV);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_LogDirect), h_LogDirect);
	if (v_CoreMsgV)
		DetourAttach(&reinterpret_cast<PVOID&>(v_CoreMsgV), h_CoreMsgV);
	if (v_HostError)
		DetourAttach(&reinterpret_cast<PVOID&>(v_HostError), h_HostError);
	if (v_HostEndGame)
		DetourAttach(&reinterpret_cast<PVOID&>(v_HostEndGame), h_HostEndGame);
	if (v_DataTableWarning)
		DetourAttach(&reinterpret_cast<PVOID&>(v_DataTableWarning), h_DataTableWarning);
	const LONG nError = DetourTransactionCommit();
	S30Log("[ENGINE-LOG] LogDirect @ 0x%llX, CoreMsgV @ 0x%llX %s (commit %ld)\n",
		static_cast<unsigned long long>(S30Pattern_Rva(pTarget)),
		static_cast<unsigned long long>(pCoreMsgV ? S30Pattern_Rva(pCoreMsgV) : 0),
		nError == NO_ERROR ? "hooked" : "FAILED", nError);
	return nError == NO_ERROR;
}
