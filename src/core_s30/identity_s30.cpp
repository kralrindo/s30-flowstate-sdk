//=============================================================================//
//
// Purpose: fixed offline platform identity for the S30 client.
//
// S30.1 checks "platform logged in" in several places before it will connect
// (the two connect handlers, CClientState::Connect 0x2FCDB0 via 0x3092C0, ...).
// Offline nothing logs in, so every connect stops. The S21 SDK gives the
// client a fixed identity instead (ebisusdk/EbisuSDK.cpp: profile + Nucleus id
// FAKE_BASE_NUCLEUD_ID); this is the S30 equivalent.
//
// The globals come from the check function itself (0x3092C0), decoded:
//   mov eax,[platState]; cmp eax,1; jnz
//   cmp dword [errorLevel],0; jnz     cmp byte [sdkInit],0; jz
//   cmp qword [nucleusId],0; jz       cmp byte [profileReady],0; jz
//   cmp byte [authCode],0; jz         cmp byte [nucleusToken],0; setnz al
// authCode is a 256-byte buffer, nucleusToken a 10240-byte one (the gap to the
// next buffer); both are NUL-terminated strings.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "s30_log.h"
#include "s30_pattern.h"
#include "identity_s30.h"

static const char* const s_pszPatternIsLoggedIn =
	"8B 05 ?? ?? ?? ?? 83 F8 01 75 ?? 83 3D ?? ?? ?? ?? 00 75 ?? 80 3D ?? ?? ?? ?? 00 74 ?? "
	"48 83 3D ?? ?? ?? ?? 00 74 ?? 80 3D ?? ?? ?? ?? 00 74 ?? 80 3D ?? ?? ?? ?? 00 74 ?? "
	"80 3D ?? ?? ?? ?? 00 0F 95 C0 C3";

static const uint64_t s_nOfflineNucleusId = 9990000; // FAKE_BASE_NUCLEUD_ID, as on S21
static const char s_szOfflineAuthCode[] = "r5f-offline";
static const char s_szOfflineToken[] = "r5f-offline";

static int* s_pPlatState = nullptr;
static int* s_pErrorLevel = nullptr;
static uint8_t* s_pSdkInit = nullptr;
static uint64_t* s_pNucleusId = nullptr;
static uint8_t* s_pProfileReady = nullptr;
static char* s_pszAuthCode = nullptr;
static char* s_pszToken = nullptr;
static int s_nApplied = 0;

// rip-relative target of an instruction at pInsn whose disp32 sits at nDispOff
// and whose total length is nLen.
static uint8_t* Identity_RipTarget(uint8_t* pInsn, int nDispOff, int nLen)
{
	int32_t nDisp = 0;
	memcpy(&nDisp, pInsn + nDispOff, sizeof(nDisp));
	return pInsn + nLen + nDisp;
}

bool S30Identity_Init(void)
{
	int nMatches = 0;
	uint8_t* const p = S30Pattern_FindUnique(s_pszPatternIsLoggedIn, &nMatches);
	if (!p)
	{
		S30Log("[IDENTITY] IsPlatformLoggedIn unresolved (matches=%d) -- no offline identity\n", nMatches);
		return false;
	}
	s_pPlatState = reinterpret_cast<int*>(Identity_RipTarget(p + 0x00, 2, 6));
	s_pErrorLevel = reinterpret_cast<int*>(Identity_RipTarget(p + 0x0B, 2, 7));
	s_pSdkInit = Identity_RipTarget(p + 0x14, 2, 7);
	s_pNucleusId = reinterpret_cast<uint64_t*>(Identity_RipTarget(p + 0x1D, 3, 8));
	s_pProfileReady = Identity_RipTarget(p + 0x27, 2, 7);
	s_pszAuthCode = reinterpret_cast<char*>(Identity_RipTarget(p + 0x30, 2, 7));
	s_pszToken = reinterpret_cast<char*>(Identity_RipTarget(p + 0x39, 2, 7));
	S30Log("[IDENTITY] IsPlatformLoggedIn @ 0x%llX; platState 0x%llX, nucleus 0x%llX, token 0x%llX\n",
		static_cast<unsigned long long>(S30Pattern_Rva(p)), static_cast<unsigned long long>(S30Pattern_Rva(s_pPlatState)),
		static_cast<unsigned long long>(S30Pattern_Rva(s_pNucleusId)), static_cast<unsigned long long>(S30Pattern_Rva(s_pszToken)));
	return true;
}

void S30Identity_Print(void)
{
	if (!s_pPlatState)
	{
		S30Log("identity: unresolved\n");
		return;
	}
	S30Log("identity: platState=%d errorLevel=0x%X sdkInit=%u nucleus=%llu profile=%u authCode=\"%.32s\" token=\"%.32s\" (applied %d)\n",
		*s_pPlatState, *s_pErrorLevel, *s_pSdkInit, static_cast<unsigned long long>(*s_pNucleusId), *s_pProfileReady,
		s_pszAuthCode, s_pszToken, s_nApplied);
}

void S30Identity_Pump(void)
{
	if (!s_pPlatState || !*s_pSdkInit)
		return; // the engine's own Origin init has not finished yet
	if (*s_pNucleusId == s_nOfflineNucleusId && *s_pProfileReady && s_pszAuthCode[0] && s_pszToken[0] && *s_pErrorLevel == 0)
		return;

	*s_pErrorLevel = 0;
	*s_pNucleusId = s_nOfflineNucleusId;
	*s_pProfileReady = 1;
	memcpy(s_pszAuthCode, s_szOfflineAuthCode, sizeof(s_szOfflineAuthCode));
	memcpy(s_pszToken, s_szOfflineToken, sizeof(s_szOfflineToken));
	++s_nApplied;
	S30Log("[IDENTITY] offline identity set (nucleus %llu, platState %d)\n", static_cast<unsigned long long>(s_nOfflineNucleusId), *s_pPlatState);
}
