//=============================================================================//
//
// Purpose: capture Squirrel print()/printt() output on the S30 client.
//
// Retail registers print callbacks that only format into a per-VM capture
// buffer, so script output never reaches a console. S21 has one callback
// (DX12 0xE286C0, the SDK's v_Script_PrintFunc); S30.1 has two, installed by
// the VM setup at 0xF575F0 under "PrintFunc" / "PrintFunc_Client" /
// "PrintFunc_UI":
//   0xF5B1D0  SERVER and CLIENT VMs, VM index at *(uint8*)(v + 0x14)
//   0xF5B2B0  UI VM (index goes through a VM table first)
// Both are (HSQUIRRELVM v, const char* fmt, ...). Their first 0x6D bytes are
// identical, so each signature reaches into the body where they diverge.
// The S21 signature misses on S30: S21 has xor where S30 has mov r9, rbx.
//
// Script errors take another callback. The error handler (sqstd_aux_printerror,
// S30.1 0xF61690) writes "SCRIPT ERROR: ...", the source line, CALLSTACK and
// LOCALS in pieces through *(table[vm] + 17024), which the VM setup fills with
//   0xF5B3B0  SERVER and CLIENT VMs
//   0xF5B420  UI VM
// (HSQUIRRELVM v, const char* text): they only append to the per-VM capture
// buffer the error dialog reads. Retail logs none of it; S21 routes the same
// text through its print callback, which the SDK flags as an error.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include "thirdparty/detours/include/detours.h"
#include "s30_log.h"
#include "s30_pattern.h"
#include "console_s30.h"
#include "script_print_s30.h"

typedef __int64 (*ScriptPrint_t)(void* v, const char* pszFormat, ...);
static void ScriptPrint_HookPrintlNative(void);

#define SCRIPT_PRINT_COMMON \
	"48 8B C4 48 89 50 10 4C 89 40 18 4C 89 48 20 53 56 57 48 81 EC 30 08 00 00 48 8B DA 48 8D 70 18 " \
	"48 8B F9 E8 ?? ?? ?? ?? 48 89 74 24 28 48 8D 54 24 30 4C 8B CB 48 C7 44 24 20 00 00 00 00 41 B8 " \
	"00 08 00 00 48 8B 08 48 83 C9 01 E8 ?? ?? ?? ?? 85 C0 B9 FF FF FF FF 0F 48 C1 3D FF 07 00 00 76 " \
	"08 C6 84 24 2F 08 00 00 00 0F B6 47 14 "

static const char* const s_pszPatternPrintClient =
	SCRIPT_PRINT_COMMON "4C 8D 0D ?? ?? ?? ?? 48 69 D0 02 20 00 00";
static const char* const s_pszPatternPrintUI =
	SCRIPT_PRINT_COMMON "48 8D 0D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 48 8B 04 C1";

static ScriptPrint_t v_ScriptPrintClient = nullptr;
static ScriptPrint_t v_ScriptPrintUI = nullptr;
static uint8_t* s_pPrintClientTarget = nullptr;
static uint8_t* s_pPrintUITarget = nullptr;
// Per-VM shared-state table (qword_1440F48C8 on S30.1). printl calls
// *(table[vm] + 17016) when it is set; the VM setup stores the print callback there.
static uintptr_t* s_pVMStateTable = nullptr;
static const int s_nPrintFuncSlot = 17016;
static volatile LONG s_nClientCalls = 0;
static volatile LONG s_nUICalls = 0;

static void ScriptPrint_Emit(const char* pszVM, const char* pszText)
{
	const size_t nLen = strlen(pszText);
	const bool bNewline = nLen && pszText[nLen - 1] == '\n';
	S30LogFile("[SCRIPT][%s] %s%s", pszVM, pszText, bNewline ? "" : "\n");

	char szLine[2200];
	snprintf(szLine, sizeof(szLine), "[%s] %s%s", pszVM, pszText, bNewline ? "" : "\n");
	S30Console_Write(szLine, S30ConsoleColor_t::SCRIPT);
}

static __int64 h_ScriptPrintClient(void* v, const char* pszFormat, ...)
{
	char szText[2048];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szText, sizeof(szText), pszFormat ? pszFormat : "", args);
	va_end(args);

	InterlockedIncrement(&s_nClientCalls);
	const unsigned char nVM = *(reinterpret_cast<const unsigned char*>(v) + 0x14);
	ScriptPrint_Emit(nVM == 0 ? "SERVER" : nVM == 1 ? "CLIENT" : "VM?", szText);
	return v_ScriptPrintClient(v, "%s", szText);
}

static __int64 h_ScriptPrintUI(void* v, const char* pszFormat, ...)
{
	char szText[2048];
	va_list args;
	va_start(args, pszFormat);
	vsnprintf(szText, sizeof(szText), pszFormat ? pszFormat : "", args);
	va_end(args);

	InterlockedIncrement(&s_nUICalls);
	ScriptPrint_Emit("UI", szText);
	return v_ScriptPrintUI(v, "%s", szText);
}

//-----------------------------------------------------------------------------
// Script error text
//-----------------------------------------------------------------------------
typedef void (*ScriptErrorText_t)(void* v, const char* pszText);

static const char* const s_pszPatternErrorClient =
	"0F B6 41 14 4C 8D 15 ?? ?? ?? ?? 4C 69 C8 02 20 00 00 43 80 BC 11 00 20 00 00 00 74 ?? 48 85 D2";
static const char* const s_pszPatternErrorUI =
	"0F B6 41 14 4C 8D 15 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? 48 8B 04 C1 0F B6 88 08 43 00 00 4C 69 C9 02 20 00 00 "
	"43 80 BC 11 00 20 00 00 00 74";

static ScriptErrorText_t v_ScriptErrorClient = nullptr;
static ScriptErrorText_t v_ScriptErrorUI = nullptr;

// The handler writes a line in several calls; lines are logged whole, per VM.
struct ScriptErrorLine_t
{
	SRWLOCK lock = SRWLOCK_INIT;
	char szLine[2048] = {};
	size_t nLen = 0;
};
static ScriptErrorLine_t s_errorLines[3]; // SERVER, CLIENT, UI

static void ScriptError_Append(int nVM, const char* pszVM, const char* pszText)
{
	ScriptErrorLine_t& line = s_errorLines[nVM];
	AcquireSRWLockExclusive(&line.lock);
	for (const char* p = pszText; *p; ++p)
	{
		if (*p != '\n' && line.nLen < sizeof(line.szLine) - 1)
		{
			line.szLine[line.nLen++] = *p;
			continue;
		}
		if (*p != '\n')
			continue; // overlong line: the rest up to its newline is cut
		line.szLine[line.nLen] = '\0';
		S30LogFile("[SCRIPT-ERROR][%s] %s\n", pszVM, line.szLine);
		char szConsole[2100];
		snprintf(szConsole, sizeof(szConsole), "[%s] %s\n", pszVM, line.szLine);
		S30Console_Write(szConsole, S30ConsoleColor_t::FAULT);
		line.nLen = 0;
	}
	ReleaseSRWLockExclusive(&line.lock);
}

static void h_ScriptErrorClient(void* v, const char* pszText)
{
	const unsigned char nVM = *(reinterpret_cast<const unsigned char*>(v) + 0x14);
	if (pszText)
		ScriptError_Append(nVM == 0 ? 0 : 1, nVM == 0 ? "SERVER" : nVM == 1 ? "CLIENT" : "VM?", pszText);
	v_ScriptErrorClient(v, pszText);
}

static void h_ScriptErrorUI(void* v, const char* pszText)
{
	if (pszText)
		ScriptError_Append(2, "UI", pszText);
	v_ScriptErrorUI(v, pszText);
}

static void ScriptPrint_HookErrorText(void)
{
	int nMatchesClient = 0;
	int nMatchesUI = 0;
	v_ScriptErrorClient = reinterpret_cast<ScriptErrorText_t>(S30Pattern_FindUnique(s_pszPatternErrorClient, &nMatchesClient));
	v_ScriptErrorUI = reinterpret_cast<ScriptErrorText_t>(S30Pattern_FindUnique(s_pszPatternErrorUI, &nMatchesUI));
	if (!v_ScriptErrorClient || !v_ScriptErrorUI)
	{
		S30Log("[SCRIPT-PRINT] error callbacks unresolved (client matches=%d, ui matches=%d) -- script errors not captured\n",
			nMatchesClient, nMatchesUI);
		return;
	}
	const uintptr_t nClientRva = S30Pattern_Rva(reinterpret_cast<void*>(v_ScriptErrorClient));
	const uintptr_t nUIRva = S30Pattern_Rva(reinterpret_cast<void*>(v_ScriptErrorUI));
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ScriptErrorClient), h_ScriptErrorClient);
	DetourAttach(&reinterpret_cast<PVOID&>(v_ScriptErrorUI), h_ScriptErrorUI);
	const LONG nError = DetourTransactionCommit();
	S30Log("[SCRIPT-PRINT] error text client/server @ 0x%llX, UI @ 0x%llX %s (commit %ld)\n",
		static_cast<unsigned long long>(nClientRva), static_cast<unsigned long long>(nUIRva),
		nError == NO_ERROR ? "hooked" : "FAILED", nError);
}

bool S30ScriptPrint_Install(void)
{
	ScriptPrint_HookErrorText();

	int nMatchesClient = 0;
	int nMatchesUI = 0;
	v_ScriptPrintClient = reinterpret_cast<ScriptPrint_t>(S30Pattern_FindUnique(s_pszPatternPrintClient, &nMatchesClient));
	v_ScriptPrintUI = reinterpret_cast<ScriptPrint_t>(S30Pattern_FindUnique(s_pszPatternPrintUI, &nMatchesUI));
	if (!v_ScriptPrintClient || !v_ScriptPrintUI)
	{
		S30Log("[SCRIPT-PRINT] unresolved (client matches=%d, ui matches=%d) -- script output not captured\n",
			nMatchesClient, nMatchesUI);
		return false;
	}

	s_pPrintClientTarget = reinterpret_cast<uint8_t*>(v_ScriptPrintClient);
	s_pPrintUITarget = reinterpret_cast<uint8_t*>(v_ScriptPrintUI);
	// UI body: lea rcx, [rip+table] at +0x6D (rel32 at +0x70, next instruction at +0x74).
	int32_t nTableRel = 0;
	memcpy(&nTableRel, s_pPrintUITarget + 0x70, sizeof(nTableRel));
	s_pVMStateTable = reinterpret_cast<uintptr_t*>(s_pPrintUITarget + 0x74 + nTableRel);
	S30Log("[SCRIPT-PRINT] VM state table @ 0x%llX\n", static_cast<unsigned long long>(S30Pattern_Rva(s_pVMStateTable)));

	const uintptr_t nClientRva = S30Pattern_Rva(reinterpret_cast<void*>(v_ScriptPrintClient));
	const uintptr_t nUIRva = S30Pattern_Rva(reinterpret_cast<void*>(v_ScriptPrintUI));
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ScriptPrintClient), h_ScriptPrintClient);
	DetourAttach(&reinterpret_cast<PVOID&>(v_ScriptPrintUI), h_ScriptPrintUI);
	const LONG nError = DetourTransactionCommit();
	S30Log("[SCRIPT-PRINT] client/server @ 0x%llX, UI @ 0x%llX %s (commit %ld)\n",
		static_cast<unsigned long long>(nClientRva), static_cast<unsigned long long>(nUIRva),
		nError == NO_ERROR ? "hooked" : "FAILED", nError);
	ScriptPrint_HookPrintlNative();
	return nError == NO_ERROR;
}

//-----------------------------------------------------------------------------
// Diagnostic: count calls to the printl native (Squirrel base library). Found
// through its registration entry {name, function, ...} in .data, which points
// at the "printl" string in .rdata -- no hard-coded address.
//-----------------------------------------------------------------------------
typedef __int64 (*SQNative_t)(void* v);
static SQNative_t v_PrintlNative = nullptr;
static volatile LONG s_nPrintlCalls = 0;

static __int64 h_PrintlNative(void* v)
{
	InterlockedIncrement(&s_nPrintlCalls);
	return v_PrintlNative(v);
}

static uint8_t* ScriptPrint_FindInSection(const char* pszSection, const void* pNeedle, size_t nLen)
{
	const uintptr_t nBase = S30Image_Base();
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase)->e_lfanew);
	const IMAGE_SECTION_HEADER* pSec = IMAGE_FIRST_SECTION(pNt);
	for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i, ++pSec)
	{
		if (strncmp(reinterpret_cast<const char*>(pSec->Name), pszSection, 8) != 0)
			continue;
		uint8_t* const pStart = reinterpret_cast<uint8_t*>(nBase + pSec->VirtualAddress);
		const size_t nSize = pSec->SizeOfRawData < pSec->Misc.VirtualSize ? pSec->SizeOfRawData : pSec->Misc.VirtualSize;
		for (size_t off = 0; off + nLen <= nSize; ++off)
		{
			if (!memcmp(pStart + off, pNeedle, nLen))
				return pStart + off;
		}
	}
	return nullptr;
}

static void ScriptPrint_HookPrintlNative(void)
{
	static const char s_szName[] = "\0printl";
	uint8_t* const pName = ScriptPrint_FindInSection(".rdata", s_szName, sizeof(s_szName)); // includes both NULs
	if (!pName)
		return;
	const uintptr_t nNameVA = reinterpret_cast<uintptr_t>(pName + 1);
	uint8_t* const pEntry = ScriptPrint_FindInSection(".data", &nNameVA, sizeof(nNameVA));
	if (!pEntry)
		return;
	v_PrintlNative = *reinterpret_cast<SQNative_t*>(pEntry + 8);
	const uintptr_t nRva = S30Pattern_Rva(reinterpret_cast<void*>(v_PrintlNative));
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_PrintlNative), h_PrintlNative);
	const LONG nError = DetourTransactionCommit();
	S30Log("[SCRIPT-PRINT] diag: printl native @ 0x%llX counted (commit %ld)\n", static_cast<unsigned long long>(nRva), nError);
}

void S30ScriptPrint_Diag(void)
{
	S30LogFile("[SCRIPT-PRINT] printl native calls so far: %ld\n", s_nPrintlCalls);
	if (!s_pVMStateTable)
		return;
	for (int nVM = 0; nVM < 3; ++nVM)
	{
		const uintptr_t nState = s_pVMStateTable[nVM];
		const uintptr_t nPrint = nState ? *reinterpret_cast<const uintptr_t*>(nState + s_nPrintFuncSlot) : 0;
		const char* pszWhich = !nPrint ? "unset"
			: nPrint == reinterpret_cast<uintptr_t>(s_pPrintClientTarget) ? "client/server print (hooked)"
			: nPrint == reinterpret_cast<uintptr_t>(s_pPrintUITarget) ? "UI print (hooked)" : "OTHER";
		S30LogFile("[SCRIPT-PRINT] vm%d state=%p print=0x%llX %s\n", nVM, reinterpret_cast<void*>(nState),
			static_cast<unsigned long long>(nPrint ? S30Pattern_Rva(reinterpret_cast<void*>(nPrint)) : 0), pszWhich);
	}
	S30LogFile("[SCRIPT-PRINT] calls: client/server=%ld ui=%ld\n", s_nClientCalls, s_nUICalls);
}
