//=============================================================================//
//
// Purpose: S30 ICvar access and engine ConCommand registration.
//
// Layouts verified on S30.1 (docs/08):
//   ConCommandBase (0x30): vtable, m_pNext +0x08, name +0x10, help +0x18,
//     usage +0x20, flags +0x28, registered bit in byte +0x2C (bit 0). S21 kept
//     m_bRegistered at +0x10; S30 dropped it there, so name/help/flags moved -8.
//   ConCommand (0x58): supplemental-finish +0x30 (cmd, +0x38), callback +0x40,
//     completion +0x48, callback kind byte +0x50 (2 = fn(const CCommand&),
//     4 = interface, else fn(void)) -- from ConCommand::Dispatch 0x5D9200.
//   CCommand: argc +0, argv0 size +8, args string +0x10, argv[] +0x410 (the
//     connect handler 0x2F0EF0 reads +8 and +0x10 the same way; same as S21).
//   ICvar (CCvar vtable 0x1827BC0): 8 AllocateDLLIdentifier, 9 RegisterConCommand
//     (sets the +0x2C bit, links m_pNext into the list at cvar+80, hashes the
//     name at cvar+88), 13/14 FindCommandBase, 15/16 FindVar, 17/18 FindCommand.
// g_pCVar is read from ConVar_RegisterStatics (0x5D8B50), the same function
// S21's SDK signature finds; it walks the static list with the same vtable
// calls on both builds.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "s30_log.h"
#include "s30_pattern.h"
#include "sdk_commands_s30.h"
#include "concommand_s30.h"

static const int FCVAR_DEVELOPMENTONLY = 1 << 1;
static const int FCVAR_CLIENTDLL = 1 << 3;
static const int FCVAR_HIDDEN = 1 << 4;
static const int FCVAR_RELEASE = 1 << 19;

static const int s_nConCommandSize = 0x58;
static const int s_nOffNext = 0x08;
static const int s_nOffName = 0x10;
static const int s_nOffHelp = 0x18;
static const int s_nOffUsage = 0x20;
static const int s_nOffFlags = 0x28;
static const int s_nOffRegistered = 0x2C;
static const int s_nOffSuppFinish = 0x30;
static const int s_nOffSuppData = 0x38;
static const int s_nOffCallback = 0x40;
static const int s_nOffCompletion = 0x48;
static const int s_nOffCallbackKind = 0x50;
static const int s_nCvarOffListHead = 80;

static const int s_nCCommandOffArgv0Size = 0x08;
static const int s_nCCommandOffArgS = 0x10;
static const int s_nCCommandOffArgv = 0x410;

static const char* const s_pszPatternRegisterStatics =
	"48 83 EC 28 8B C1 48 8B 0D ?? ?? ?? ?? 48 85 C9 0F 84 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 "
	"0F 85 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? 48 8B 01 48 89 7C 24 20 C6 05 ?? ?? ?? ?? 01 FF 50 40";

typedef void (*RegisterConCommand_t)(void* pCvar, void* pCommand);
typedef void* (*FindCommandBase_t)(void* pCvar, const char* pszName);

static void** s_ppCvar = nullptr;            // &g_pCVar
static const uint8_t* s_pTemplate = nullptr; // the engine's static "echo" ConCommand
static bool s_bInit = false;

// SDK commands become engine commands one object each; fixed storage.
static const int s_nMaxEngineCommands = 64;
static uint8_t s_engineCommands[s_nMaxEngineCommands][s_nConCommandSize];
static int s_nEngineCommands = 0;

static void* ConCommand_Cvar(void)
{
	return s_ppCvar ? *s_ppCvar : nullptr;
}

static void* ConCommand_VFunc(void* pObject, int nIndex)
{
	return (*reinterpret_cast<void***>(pObject))[nIndex];
}

// Finds the static ConCommand object of an engine command by its name string.
static const uint8_t* ConCommand_FindStatic(const char* pszName)
{
	const uintptr_t nBase = S30Image_Base();
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase)->e_lfanew);
	const IMAGE_SECTION_HEADER* pSecs = IMAGE_FIRST_SECTION(pNt);
	const IMAGE_SECTION_HEADER* pRdata = nullptr;
	const IMAGE_SECTION_HEADER* pData = nullptr;
	for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i)
	{
		if (!memcmp(pSecs[i].Name, ".rdata", 6))
			pRdata = &pSecs[i];
		else if (!memcmp(pSecs[i].Name, ".data", 5))
			pData = &pSecs[i];
	}
	if (!pRdata || !pData)
		return nullptr;

	const size_t nNameLen = strlen(pszName) + 1;
	const uint8_t* const pR = reinterpret_cast<const uint8_t*>(nBase + pRdata->VirtualAddress);
	for (size_t off = 1; off + nNameLen < pRdata->Misc.VirtualSize; ++off)
	{
		if (pR[off - 1] != 0 || memcmp(pR + off, pszName, nNameLen) != 0)
			continue;
		const uintptr_t nNameVA = reinterpret_cast<uintptr_t>(pR + off);
		const uint8_t* const pD = reinterpret_cast<const uint8_t*>(nBase + pData->VirtualAddress);
		const size_t nDataSize = pData->SizeOfRawData;
		for (size_t d = s_nOffName; d + 8 <= nDataSize; d += 8)
		{
			if (*reinterpret_cast<const uintptr_t*>(pD + d) != nNameVA)
				continue;
			const uint8_t* const pObject = pD + d - s_nOffName;
			const uintptr_t nVtable = *reinterpret_cast<const uintptr_t*>(pObject);
			// A ConCommand's vtable lives in .rdata.
			if (nVtable >= nBase + pRdata->VirtualAddress && nVtable < nBase + pRdata->VirtualAddress + pRdata->Misc.VirtualSize)
				return pObject;
		}
	}
	return nullptr;
}

bool S30ConCommand_Init(void)
{
	int nMatches = 0;
	const uint8_t* const pRegisterStatics = S30Pattern_FindUnique(s_pszPatternRegisterStatics, &nMatches);
	if (!pRegisterStatics)
	{
		S30Log("[CVAR] ConVar_RegisterStatics unresolved (matches=%d) -- no engine commands\n", nMatches);
		return false;
	}
	// mov rcx, [rip+g_pCVar] at +6 (rel32 at +9, next instruction +13).
	int32_t nRel = 0;
	memcpy(&nRel, pRegisterStatics + 9, sizeof(nRel));
	s_ppCvar = reinterpret_cast<void**>(const_cast<uint8_t*>(pRegisterStatics) + 13 + nRel);

	s_pTemplate = ConCommand_FindStatic("echo");
	if (!s_pTemplate)
	{
		S30Log("[CVAR] template ConCommand (echo) not found -- no engine commands\n");
		return false;
	}
	s_bInit = true;
	S30Log("[CVAR] g_pCVar @ 0x%llX, ConCommand vtable 0x%llX (from echo @ 0x%llX)\n",
		static_cast<unsigned long long>(S30Pattern_Rva(s_ppCvar)),
		static_cast<unsigned long long>(S30Pattern_Rva(*reinterpret_cast<void* const*>(s_pTemplate))),
		static_cast<unsigned long long>(S30Pattern_Rva(s_pTemplate)));
	return true;
}

void* S30ConCommand_FindCommandBase(const char* pszName)
{
	void* const pCvar = ConCommand_Cvar();
	if (!s_bInit || !pCvar)
		return nullptr;
	return reinterpret_cast<FindCommandBase_t>(ConCommand_VFunc(pCvar, 13))(pCvar, pszName);
}

bool S30ConCommand_IsReady(void)
{
	return S30ConCommand_FindCommandBase("echo") != nullptr;
}

int S30ConCommand_GetFlags(const char* pszName)
{
	const uint8_t* const pBase = static_cast<const uint8_t*>(S30ConCommand_FindCommandBase(pszName));
	return pBase ? *reinterpret_cast<const int*>(pBase + s_nOffFlags) : -1;
}

int S30ConCommand_ClearFlags(const char* pszName, int nClear)
{
	uint8_t* const pBase = static_cast<uint8_t*>(S30ConCommand_FindCommandBase(pszName));
	if (!pBase)
		return -1;
	int& nFlags = *reinterpret_cast<int*>(pBase + s_nOffFlags);
	const int nOld = nFlags;
	nFlags &= ~nClear;
	return nOld;
}

int S30ConCommand_UnlockDevelopmentOnly(void)
{
	uint8_t* const pCvar = static_cast<uint8_t*>(ConCommand_Cvar());
	if (!s_bInit || !pCvar)
		return 0;
	int nChanged = 0;
	int nGuard = 0;
	for (uint8_t* p = *reinterpret_cast<uint8_t**>(pCvar + s_nCvarOffListHead); p && nGuard < 100000;
		p = *reinterpret_cast<uint8_t**>(p + s_nOffNext), ++nGuard)
	{
		int& nFlags = *reinterpret_cast<int*>(p + s_nOffFlags);
		if (nFlags & (FCVAR_DEVELOPMENTONLY | FCVAR_HIDDEN))
		{
			nFlags &= ~(FCVAR_DEVELOPMENTONLY | FCVAR_HIDDEN);
			++nChanged;
		}
	}
	S30Log("[CVAR] unlocked %d of %d commands/convars (cleared DEVELOPMENTONLY/HIDDEN)\n", nChanged, nGuard);
	return nChanged;
}

// One callback for every SDK engine command: rebuild "name args" from the
// CCommand and hand it to the SDK dispatcher.
static void ConCommand_Callback(const uint8_t* pCommand)
{
	const char* const* ppArgv = reinterpret_cast<const char* const*>(pCommand + s_nCCommandOffArgv);
	const long long nArgv0Size = *reinterpret_cast<const long long*>(pCommand + s_nCCommandOffArgv0Size);
	const char* const pszArgS = reinterpret_cast<const char*>(pCommand + s_nCCommandOffArgS);
	char szLine[1024];
	snprintf(szLine, sizeof(szLine), "%s %s", ppArgv[0] ? ppArgv[0] : "", nArgv0Size ? pszArgS + nArgv0Size : "");
	S30Commands_Dispatch(szLine);
}

static bool ConCommand_RegisterOne(const char* pszName, const char* pszHelp)
{
	void* const pCvar = ConCommand_Cvar();
	if (s_nEngineCommands >= s_nMaxEngineCommands || !pCvar)
		return false;
	if (S30ConCommand_FindCommandBase(pszName))
		return false; // the engine (or an earlier call) already has it

	uint8_t* const pObject = s_engineCommands[s_nEngineCommands];
	memset(pObject, 0, s_nConCommandSize);
	memcpy(pObject, s_pTemplate, sizeof(void*));                                                  // ConCommand vtable
	*reinterpret_cast<const char**>(pObject + s_nOffName) = pszName;
	*reinterpret_cast<const char**>(pObject + s_nOffHelp) = pszHelp;
	*reinterpret_cast<int*>(pObject + s_nOffFlags) = FCVAR_RELEASE | FCVAR_CLIENTDLL;
	memcpy(pObject + s_nOffSuppFinish, s_pTemplate + s_nOffSuppFinish, sizeof(void*));            // engine no-op
	memcpy(pObject + s_nOffCompletion, s_pTemplate + s_nOffCompletion, sizeof(void*));            // "no completions"
	*reinterpret_cast<void**>(pObject + s_nOffCallback) = reinterpret_cast<void*>(&ConCommand_Callback);
	pObject[s_nOffCallbackKind] = 2;                                                              // fn(const CCommand&)

	reinterpret_cast<RegisterConCommand_t>(ConCommand_VFunc(pCvar, 9))(pCvar, pObject);
	if (!(pObject[s_nOffRegistered] & 1))
	{
		S30Log("[CVAR] RegisterConCommand did not take %s\n", pszName);
		return false;
	}
	++s_nEngineCommands;
	return true;
}

int S30ConCommand_RegisterSdkCommands(void)
{
	if (!s_bInit || !S30ConCommand_IsReady())
		return 0;
	const char* ppszNames[64];
	const char* ppszHelp[64];
	const int nCommands = S30Commands_List(ppszNames, ppszHelp, 64);
	int nRegistered = 0;
	for (int i = 0; i < nCommands; ++i)
	{
		if (ConCommand_RegisterOne(ppszNames[i], ppszHelp[i]))
			++nRegistered;
	}
	if (nRegistered)
		S30Log("[CVAR] registered %d SDK commands with the engine\n", nRegistered);
	return nRegistered;
}
