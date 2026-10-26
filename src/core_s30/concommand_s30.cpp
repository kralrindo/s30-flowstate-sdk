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
#include <cstdlib>
#include <cstring>
#include "s30_log.h"
#include "s30_pattern.h"
#include "sdk_commands_s30.h"
#include "concommand_s30.h"
#include "bridge_handshake_s30.h"

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

// The engine's `connect` routed through the bridge, as the S21 client's
// Hook_Connect_Worker does: `connect <host[:port]>` runs the S3 handshake, which
// then hands the connection to the engine with its own `connect` (passed through).
typedef void (*CommandCallback_t)(const uint8_t* pCommand);
static CommandCallback_t s_pfnEngineConnect = nullptr;

static void ConCommand_Connect(const uint8_t* pCommand)
{
	// The handler (0x2F0EF0) reads the text after the command name, not argc/argv
	// (argc is -1 here): args string +0x10, offset by argv0's size (+8).
	const long long nArgv0Size = *reinterpret_cast<const long long*>(pCommand + s_nCCommandOffArgv0Size);
	const char* pszArg = nArgv0Size > 0 && nArgv0Size < 512
		? reinterpret_cast<const char*>(pCommand + s_nCCommandOffArgS) + nArgv0Size : "";
	while (*pszArg == ' ' || *pszArg == '\t')
		++pszArg;
	char szAddress[256];
	size_t n = 0;
	while (pszArg[n] && pszArg[n] != ' ' && pszArg[n] != '\t' && n + 1 < sizeof(szAddress))
	{
		szAddress[n] = pszArg[n];
		++n;
	}
	szAddress[n] = '\0';
	if (S30Bridge_TakeInternalConnect() || !szAddress[0])
	{
		s_pfnEngineConnect(pCommand);
		return;
	}
	S30Log("[CVAR] connect %s -> S3 bridge handshake\n", szAddress);
	S30Bridge_Connect(szAddress);
}

bool S30ConCommand_WrapConnect(void)
{
	uint8_t* const pCommand = static_cast<uint8_t*>(S30ConCommand_FindCommandBase("connect"));
	if (!pCommand || pCommand[s_nOffCallbackKind] != 2 || !*reinterpret_cast<void**>(pCommand + s_nOffCallback))
	{
		S30Log("[CVAR] engine connect not wrapped (%s) -- use bridge_connect\n", pCommand ? "callback kind" : "not found");
		return false;
	}
	if (s_pfnEngineConnect)
		return true;
	s_pfnEngineConnect = *reinterpret_cast<CommandCallback_t*>(pCommand + s_nOffCallback);
	*reinterpret_cast<void**>(pCommand + s_nOffCallback) = reinterpret_cast<void*>(&ConCommand_Connect);
	S30Log("[CVAR] engine connect (callback 0x%llX) routed through the S3 bridge\n",
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<void*>(s_pfnEngineConnect))));
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

// ConVar (0x90), from the inlined constructor of the static "developer" (0x7E5D0):
// the ConCommandBase above, IConVar vtable +0x30, parent +0x38 (itself), default
// +0x40, value string +0x48 (a g_pMemAlloc buffer, as the engine reallocates it on
// a longer value), its size +0x50, float +0x58, int +0x5C, min/max +0x60..+0x6F,
// change callbacks +0x70 (CUtlVector, zero is empty). Both vtables are copied from
// the live "developer".
static const int s_nConVarSize = 0x90;
static const int s_nOffConVarVtable = 0x30;
static const int s_nOffConVarParent = 0x38;
static const int s_nOffConVarDefault = 0x40;
static const int s_nOffConVarString = 0x48;
static const int s_nOffConVarStringSize = 0x50;
static const int s_nOffConVarFloat = 0x58;
static const int s_nOffConVarInt = 0x5C;

// mov rax, [g_pMemAlloc] / test / jnz / call CreateGlobalMemAlloc / mov [g_pMemAlloc], rax
// (inlined at every allocation; all sites name the same pair).
static const char* const s_pszPatternMemAlloc = "48 8B 05 ?? ?? ?? ?? 48 85 C0 75 ?? E8 ?? ?? ?? ?? 48 89 05";

static const int s_nMaxEngineConVars = 32;
static uint8_t s_engineConVars[s_nMaxEngineConVars][s_nConVarSize];
static int s_nEngineConVars = 0;

typedef void* (*CreateGlobalMemAlloc_t)(void);
typedef void* (*MemAlloc_Alloc_t)(void* pAlloc, size_t nSize);

static void* ConCommand_MemAlloc(void)
{
	static void** s_ppAlloc = nullptr;
	static CreateGlobalMemAlloc_t s_pfnCreate = nullptr;
	if (!s_ppAlloc)
	{
		uint8_t* pSites[8];
		const int nSites = S30Pattern_FindAll(s_pszPatternMemAlloc, pSites, 8);
		for (int i = 0; i < nSites; ++i)
		{
			int32_t nGlobal = 0, nCall = 0;
			memcpy(&nGlobal, pSites[i] + 3, sizeof(nGlobal));
			memcpy(&nCall, pSites[i] + 13, sizeof(nCall));
			void** const ppAlloc = reinterpret_cast<void**>(pSites[i] + 7 + nGlobal);
			const auto pfnCreate = reinterpret_cast<CreateGlobalMemAlloc_t>(pSites[i] + 17 + nCall);
			if (i && (ppAlloc != s_ppAlloc || pfnCreate != s_pfnCreate))
			{
				S30Log("[CVAR] g_pMemAlloc sites disagree -- no engine convars\n");
				s_ppAlloc = nullptr;
				return nullptr;
			}
			s_ppAlloc = ppAlloc;
			s_pfnCreate = pfnCreate;
		}
		if (!s_ppAlloc)
			return nullptr;
	}
	return *s_ppAlloc ? *s_ppAlloc : s_pfnCreate();
}

bool S30ConCommand_RegisterConVar(const char* pszName, const char* pszDefault, const char* pszHelp)
{
	void* const pCvar = ConCommand_Cvar();
	if (s_nEngineConVars >= s_nMaxEngineConVars || !pCvar)
		return false;
	if (S30ConCommand_FindCommandBase(pszName))
		return false;
	const uint8_t* const pTemplate = static_cast<const uint8_t*>(S30ConCommand_FindCommandBase("developer"));
	void* const pAlloc = ConCommand_MemAlloc();
	if (!pTemplate || !pAlloc)
	{
		S30Log("[CVAR] %s: template convar or g_pMemAlloc unresolved\n", pszName);
		return false;
	}
	const size_t nSize = strlen(pszDefault) + 1;
	char* const pszValue = static_cast<char*>(reinterpret_cast<MemAlloc_Alloc_t>(ConCommand_VFunc(pAlloc, 1))(pAlloc, nSize));
	if (!pszValue)
		return false;
	memcpy(pszValue, pszDefault, nSize);

	uint8_t* const pObject = s_engineConVars[s_nEngineConVars];
	memset(pObject, 0, s_nConVarSize);
	memcpy(pObject, pTemplate, sizeof(void*));
	memcpy(pObject + s_nOffConVarVtable, pTemplate + s_nOffConVarVtable, sizeof(void*));
	*reinterpret_cast<const char**>(pObject + s_nOffName) = pszName;
	*reinterpret_cast<const char**>(pObject + s_nOffHelp) = pszHelp;
	*reinterpret_cast<int*>(pObject + s_nOffFlags) = FCVAR_RELEASE | FCVAR_CLIENTDLL;
	*reinterpret_cast<uint8_t**>(pObject + s_nOffConVarParent) = pObject;
	*reinterpret_cast<const char**>(pObject + s_nOffConVarDefault) = pszDefault;
	*reinterpret_cast<char**>(pObject + s_nOffConVarString) = pszValue;
	*reinterpret_cast<size_t*>(pObject + s_nOffConVarStringSize) = nSize;
	const float flValue = static_cast<float>(atof(pszDefault));
	*reinterpret_cast<float*>(pObject + s_nOffConVarFloat) = flValue;
	*reinterpret_cast<int*>(pObject + s_nOffConVarInt) = static_cast<int>(flValue);

	reinterpret_cast<RegisterConCommand_t>(ConCommand_VFunc(pCvar, 9))(pCvar, pObject);
	if (!(pObject[s_nOffRegistered] & 1))
	{
		S30Log("[CVAR] RegisterConCommand did not take convar %s\n", pszName);
		return false;
	}
	++s_nEngineConVars;
	return true;
}
