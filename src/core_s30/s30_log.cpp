//=============================================================================//
//
// Purpose: client_s30 log file.
//
// One file per run under platform/logs/client_s30/, opened share-read so it can
// be read while the client runs. Every line is flushed: a crash must not eat
// the last lines.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <share.h>
#include "s30_log.h"
#include "console_s30.h"

static FILE* s_pLogFile = nullptr;
static CRITICAL_SECTION s_logLock;
static bool s_bLogInit = false;
static ULONGLONG s_nLogStartMs = 0;

void S30Log_Init(void)
{
	if (s_bLogInit)
		return;
	InitializeCriticalSection(&s_logLock);
	s_nLogStartMs = GetTickCount64();
	s_bLogInit = true;

	CreateDirectoryA("platform", NULL);
	CreateDirectoryA("platform\\logs", NULL);
	CreateDirectoryA("platform\\logs\\client_s30", NULL);

	SYSTEMTIME st;
	GetLocalTime(&st);
	char szPath[MAX_PATH];
	snprintf(szPath, sizeof(szPath), "platform\\logs\\client_s30\\%04u%02u%02u_%02u%02u%02u.log",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	s_pLogFile = _fsopen(szPath, "w", _SH_DENYNO);
}

void S30Log_Shutdown(void)
{
	if (!s_bLogInit)
		return;
	EnterCriticalSection(&s_logLock);
	if (s_pLogFile)
	{
		fclose(s_pLogFile);
		s_pLogFile = nullptr;
	}
	LeaveCriticalSection(&s_logLock);
}

static void Log_Write(const bool bConsole, const char* pszFormat, va_list args)
{
	if (!s_bLogInit)
		return;

	char szLine[4096];
	const int nPrefix = snprintf(szLine, sizeof(szLine), "[%9.3f] [%5lu] ",
		(GetTickCount64() - s_nLogStartMs) / 1000.0, GetCurrentThreadId());
	vsnprintf(szLine + nPrefix, sizeof(szLine) - nPrefix, pszFormat, args);

	EnterCriticalSection(&s_logLock);
	if (s_pLogFile)
	{
		fputs(szLine, s_pLogFile);
		fflush(s_pLogFile);
	}
	LeaveCriticalSection(&s_logLock);
	if (bConsole)
		S30Console_Write(szLine + nPrefix, S30ConsoleColor_t::SDK);
}

void S30Log(const char* pszFormat, ...)
{
	va_list args;
	va_start(args, pszFormat);
	Log_Write(true, pszFormat, args);
	va_end(args);
}

void S30LogFile(const char* pszFormat, ...)
{
	va_list args;
	va_start(args, pszFormat);
	Log_Write(false, pszFormat, args);
	va_end(args);
}

void S30Log_FormatAddress(const void* pAddress, char* pszOut, unsigned long nOut)
{
	HMODULE hModule = NULL;
	pszOut[0] = '\0';
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCSTR>(pAddress), &hModule) && hModule)
	{
		char szPath[MAX_PATH];
		if (GetModuleFileNameA(hModule, szPath, MAX_PATH))
		{
			const char* pszLeaf = strrchr(szPath, '\\');
			snprintf(pszOut, nOut, "%s+0x%llX", pszLeaf ? pszLeaf + 1 : szPath,
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pAddress) - reinterpret_cast<uintptr_t>(hModule)));
			return;
		}
	}
	snprintf(pszOut, nOut, "%p", pAddress);
}
