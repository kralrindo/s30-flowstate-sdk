#ifndef CORE_S30_IMAGE_H
#define CORE_S30_IMAGE_H
//=============================================================================//
//
// Purpose: identity of the S30 client images client_s30 supports.
//
// Every RVA here was verified by disassembly against the image it is listed
// for (see s30/docs). A host that matches none of them gets no hooks.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>

struct S30Image_t
{
	DWORD nTimeDateStamp;
	uintptr_t nLauncherMainRva; // WinMain -> LauncherMain call target
	const char* pszBuild;
};

inline const S30Image_t g_S30Images[] =
{
	{ 0x6AAB34CEu, 0x588C50, "R5pc_r5-301_J28_CL11570498_FSv30_1_2026_09_16_17_18" },
};

// The S30 image this process runs, or null.
inline const S30Image_t* S30Image_Get(void)
{
	const HMODULE hExe = GetModuleHandleW(NULL);
	if (!hExe)
		return nullptr;
	const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hExe);
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
		reinterpret_cast<const BYTE*>(hExe) + pDos->e_lfanew);
	for (const S30Image_t& image : g_S30Images)
	{
		if (pNt->FileHeader.TimeDateStamp == image.nTimeDateStamp)
			return &image;
	}
	return nullptr;
}

inline uintptr_t S30Image_Base(void)
{
	return reinterpret_cast<uintptr_t>(GetModuleHandleW(NULL));
}

#endif // CORE_S30_IMAGE_H
