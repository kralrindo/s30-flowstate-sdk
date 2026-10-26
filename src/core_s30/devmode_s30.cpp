//=============================================================================//
//
// Purpose: -dev / -developer: S30's #if DEV script code and dev script lists.
//
// S30.1 decides DEV in two places, and retail switches both off:
//   - script lists (rson "DEV" conditions, resolver 0xC09DD0): the developer cvar's
//     int (> 0). -dev does not set it; +developer 1 does.
//   - #if DEV in a script: the compiler turns the identifier into its own node
//     (kind 0x38, 0xF78CF8) and the constant folder (case at 0xF86B5E) replaces it
//     with the bool constant false (movabs rcx, 0x2800000000000000).
// With only the cvar on, dev files that use #if DEV struct fields fail to compile
// ("DEV_name" is not a property of struct "LoadoutEntry", sh_dev_utility.nut), so
// both go on together: the command line gains +developer 1 (GetCommandLineA/W as
// LauncherMain reads them) and the folded constant becomes true. The S21 client
// had the same split and forced the developer cvar plus a DEVELOPER prelude.
//
// The dev code also reads convars retail does not register ("ConVar ... is not
// valid" script errors); they are registered here with the value that matches the
// non-DEV path of the script that reads them.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "thirdparty/detours/include/detours.h"
#include "devmode_s30.h"
#include "concommand_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

// mov rax, [rdx+18h] / movabs rcx, <false> / movabs r12, 4800000000000000h / mov [rdx+18h], rcx
static const char* const s_pszPatternDevFold =
	"48 8B 42 18 48 B9 00 00 00 00 00 00 00 28 49 BC 00 00 00 00 00 00 00 48 48 89 4A 18";
static const int kFoldImmediate = 6; // the bool constant's payload byte

typedef LPSTR(WINAPI* GetCommandLineA_t)(void);
typedef LPWSTR(WINAPI* GetCommandLineW_t)(void);
struct DevConVar_t
{
	const char* pszName;
	const char* pszDefault;
	const char* pszHelp;
};
// Every GetConVar* in #if DEV code / DEV-listed scripts naming a convar the exe lacks.
static const DevConVar_t s_devConVars[] = {
	{ "artifacts_tier_override", "0", "DEV: artifact tier override (mp/sh_artifacts.nut)" },
	{ "entity_inspector", "0", "DEV: entity inspector (sh_dev_utility.nut)" },
	{ "escrow_is_player_trusted", "0", "DEV: 1 trusted, -1 untrusted, 0 normal (sh_grx.gnut)" },
	{ "force_sequence_index", "-1", "DEV: forced flourish index, -1 off (mp/sh_quips.nut)" },
	{ "gwut_debug_step", "0", "DEV: phase breach debug step (weapons/mp_weapon_phase_breach.nut)" },
	{ "gwut_debug_substep", "-1", "DEV: phase breach debug substep, -1 all" },
	{ "kepler_forceVariant", "", "DEV: forced Kepler variant (sh_kepler.gnut)" },
	{ "mastery_unlock_all_trials", "0", "DEV: unlock all mastery trials (sh_mastery.gnut)" },
	{ "pip_scene_verbose", "0", "DEV: PIP scene logging (sh_pip_scene.gnut)" },
	{ "rui_dev_test_fullscreen_rui_ref", "", "DEV: fullscreen RUI test asset (dev/ui_rui_fullscreen_test.nut)" },
	{ "rui_live", "0", "DEV: RUI live editor open (clui_utility.gnut)" },
	{ "weapon_position_debugger", "0", "DEV: menu weapon position debugger (sh_menu_models.gnut)" },
};
static bool s_bDevMode = false;

static GetCommandLineA_t v_GetCommandLineA = nullptr;
static GetCommandLineW_t v_GetCommandLineW = nullptr;
static std::string s_commandLineA;
static std::wstring s_commandLineW;

static LPSTR WINAPI h_GetCommandLineA(void) { return const_cast<LPSTR>(s_commandLineA.c_str()); }
static LPWSTR WINAPI h_GetCommandLineW(void) { return const_cast<LPWSTR>(s_commandLineW.c_str()); }

// Whole tokens only: -devsdk is not dev mode.
static bool DevMode_HasToken(const char* pszCommandLine, const char* pszToken)
{
	const size_t nLen = strlen(pszToken);
	for (const char* p = pszCommandLine; (p = strstr(p, pszToken)) != nullptr; p += nLen)
	{
		const bool bStart = p == pszCommandLine || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '"';
		const char c = p[nLen];
		if (bStart && (c == '\0' || c == ' ' || c == '\t' || c == '"'))
			return true;
	}
	return false;
}

static bool DevMode_PatchFold(void)
{
	int nMatches = 0;
	uint8_t* const pFold = S30Pattern_FindUnique(s_pszPatternDevFold, &nMatches);
	if (!pFold)
	{
		S30Log("[DEVMODE] #if DEV constant unresolved (%d matches) -- DEV stays false in scripts\n", nMatches);
		return false;
	}
	DWORD nOld = 0;
	if (!VirtualProtect(pFold + kFoldImmediate, 1, PAGE_EXECUTE_READWRITE, &nOld))
		return false;
	pFold[kFoldImmediate] = 1; // bool true
	VirtualProtect(pFold + kFoldImmediate, 1, nOld, &nOld);
	FlushInstructionCache(GetCurrentProcess(), pFold, 16);
	S30Log("[DEVMODE] #if DEV folds to true (0x%llX)\n", static_cast<unsigned long long>(S30Pattern_Rva(pFold)));
	return true;
}

static void DevMode_AddDeveloper(const char* pszCommandLine)
{
	if (strstr(pszCommandLine, "+developer"))
	{
		S30Log("[DEVMODE] +developer already on the command line\n");
		return;
	}
	s_commandLineA = std::string(pszCommandLine) + " +developer 1";
	s_commandLineW = std::wstring(GetCommandLineW()) + L" +developer 1";
	HMODULE hKernelBase = GetModuleHandleW(L"kernelbase.dll");
	if (!hKernelBase)
		hKernelBase = GetModuleHandleW(L"kernel32.dll");
	v_GetCommandLineA = reinterpret_cast<GetCommandLineA_t>(GetProcAddress(hKernelBase, "GetCommandLineA"));
	v_GetCommandLineW = reinterpret_cast<GetCommandLineW_t>(GetProcAddress(hKernelBase, "GetCommandLineW"));
	if (!v_GetCommandLineA || !v_GetCommandLineW)
	{
		S30Log("[DEVMODE] GetCommandLineA/W unresolved -- developer stays 0\n");
		return;
	}
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_GetCommandLineA), h_GetCommandLineA);
	DetourAttach(&reinterpret_cast<PVOID&>(v_GetCommandLineW), h_GetCommandLineW);
	const LONG nError = DetourTransactionCommit();
	S30Log("[DEVMODE] command line +developer 1 %s\n", nError == NO_ERROR ? "added" : "FAILED");
}

bool S30DevMode_Install(void)
{
	const char* const pszCommandLine = GetCommandLineA();
	if (!pszCommandLine || (!DevMode_HasToken(pszCommandLine, "-dev") && !DevMode_HasToken(pszCommandLine, "-developer")))
		return false;
	s_bDevMode = true;
	DevMode_PatchFold();
	DevMode_AddDeveloper(pszCommandLine);
	return true;
}

void S30DevMode_RegisterConVars(void)
{
	if (!s_bDevMode)
		return;
	int nRegistered = 0;
	for (const DevConVar_t& cv : s_devConVars)
		nRegistered += S30ConCommand_RegisterConVar(cv.pszName, cv.pszDefault, cv.pszHelp);
	S30Log("[DEVMODE] registered %d of %d dev convars\n", nRegistered, static_cast<int>(sizeof(s_devConVars) / sizeof(s_devConVars[0])));
}
