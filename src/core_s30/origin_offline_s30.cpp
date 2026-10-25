//=============================================================================//
//
// Purpose: S30 port of the S21 offline platform patches.
//
// S30 statically links the same Origin::OriginSDK::Initialize as S21; the branch
// structure is identical and only the error-code register moved (r12d -> r13d).
// Sites (S30.1 RVAs, S21 DX12 in brackets), verified side by side in IDA:
//   D  0x11D0A47 [0x107A9F4] jnz -> jmp: a missing EA app install is not fatal
//   E  0x11D0A85 [0x107AA32] NOP jnz: do not skip to LSX when the EA app runs
//   A1 0x11D0A91 [0x107AA3E] NOP jnb: never take the launch-the-EA-app branch
//   A2 0x11D0AA6 [0x107AA53] mov r13d,0A0020000h -> 42010001h (start succeeded)
//   C  0x11CDB30 [0x1078060] platform "singleton ready" predicate -> false, so
//      every C-API entry takes its not-initialized path. The idiom matches twice
//      on S30; the C-API anchor's call at +0x21 picks the right one.
//   F  0x42D1E1 [OriginInitFatalCall] NOP the Error() call for "Failed to get
//      Origin environment setting": with C applied the setting query fails, and
//      the rest of the one-time platform init must still run (S21 does the same
//      in core/dllmain_client.cpp).
//   P  0x42F940 [0x3F7530] the per-frame Origin poll (OriginUpdate) -> ret. With
//      no platform it reports an error level every frame and calls Error();
//      S21 DX12 stubs the same function (EbisuSDK_StubNativePollIfUnhooked).
// Without D-C the S30 client spawns EADesktop.exe and exits when it cannot;
// without F it reaches the title screen's Error() and exits with code 3.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "s30_log.h"
#include "s30_pattern.h"
#include "origin_offline_s30.h"

static const char* const s_pszPatternD =
	"E8 ?? ?? ?? ?? 84 C0 75 ?? B9 08 00 02 A0";
static const char* const s_pszPatternA =
	"85 C0 75 ?? 8B 86 68 04 00 00 0F BA E0 09 73 ?? 0F BA E0 0C 73 ?? "
	"45 33 C0 B2 01 48 8B CE E8 ?? ?? ?? ?? 41 BD 00 00 02 A0";
static const char* const s_pszPatternCAnchor =
	"48 89 5C 24 08 57 48 83 EC 30 48 8B DA 48 8B F9 48 8D 15 ?? ?? ?? ?? "
	"B9 00 00 00 03 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 1D E8 ?? ?? ?? ?? "
	"48 8B C8 4C 8B C3 48 8B D7";

// lea rcx, msg / mov r8d, ebx / call Error / lea r8, envbuf / lea r9, "production"
static const char* const s_pszPatternF =
	"48 8D 0D ?? ?? ?? ?? 44 8B C3 E8 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 4C 8D 0D";
static const char s_szFatalMessageF[] = "Failed to get Origin environment setting";
// sub rsp,578h / cmp byte [initialized],0 / jz / cmp byte [shutdown],0 / jz +0C / add rsp / jmp
static const char* const s_pszPatternP =
	"48 81 EC 78 05 00 00 80 3D ?? ?? ?? ?? 00 0F 84 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 74 0C 48 81 C4 78 05 00 00 E9";

static bool OriginOffline_Write(uint8_t* pSite, const uint8_t* pBytes, size_t nLen)
{
	DWORD nOldProt = 0;
	if (!VirtualProtect(pSite, nLen, PAGE_EXECUTE_READWRITE, &nOldProt))
		return false;
	memcpy(pSite, pBytes, nLen);
	VirtualProtect(pSite, nLen, nOldProt, &nOldProt);
	FlushInstructionCache(GetCurrentProcess(), pSite, nLen);
	return true;
}

//-----------------------------------------------------------------------------
// G: connect without an Origin identity. Both connect handlers (0x2F0B50 and
// 0x2F1220 on S30.1) refuse to start unless ICommandLine vfunc 14 (0x238480,
// "return 0" in retail) says otherwise, or the Nucleus id, profile flag, auth
// code and NucleusToken are all present; offline none are, and the command
// silently becomes "#ORIGIN_REQUEST_TOKEN_FAILURE" + Disconnected. The bypass
// branch already exists: jnz -> jmp (90 E9, same rel32). The S3 dedi never sees
// S30's tokens; the bridge sends S3 its own connect.
//-----------------------------------------------------------------------------
static const char* const s_pszPatternConnectGate =
	"FF 50 70 84 C0 0F 85 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 83 F9 01 75 ?? 8B 05 ?? ?? ?? ?? 44 8B C3 85 C0";

static int OriginOffline_PatchConnectGates(void)
{
	uint8_t bytes[64];
	bool mask[64];
	const size_t nLen = S30Pattern_Parse(s_pszPatternConnectGate, bytes, mask, sizeof(bytes));
	const uintptr_t nBase = S30Image_Base();
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase)->e_lfanew);
	const IMAGE_SECTION_HEADER* pSec = IMAGE_FIRST_SECTION(pNt);
	int nPatched = 0;
	for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i, ++pSec)
	{
		if (memcmp(pSec->Name, ".text", 5) != 0)
			continue;
		uint8_t* const pStart = reinterpret_cast<uint8_t*>(nBase + pSec->VirtualAddress);
		for (size_t off = 0; off + nLen <= pSec->Misc.VirtualSize && nPatched < 4; ++off)
		{
			size_t k = 0;
			while (k < nLen && (!mask[k] || pStart[off + k] == bytes[k]))
				++k;
			if (k != nLen)
				continue;
			uint8_t* const pJnz = pStart + off + 5; // 0F 85 rel32
			if (pJnz[0] != 0x0F || pJnz[1] != 0x85)
				continue;
			static const uint8_t s_jmp[2] = { 0x90, 0xE9 };
			if (OriginOffline_Write(pJnz, s_jmp, sizeof(s_jmp)))
			{
				++nPatched;
				S30Log("[OFFLINE] connect identity gate opened at 0x%llX\n", static_cast<unsigned long long>(S30Pattern_Rva(pJnz)));
			}
		}
	}
	return nPatched;
}

bool S30OriginOffline_Apply(void)
{
	// Two connect handlers on S30.1; anything else means the code moved -- leave it.
	const int nGates = OriginOffline_PatchConnectGates();
	if (nGates != 2)
		S30Log("[OFFLINE] patch G: %d connect gates patched (expected 2)\n", nGates);

	int nMatches = 0;

	// D
	uint8_t* const pMatchD = S30Pattern_FindUnique(s_pszPatternD, &nMatches);
	uint8_t* const pSiteD = pMatchD ? pMatchD + 7 : nullptr;
	if (!pSiteD || pSiteD[0] != 0x75)
	{
		S30Log("[OFFLINE] patch D unresolved (matches=%d) -- nothing written\n", nMatches);
		return false;
	}

	// E, A1, A2
	uint8_t* const pMatchA = S30Pattern_FindUnique(s_pszPatternA, &nMatches);
	if (!pMatchA)
	{
		S30Log("[OFFLINE] patch A unresolved (matches=%d) -- nothing written\n", nMatches);
		return false;
	}
	uint8_t* const pSiteE = pMatchA + 2;
	uint8_t* const pSiteA1 = pMatchA + 14;
	uint8_t* const pSiteA2 = pMatchA + 35;
	static const uint8_t s_expectA2[6] = { 0x41, 0xBD, 0x00, 0x00, 0x02, 0xA0 }; // mov r13d, 0A0020000h
	if (pSiteE[0] != 0x75 || pSiteA1[0] != 0x73 || memcmp(pSiteA2, s_expectA2, sizeof(s_expectA2)) != 0)
	{
		S30Log("[OFFLINE] patch E/A1/A2 byte mismatch -- nothing written\n");
		return false;
	}

	// C: bind the predicate through the anchor's call, then check its body.
	uint8_t* const pAnchor = S30Pattern_FindUnique(s_pszPatternCAnchor, &nMatches);
	if (!pAnchor || pAnchor[0x21] != 0xE8)
	{
		S30Log("[OFFLINE] patch C anchor unresolved (matches=%d) -- nothing written\n", nMatches);
		return false;
	}
	int32_t nRel = 0;
	memcpy(&nRel, pAnchor + 0x22, sizeof(nRel));
	uint8_t* const pSiteC = pAnchor + 0x21 + 5 + nRel;
	// cmp qword [rip+disp], 0 / setne al / ret
	if (pSiteC[0] != 0x48 || pSiteC[1] != 0x83 || pSiteC[2] != 0x3D || pSiteC[7] != 0x00
		|| pSiteC[8] != 0x0F || pSiteC[9] != 0x95 || pSiteC[10] != 0xC0 || pSiteC[11] != 0xC3)
	{
		S30Log("[OFFLINE] patch C target 0x%llX is not the ready predicate -- nothing written\n",
			static_cast<unsigned long long>(S30Pattern_Rva(pSiteC)));
		return false;
	}

	// F: the call must load exactly this message, or it is not our Error().
	uint8_t* const pMatchF = S30Pattern_FindUnique(s_pszPatternF, &nMatches);
	uint8_t* const pSiteF = pMatchF ? pMatchF + 10 : nullptr;
	bool bMessageOk = false;
	if (pMatchF)
	{
		int32_t nMsgRel = 0;
		memcpy(&nMsgRel, pMatchF + 3, sizeof(nMsgRel));
		const char* const pszMessage = reinterpret_cast<const char*>(pMatchF + 7 + nMsgRel);
		bMessageOk = strncmp(pszMessage, s_szFatalMessageF, sizeof(s_szFatalMessageF) - 1) == 0;
	}
	if (!pSiteF || pSiteF[0] != 0xE8 || !bMessageOk)
	{
		S30Log("[OFFLINE] patch F unresolved (matches=%d message=%d) -- nothing written\n", nMatches, bMessageOk);
		return false;
	}

	uint8_t* const pSiteP = S30Pattern_FindUnique(s_pszPatternP, &nMatches);
	if (!pSiteP)
	{
		S30Log("[OFFLINE] patch P (Origin poll) unresolved (matches=%d) -- nothing written\n", nMatches);
		return false;
	}

	static const uint8_t s_patchP[] = { 0xC3 }; // ret
	static const uint8_t s_patchF[] = { 0x90, 0x90, 0x90, 0x90, 0x90 };
	static const uint8_t s_patchD[] = { 0xEB };
	static const uint8_t s_patchE[] = { 0x90, 0x90 };
	static const uint8_t s_patchA1[] = { 0x90, 0x90 };
	static const uint8_t s_patchA2[] = { 0x41, 0xBD, 0x01, 0x00, 0x01, 0x42 }; // mov r13d, 42010001h
	static const uint8_t s_patchC[] = { 0x33, 0xC0, 0xC3 };                    // xor eax, eax / ret

	const bool bOk = OriginOffline_Write(pSiteD, s_patchD, sizeof(s_patchD))
		&& OriginOffline_Write(pSiteE, s_patchE, sizeof(s_patchE))
		&& OriginOffline_Write(pSiteA1, s_patchA1, sizeof(s_patchA1))
		&& OriginOffline_Write(pSiteA2, s_patchA2, sizeof(s_patchA2))
		&& OriginOffline_Write(pSiteC, s_patchC, sizeof(s_patchC))
		&& OriginOffline_Write(pSiteF, s_patchF, sizeof(s_patchF))
		&& OriginOffline_Write(pSiteP, s_patchP, sizeof(s_patchP));

	S30Log("[OFFLINE] origin poll stub P=0x%llX\n", static_cast<unsigned long long>(S30Pattern_Rva(pSiteP)));
	S30Log("[OFFLINE] origin patches %s: D=0x%llX E=0x%llX A1=0x%llX A2=0x%llX C=0x%llX F=0x%llX\n", bOk ? "applied" : "FAILED",
		static_cast<unsigned long long>(S30Pattern_Rva(pSiteD)), static_cast<unsigned long long>(S30Pattern_Rva(pSiteE)),
		static_cast<unsigned long long>(S30Pattern_Rva(pSiteA1)), static_cast<unsigned long long>(S30Pattern_Rva(pSiteA2)),
		static_cast<unsigned long long>(S30Pattern_Rva(pSiteC)), static_cast<unsigned long long>(S30Pattern_Rva(pSiteF)));
	return bOk;
}
