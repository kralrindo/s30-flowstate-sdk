//=============================================================================//
//
// Purpose: fault capture for the S30 client.
//
// The engine's own handler uploads to backtrace.io (refused by the network
// guard) and keeps nothing local, so faults are caught here first: a log line
// per fault (capped) and one minidump for the first, written before the
// engine's handler runs.
//
//=============================================================================//
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#include "thirdparty/detours/include/detours.h"
#include "s30_log.h"
#include "s30_image.h"
#include "crash_s30.h"

#pragma comment(lib, "dbghelp.lib")

static volatile LONG s_nFaults = 0;
static volatile LONG s_nDumpWritten = 0;

static bool Crash_IsFault(DWORD nCode)
{
	switch (nCode)
	{
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_STACK_OVERFLOW:
	case EXCEPTION_IN_PAGE_ERROR:
	case STATUS_STACK_BUFFER_OVERRUN:
	case STATUS_HEAP_CORRUPTION:
		return true;
	default:
		return false;
	}
}

static void Crash_WriteDump(PEXCEPTION_POINTERS pInfo)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char szPath[MAX_PATH];
	snprintf(szPath, sizeof(szPath), "platform\\logs\\client_s30\\%04u%02u%02u_%02u%02u%02u.dmp",
		st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	const HANDLE hFile = CreateFileA(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;
	MINIDUMP_EXCEPTION_INFORMATION mei = {};
	mei.ThreadId = GetCurrentThreadId();
	mei.ExceptionPointers = pInfo;
	mei.ClientPointers = FALSE;
	const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo
		| MiniDumpWithUnloadedModules | MiniDumpWithDataSegs);
	const BOOL bOk = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type, &mei, NULL, NULL);
	CloseHandle(hFile);
	S30Log("[CRASH] minidump %s: %s\n", bOk ? "written" : "FAILED", szPath);
}

static LONG CALLBACK Crash_VectoredHandler(PEXCEPTION_POINTERS pInfo)
{
	const DWORD nCode = pInfo->ExceptionRecord->ExceptionCode;
	if (!Crash_IsFault(nCode))
		return EXCEPTION_CONTINUE_SEARCH;

	const LONG nFault = InterlockedIncrement(&s_nFaults);
	if (nFault > 16)
		return EXCEPTION_CONTINUE_SEARCH;

	const CONTEXT* const pCtx = pInfo->ContextRecord;
	char szWhere[MAX_PATH];
	S30Log_FormatAddress(pInfo->ExceptionRecord->ExceptionAddress, szWhere, sizeof(szWhere));
	S30Log("[CRASH] #%ld code=0x%08lX at %s info=[0x%llX, 0x%llX]\n", nFault, nCode, szWhere,
		static_cast<unsigned long long>(pInfo->ExceptionRecord->ExceptionInformation[0]),
		static_cast<unsigned long long>(pInfo->ExceptionRecord->ExceptionInformation[1]));
	S30Log("[CRASH]   rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\n", pCtx->Rax, pCtx->Rbx, pCtx->Rcx, pCtx->Rdx);
	S30Log("[CRASH]   rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\n", pCtx->Rsi, pCtx->Rdi, pCtx->Rbp, pCtx->Rsp);
	S30Log("[CRASH]   r8 =%016llX r9 =%016llX r10=%016llX r11=%016llX\n", pCtx->R8, pCtx->R9, pCtx->R10, pCtx->R11);
	S30Log("[CRASH]   r12=%016llX r13=%016llX r14=%016llX r15=%016llX\n", pCtx->R12, pCtx->R13, pCtx->R14, pCtx->R15);

	// Return-address candidates: stack qwords pointing into the host image.
	// A heuristic scan, not an unwind; it is enough to find the callers in IDA.
	const uintptr_t nBase = S30Image_Base();
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase)->e_lfanew);
	const uintptr_t nEnd = nBase + pNt->OptionalHeader.SizeOfImage;
	const uintptr_t* pStack = reinterpret_cast<const uintptr_t*>(pCtx->Rsp);
	char szLine[1024];
	int nPos = 0;
	int nFound = 0;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(pStack, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT)
	{
		const uintptr_t nStackEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
		for (int i = 0; i < 512 && reinterpret_cast<uintptr_t>(pStack + i + 1) <= nStackEnd && nFound < 16; ++i)
		{
			const uintptr_t v = pStack[i];
			if (v > nBase + 0x1000 && v < nEnd)
			{
				nPos += snprintf(szLine + nPos, sizeof(szLine) - nPos, " 0x%llX", static_cast<unsigned long long>(v - nBase));
				++nFound;
			}
		}
	}
	S30Log("[CRASH]   stack exe refs:%s\n", nFound ? szLine : " (none)");

	if (InterlockedCompareExchange(&s_nDumpWritten, 1, 0) == 0)
		Crash_WriteDump(pInfo);
	return EXCEPTION_CONTINUE_SEARCH;
}

//-----------------------------------------------------------------------------
// Exit paths: an engine Error() ends in a message box and/or ExitProcess or
// TerminateProcess without any fault. Log who asked, and what it said.
//-----------------------------------------------------------------------------
static void Crash_LogExeStack(const char* pszTag)
{
	void* pFrames[48];
	const USHORT nFrames = RtlCaptureStackBackTrace(1, 48, pFrames, NULL);
	const uintptr_t nBase = S30Image_Base();
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase)->e_lfanew);
	const uintptr_t nEnd = nBase + pNt->OptionalHeader.SizeOfImage;
	char szLine[1024];
	int nPos = 0;
	for (USHORT i = 0; i < nFrames && nPos < 900; ++i)
	{
		const uintptr_t v = reinterpret_cast<uintptr_t>(pFrames[i]);
		if (v >= nBase && v < nEnd)
			nPos += snprintf(szLine + nPos, sizeof(szLine) - nPos, " 0x%llX", static_cast<unsigned long long>(v - nBase));
	}
	S30Log("[%s]   exe frames:%s\n", pszTag, nPos ? szLine : " (none)");
}

static decltype(&ExitProcess) v_ExitProcess = ExitProcess;
static decltype(&TerminateProcess) v_TerminateProcess = TerminateProcess;
static decltype(&MessageBoxA) v_MessageBoxA = MessageBoxA;
static decltype(&MessageBoxW) v_MessageBoxW = MessageBoxW;

static void WINAPI h_ExitProcess(UINT nExitCode)
{
	S30Log("[EXIT] ExitProcess(%u)\n", nExitCode);
	Crash_LogExeStack("EXIT");
	v_ExitProcess(nExitCode);
}

static BOOL WINAPI h_TerminateProcess(HANDLE hProcess, UINT nExitCode)
{
	if (hProcess == GetCurrentProcess() || GetProcessId(hProcess) == GetCurrentProcessId())
	{
		S30Log("[EXIT] TerminateProcess(self, %u)\n", nExitCode);
		Crash_LogExeStack("EXIT");
	}
	return v_TerminateProcess(hProcess, nExitCode);
}

static int WINAPI h_MessageBoxA(HWND hWnd, LPCSTR pszText, LPCSTR pszCaption, UINT nType)
{
	S30Log("[MSGBOX] \"%s\": %s\n", pszCaption ? pszCaption : "", pszText ? pszText : "");
	Crash_LogExeStack("MSGBOX");
	return v_MessageBoxA(hWnd, pszText, pszCaption, nType);
}

static int WINAPI h_MessageBoxW(HWND hWnd, LPCWSTR pwszText, LPCWSTR pwszCaption, UINT nType)
{
	char szText[1024] = {};
	char szCaption[256] = {};
	if (pwszText)
		WideCharToMultiByte(CP_UTF8, 0, pwszText, -1, szText, sizeof(szText) - 1, NULL, NULL);
	if (pwszCaption)
		WideCharToMultiByte(CP_UTF8, 0, pwszCaption, -1, szCaption, sizeof(szCaption) - 1, NULL, NULL);
	S30Log("[MSGBOX] \"%s\": %s\n", szCaption, szText);
	Crash_LogExeStack("MSGBOX");
	return v_MessageBoxW(hWnd, pwszText, pwszCaption, nType);
}

void S30Crash_Install(void)
{
	AddVectoredExceptionHandler(1, Crash_VectoredHandler);

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ExitProcess), h_ExitProcess);
	DetourAttach(&reinterpret_cast<PVOID&>(v_TerminateProcess), h_TerminateProcess);
	DetourAttach(&reinterpret_cast<PVOID&>(v_MessageBoxA), h_MessageBoxA);
	DetourAttach(&reinterpret_cast<PVOID&>(v_MessageBoxW), h_MessageBoxW);
	const LONG nError = DetourTransactionCommit();
	S30Log("[CRASH] fault capture installed, exit/msgbox hooks %s (commit %ld)\n", nError == NO_ERROR ? "armed" : "FAILED", nError);
}
