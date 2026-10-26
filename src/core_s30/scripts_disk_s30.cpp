//=============================================================================//
//
// Purpose: the CLIENT and UI scripts from platform/scripts/vscripts on disk.
//
// S30.1 compiles each script from "scripts/vscripts/<name><.client|.ui|.server>"
// (0xF591E0) and reads the lists from "scripts/vscripts/scripts.rson<suffix>"
// (0xC0A687). Scripts go through the filesystem's ReadFile (vtable +0x78, 0x4AA9D0:
// this, path, path id, buffer); a list goes through the rson loader 0x4B4770, which
// looks the full path's GUID up in the paks and, when no pak has it, opens the path
// minus its VM suffix from disk and parses it as text. ReadFile serves a path from the loaded paks
// first (script_client.rpak / script_ui.rpak, which the client cannot start
// without) and only then from disk, so a loose copy under the same name is never
// read, and past the paks ReadFile refuses any ".nut"/".gnut" path outright
// (patched out here). Here a script path is handed to ReadFile as the path of the
// file in the merged folder (s30/tools/merge_s30_scripts.py: one file per script,
// CLIENT and UI blocks as #if CLIENT / #if UI, one scripts.rson with
// When: "CLIENT" / "UI"), which no pak serves; the engine's own buffer handling
// is unchanged, and a list path becomes the folder's scripts.rson (its GUID is in
// no pak, so the loader reads and parses the file; the conditions resolve CLIENT,
// UI, SERVER and DEV per VM, 0xC09D10). A script the folder lacks still comes from
// the pak.
// Off with -s30_rpakscripts, or when the folder has no scripts.rson.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include "thirdparty/detours/include/detours.h"
#include "scripts_disk_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternReadFile =
	"48 89 6C 24 20 56 57 41 57 48 81 EC 40 01 00 00 49 8B F0 48 8B FA 4C 8B F9 48 8B D6 48 8B CF 4D 8B C1 49 8B E9 E8";
// ReadFile 0x4AAAE0: lea rdx, ".nut" / strstr / test / jne refuse; the same for ".gnut".
static const char* const s_pszPatternScriptRefusal =
	"48 8D 15 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 85 C0 0F 85 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 85 C0 0F 85";
static const int kRefusalNutJne = 0x12;
static const int kRefusalGnutJne = 0x2A;
static const int kRefusalSize = 0x30;
// rson loader 0x4B4770: push rbx/rsi/rdi/r12, sub rsp 148h, mov rsi, rcx, the path hash pick.
static const char* const s_pszPatternLoadRson =
	"40 53 56 57 41 54 48 81 EC 48 01 00 00 48 8B F1 48 8D 05 ?? ?? ?? ?? F6 C2 03 48 8D 0D ?? ?? ?? ?? 4C 8B E2";
static const char* const s_pszVmSuffixes[] = { ".client", ".ui", ".server" };

typedef bool (*ReadFile_t)(void* pThis, const char* pszPath, const char* pszPathId, void* pBuffer);
static ReadFile_t v_ReadFile = nullptr;
// (out, path, ...): true when the rson was found and parsed.
typedef bool (*LoadRson_t)(void* pOut, const char* pszPath, void* a3, void* a4, void* a5, void* a6);
static LoadRson_t v_LoadRson = nullptr;
static std::string s_root; // <game>\platform\scripts\vscripts\

// "scripts/vscripts/<rel><suffix>" -> the merged file on disk, or "".
static std::string ScriptsDisk_Map(const char* pszPath)
{
	static const char kPrefix[] = "scripts/vscripts/";
	const size_t nPrefix = sizeof(kPrefix) - 1;
	if (_strnicmp(pszPath, kPrefix, nPrefix) && _strnicmp(pszPath, "scripts\\vscripts\\", nPrefix))
		return std::string();
	std::string rel(pszPath + nPrefix);
	for (const char* pszSuffix : s_pszVmSuffixes)
	{
		const size_t n = strlen(pszSuffix);
		if (rel.size() > n && !_stricmp(rel.c_str() + rel.size() - n, pszSuffix))
		{
			rel.resize(rel.size() - n);
			break;
		}
	}
	for (char& c : rel)
		if (c == '/')
			c = '\\';
	std::string disk = s_root + rel;
	const DWORD nAttributes = GetFileAttributesA(disk.c_str());
	if (nAttributes == INVALID_FILE_ATTRIBUTES || (nAttributes & FILE_ATTRIBUTE_DIRECTORY))
		return std::string();
	return disk;
}

// The path forms tried for a disk file; the first that reads is kept.
enum PathForm_t { FORM_ABSOLUTE_FORWARD, FORM_ABSOLUTE, FORM_GAME_RELATIVE, FORM_COUNT };
static const char* const s_pszFormNames[FORM_COUNT] = { "absolute, forward slashes", "absolute", "relative to the game folder" };
static volatile LONG s_nForm = -1;

static std::string ScriptsDisk_Form(const std::string& disk, int nForm)
{
	std::string path = disk;
	if (nForm == FORM_GAME_RELATIVE)
		path = disk.substr(s_root.size() - strlen("platform\\scripts\\vscripts\\"));
	if (nForm != FORM_ABSOLUTE)
		for (char& c : path)
			if (c == '\\')
				c = '/';
	return path;
}

static bool h_ReadFile(void* pThis, const char* pszPath, const char* pszPathId, void* pBuffer)
{
	if (pszPath)
	{
		const std::string disk = ScriptsDisk_Map(pszPath);
		if (!disk.empty())
		{
			const LONG nKnown = s_nForm;
			for (int nForm = nKnown >= 0 ? nKnown : 0; nForm < (nKnown >= 0 ? nKnown + 1 : FORM_COUNT); ++nForm)
			{
				const std::string path = ScriptsDisk_Form(disk, nForm);
				if (!v_ReadFile(pThis, path.c_str(), pszPathId, pBuffer))
					continue;
				if (nKnown < 0)
				{
					InterlockedExchange(&s_nForm, nForm);
					S30Log("[SCRIPTS] the filesystem reads disk scripts by %s path (%s)\n", s_pszFormNames[nForm], path.c_str());
				}
				return true;
			}
			static volatile LONG s_nFailed = 0;
			if (InterlockedIncrement(&s_nFailed) <= 8)
				S30LogFile("[SCRIPTS] %s: %s not readable -- from the pak\n", pszPath, disk.c_str());
		}
	}
	return v_ReadFile(pThis, pszPath, pszPathId, pBuffer);
}

static bool ScriptsDisk_IsList(const char* pszPath)
{
	static const char kList[] = "scripts/vscripts/scripts.rson";
	const size_t nList = sizeof(kList) - 1;
	if (_strnicmp(pszPath, kList, nList) && _strnicmp(pszPath, "scripts\\vscripts\\scripts.rson", nList))
		return false;
	const char* const pszSuffix = pszPath + nList;
	if (!*pszSuffix)
		return true;
	for (const char* pszVm : s_pszVmSuffixes)
		if (!_stricmp(pszSuffix, pszVm))
			return true;
	return false;
}

static bool h_LoadRson(void* pOut, const char* pszPath, void* a3, void* a4, void* a5, void* a6)
{
	if (pszPath && ScriptsDisk_IsList(pszPath))
	{
		const std::string path = ScriptsDisk_Form(s_root + "scripts.rson", FORM_ABSOLUTE_FORWARD);
		if (v_LoadRson(pOut, path.c_str(), a3, a4, a5, a6))
		{
			S30Log("[SCRIPTS] %s from %s\n", pszPath, path.c_str());
			return true;
		}
		S30Log("[SCRIPTS] %s: %s did not load -- from the pak\n", pszPath, path.c_str());
	}
	return v_LoadRson(pOut, pszPath, a3, a4, a5, a6);
}

bool S30ScriptsDisk_Install(void)
{
	if (strstr(GetCommandLineA(), "-s30_rpakscripts"))
	{
		S30Log("[SCRIPTS] -s30_rpakscripts: scripts come from the paks\n");
		return false;
	}
	char szExe[MAX_PATH];
	const DWORD nLen = GetModuleFileNameA(nullptr, szExe, MAX_PATH);
	if (!nLen || nLen >= MAX_PATH)
		return false;
	*strrchr(szExe, '\\') = '\0';
	s_root = std::string(szExe) + "\\platform\\scripts\\vscripts\\";
	if (GetFileAttributesA((s_root + "scripts.rson").c_str()) == INVALID_FILE_ATTRIBUTES)
	{
		S30Log("[SCRIPTS] no %sscripts.rson -- scripts come from the paks\n", s_root.c_str());
		return false;
	}
	int nMatches = 0;
	v_ReadFile = reinterpret_cast<ReadFile_t>(S30Pattern_FindUnique(s_pszPatternReadFile, &nMatches));
	v_LoadRson = reinterpret_cast<LoadRson_t>(S30Pattern_FindUnique(s_pszPatternLoadRson, &nMatches));
	uint8_t* const pRefusal = S30Pattern_FindUnique(s_pszPatternScriptRefusal, &nMatches);
	if (!v_ReadFile || !v_LoadRson || !pRefusal)
	{
		S30Log("[SCRIPTS] filesystem ReadFile, the rson loader or the script refusal unresolved -- scripts come from the paks\n");
		return false;
	}
	// Past the paks, ReadFile refuses any path containing ".nut" or ".gnut" (two
	// strstr checks, jne to "not read"): retail reads scripts from paks only.
	DWORD nOld = 0;
	if (!VirtualProtect(pRefusal, kRefusalSize, PAGE_EXECUTE_READWRITE, &nOld))
		return false;
	for (int nJne : { kRefusalNutJne, kRefusalGnutJne })
		memset(pRefusal + nJne, 0x90, 6);
	VirtualProtect(pRefusal, kRefusalSize, nOld, &nOld);
	FlushInstructionCache(GetCurrentProcess(), pRefusal, kRefusalSize);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ReadFile), h_ReadFile);
	DetourAttach(&reinterpret_cast<PVOID&>(v_LoadRson), h_LoadRson);
	const LONG nError = DetourTransactionCommit();
	S30Log("[SCRIPTS] CLIENT/UI scripts from %s %s\n", s_root.c_str(), nError == NO_ERROR ? "(disk first)" : "-- hook FAILED");
	return nError == NO_ERROR;
}
