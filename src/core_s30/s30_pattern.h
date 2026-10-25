#ifndef CORE_S30_PATTERN_H
#define CORE_S30_PATTERN_H
//=============================================================================//
//
// Purpose: byte-signature search over the host image's .text for client_s30.
//
// Stricter than the S21 CMemory search on purpose: a signature counts only when
// it matches exactly once. A second match means the signature no longer pins
// the function, and the caller gets null instead of the first hit.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "s30_image.h"

// Parses "48 8B ?? 05" into bytes + mask. Returns the byte count, 0 on error.
inline size_t S30Pattern_Parse(const char* pszPattern, uint8_t* pBytes, bool* pMask, size_t nMax)
{
	size_t n = 0;
	for (const char* p = pszPattern; *p;)
	{
		if (*p == ' ')
		{
			++p;
			continue;
		}
		if (n >= nMax)
			return 0;
		if (*p == '?')
		{
			pMask[n] = false;
			pBytes[n++] = 0;
			while (*p == '?')
				++p;
			continue;
		}
		char sz[3] = { p[0], p[1], 0 };
		pBytes[n] = static_cast<uint8_t>(strtoul(sz, nullptr, 16));
		pMask[n++] = true;
		p += 2;
	}
	return n;
}

// The single match of a signature in the host .text, or null (none, or more than one).
inline uint8_t* S30Pattern_FindUnique(const char* pszPattern, int* pnMatches = nullptr)
{
	uint8_t bytes[256];
	bool mask[256];
	const size_t nLen = S30Pattern_Parse(pszPattern, bytes, mask, sizeof(bytes));
	if (pnMatches)
		*pnMatches = 0;
	if (!nLen)
		return nullptr;

	const uintptr_t base = S30Image_Base();
	const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
	const IMAGE_SECTION_HEADER* pSec = IMAGE_FIRST_SECTION(pNt);
	for (WORD i = 0; i < pNt->FileHeader.NumberOfSections; ++i, ++pSec)
	{
		if (memcmp(pSec->Name, ".text", 5) != 0)
			continue;
		uint8_t* const pStart = reinterpret_cast<uint8_t*>(base + pSec->VirtualAddress);
		const size_t nSize = pSec->Misc.VirtualSize;
		uint8_t* pFound = nullptr;
		int nMatches = 0;
		for (size_t off = 0; off + nLen <= nSize; ++off)
		{
			if (pStart[off] != bytes[0] && mask[0])
				continue;
			size_t k = 0;
			while (k < nLen && (!mask[k] || pStart[off + k] == bytes[k]))
				++k;
			if (k == nLen)
			{
				pFound = pStart + off;
				if (++nMatches > 1)
					break;
			}
		}
		if (pnMatches)
			*pnMatches = nMatches;
		return nMatches == 1 ? pFound : nullptr;
	}
	return nullptr;
}

inline uintptr_t S30Pattern_Rva(const void* pAddress)
{
	return reinterpret_cast<uintptr_t>(pAddress) - S30Image_Base();
}

#endif // CORE_S30_PATTERN_H
