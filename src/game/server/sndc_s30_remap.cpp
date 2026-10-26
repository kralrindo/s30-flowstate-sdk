//=============================================================================//
//
// Purpose: script netvars in the S30.1 client's layout (+bridge_client_set s30).
//
// Categories and types are numbered differently on the two sides (dedi
// scriptnetdata_client.h; S30 constants registered at 0xB836C4):
//   category  GLOBAL 0/0, PLAYER_GLOBAL 1/2, PLAYER_EXCLUSIVE 2/3 (4 expanded),
//             TITAN_SOUL 3/5, DEATH_BOX 4/6, GLOBAL_NON_REWIND 5/1   (dedi/S30)
//   type      dedi BOOL 0 INT 1 UINT 2 BIG_INT 3 FLOAT_RANGE 4 FR_OVER_TIME 5 TIME 6 ENTITY 7
//             S30  BOOL 0 INT 1 UINT 2 BIG_INT 3 FLOAT_RANGE 4 FR_OVER_TIME 5
//                  FLOAT_HIGH_PRECISION 6 TIME 7 ENTITY 8
// Each type lives in one of five arrays, the same five on both sides (S30
// registration jump table 0xB808EC; dedi slots in g_pClientNetVars):
//   m_bools, m_ranges (INT/UINT/FLOAT_RANGE*), m_int32s (BIG_INT),
//   m_times = S30 m_float32s (TIME, S30 FLOAT_HIGH_PRECISION), m_entities.
// The element encodings match (dedi 0x889B45..0x889B8B, S30 0xB81030: INT a 10-bit
// signed range, UINT a u16, BIG_INT an int32), so an element is the dedi's own
// element, encoded by its own proxy, only read from another slot.
//
//=============================================================================//
#include "core/stdafx.h"
#include "game/shared/dt_extend.h"
#include "game/shared/activity_s3_to_s21.h"
#include "game/shared/scriptnetdata_ext.h"
#include "sndc_s30_remap.h"
#include <string>
#include <vector>
#include <utility>

//-----------------------------------------------------------------------------
// The S30 client's table
//-----------------------------------------------------------------------------
struct S30NetVar_t
{
	std::string name;
	uint8_t nCategory;
	uint8_t nType;
	uint8_t nSlot;
	uint8_t nSlot2;
	float flMin, flMax;
};

static std::vector<S30NetVar_t> s_table;    // complete, by index
static std::vector<S30NetVar_t> s_incoming; // being received
static std::vector<bool> s_received;
static int s_nIncomingTotal = 0;
static bool s_bTableDirty = false;
static const char* const s_pszTableFile = "platform/cfg/s30_netvars.bin";

static const int kS30ToDediCategory[7] = { 0, 5, 1, 2, 2, 3, 4 };
static const int kS30TypeToArray[9] = { 0, 1, 1, 2, 1, 1, 3, 3, 4 };
static const int kDediTypeToArray[8] = { 0, 1, 1, 2, 1, 1, 3, 4 };
static const char* const kArrayNames[5] = { "m_bools", "m_ranges", "m_int32s", "m_times", "m_entities" };

//-----------------------------------------------------------------------------
// Reshaped arrays
//-----------------------------------------------------------------------------
static constexpr int kMaxArrays = 64;
static constexpr int kMaxElements = 64;

struct RemapArray_t
{
	const char* pszTable;
	const uint8_t* pArrayProp;
	int nDediCategory;
	int nArray;           // kArrayNames index
	int nS30Count;
	int nDediCount;
	int nTemplateOffset;  // element 0, entity-relative
	int nStride;
	DTExtendProxyFn pfnOriginal;
	int16_t map[kMaxElements]; // S30 slot -> dedi slot (| kFromRange*), -1 none
	int nRangesOffset;         // m_int32s: element 0 of the same entity's m_ranges
	volatile LONG nTraced;
};
static RemapArray_t s_arrays[kMaxArrays];
static int s_nArrays = 0;

// An S30 BIG_INT the dedi registered as an INT / UNSIGNED_INT (livingPlayerCount,
// squadsRemainingCount, ...): read from the dedi's m_ranges and widened.
static constexpr int16_t kFromRangeSigned = 0x4000;
static constexpr int16_t kFromRangeUnsigned = 0x2000;
static constexpr int16_t kSlotMask = 0x1FFF;

// An element no var of S30's maps to: false / 0 / no entity.
alignas(16) static const uint8_t s_zero[16] = {};
alignas(16) static const uint8_t s_noEntity[16] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// The element as the encoder would have produced it: the original proxy, or for an
// element with none (m_int32s, m_times) its raw value in the DVariant.
static void SNDCS30_Encode(const RemapArray_t& a, void* pProp, void* pStruct, void* pSource, void* pOut, int iElement, int objectID)
{
	if (a.pfnOriginal)
	{
		a.pfnOriginal(pProp, pStruct, pSource, pOut, iElement, objectID);
		return;
	}
	if (!pOut)
		return;
	memset(pOut, 0, 24);
	memcpy(pOut, pSource, a.nStride <= 8 ? a.nStride : 8);
}

static void SNDCS30_Element(RemapArray_t& a, void* pProp, void* pStruct, void* pData, void* pOut, int iElement, int objectID)
{
	// The encoder hands element k as pData = entity + element 0 + k * stride with
	// iElement k (traced). A call from another context (the PLAYER_EXCLUSIVE table
	// nested in _EXPANDED runs with another struct) is left as it was.
	const ptrdiff_t nRel = static_cast<uint8_t*>(pData) - (static_cast<uint8_t*>(pStruct) + a.nTemplateOffset);
	const bool bOwn = a.nStride > 0 && nRel >= 0 && nRel % a.nStride == 0 && nRel / a.nStride < a.nS30Count;
	if (a.nTraced < 4 && InterlockedIncrement(&a.nTraced) <= 4)
		Msg(eDLL_T::SERVER, "[SNDC-S30] %s.%s element: iElement %d, pData at element %td%s\n",
			a.pszTable, kArrayNames[a.nArray], iElement, a.nStride > 0 ? nRel / a.nStride : -1, bOwn ? "" : " (not this array -- as is)");
	if (!bOwn)
	{
		SNDCS30_Encode(a, pProp, pStruct, pData, pOut, iElement, objectID);
		return;
	}
	const int k = static_cast<int>(nRel / a.nStride);
	const int j = k < kMaxElements ? a.map[k] : -1;
	if (j >= 0 && (j & (kFromRangeSigned | kFromRangeUnsigned)) && a.nRangesOffset >= 0 && pOut)
	{
		// The dedi's 10-bit range element (0x889B59/0x889B6E): signed for INT.
		int v = *reinterpret_cast<const uint16_t*>(static_cast<uint8_t*>(pStruct) + a.nRangesOffset + 2 * (j & kSlotMask));
		if ((j & kFromRangeSigned) && v >= 0x200)
			v -= 0x400;
		memset(pOut, 0, 24);
		*static_cast<int*>(pOut) = v;
		return;
	}
	void* pSource;
	if (j >= 0 && j < a.nDediCount)
		pSource = static_cast<uint8_t*>(pStruct) + a.nTemplateOffset + j * a.nStride;
	else
		pSource = const_cast<uint8_t*>(a.nArray == 4 ? s_noEntity : s_zero);
	SNDCS30_Encode(a, pProp, pStruct, pSource, pOut, j >= 0 ? j : k, objectID);
}

template <int N>
static void __fastcall SNDCS30_Proxy(void* pProp, void* pStruct, void* pData, void* pOut, int iElement, int objectID)
{
	SNDCS30_Element(s_arrays[N], pProp, pStruct, pData, pOut, iElement, objectID);
}

template <int... I>
static DTExtendProxyFn SNDCS30_ProxyAt(int n, std::integer_sequence<int, I...>)
{
	static const DTExtendProxyFn table[] = { &SNDCS30_Proxy<I>... };
	return table[n];
}

struct RemapTable_t
{
	const char* pszName;
	int nDediCategory;
	int nS30Counts[5];
};
// S30 RecvTable counts (read from the S30.1 client's RecvTables).
static const RemapTable_t kTables[] =
{
	{ "DT_ScriptNetData_SNDC_GLOBAL",                    0, { 18, 38, 18, 26, 10 } },
	{ "DT_ScriptNetDataGlobal",                          0, { 18, 38, 18, 26, 10 } },
	{ "DT_ScriptNetDataGlobalNonRewind",                 5, { 18, 18, 12, 18,  6 } },
	{ "DT_ScriptNetData_SNDC_GLOBAL_NON_REWIND",         5, { 18, 18, 12, 18,  6 } },
	{ "DT_ScriptNetData_SNDC_PLAYER_GLOBAL",             1, { 18, 38, 38, 14,  6 } },
	{ "DT_ScriptNetData_SNDC_PLAYER_EXCLUSIVE",          2, { 36, 30,  8, 22, 24 } },
	{ "DT_ScriptNetData_SNDC_PLAYER_EXCLUSIVE_EXPANDED", 2, { 36, 50,  8, 22, 24 } },
	{ "DT_ScriptNetData_SNDC_TITAN_SOUL",                3, { 10, 18,  4, 10,  6 } },
	{ "DT_ScriptNetData_SNDC_DEATH_BOX",                 4, {  5,  4, 14,  3,  3 } },
};

static void SNDCS30_ReshapeTable(uint8_t* pTable, const RemapTable_t& t, int& nReshaped)
{
	uint8_t* const pProps = *reinterpret_cast<uint8_t**>(pTable + ST_PROPS);
	const int nProps = *reinterpret_cast<int*>(pTable + ST_NPROPS);
	if (!pProps || nProps <= 0 || nProps > 4096)
		return;
	for (int i = 1; i < nProps; ++i)
	{
		uint8_t* const pArray = pProps + static_cast<uint64_t>(i) * SP_SIZE;
		if (*reinterpret_cast<int*>(pArray + SP_TYPE) != 5)
			continue;
		const char* const pszName = *reinterpret_cast<const char**>(pArray + SP_VARNAME);
		int nArray = -1;
		for (int a = 0; a < 5 && pszName; ++a)
			if (!strcmp(pszName, kArrayNames[a]))
				nArray = a;
		if (nArray < 0)
			continue;
		// The element template: the preceding prop of the same name (dt_extend's SNDC resize);
		// SendTable_Init links it as the array prop later (null until then).
		uint8_t* const pTemplate = pArray - SP_SIZE;
		const char* const pszTemplate = *reinterpret_cast<const char**>(pTemplate + SP_VARNAME);
		uint8_t* const pLinked = *reinterpret_cast<uint8_t**>(pArray + SP_ARRAYPROP);
		if (!pszTemplate || strcmp(pszTemplate, pszName) || (pLinked && pLinked != pTemplate))
		{
			Warning(eDLL_T::SERVER, "[SNDC-S30] %s.%s: element template not the preceding prop -- left alone\n", t.pszName, pszName);
			continue;
		}
		DTExtendProxyFn pfnOriginal = *reinterpret_cast<DTExtendProxyFn*>(pTemplate + 0x60); // none: raw value
		// A deep-cloned table (PLAYER_EXCLUSIVE_EXPANDED, cloned from PLAYER_EXCLUSIVE by
		// DTExtend_PostApplyS21Overrides) carries the wrapper of the prop it was cloned from.
		for (int s = 0; s < s_nArrays; ++s)
			if (pfnOriginal == SNDCS30_ProxyAt(s, std::make_integer_sequence<int, kMaxArrays>{}))
			{
				pfnOriginal = s_arrays[s].pfnOriginal;
				break;
			}
		bool bShared = false;
		for (int s = 0; s < s_nArrays && !bShared; ++s)
		{
			if (s_arrays[s].pArrayProp != pArray)
				continue;
			bShared = true;
			Warning(eDLL_T::SERVER, "[SNDC-S30] %s.%s is %s's prop -- reshaped once\n", t.pszName, pszName, s_arrays[s].pszTable);
		}
		if (bShared)
			continue;
		if (s_nArrays >= kMaxArrays)
		{
			Warning(eDLL_T::SERVER, "[SNDC-S30] more than %d arrays -- %s.%s left alone\n", kMaxArrays, t.pszName, pszName);
			continue;
		}
		RemapArray_t& a = s_arrays[s_nArrays];
		a.pszTable = t.pszName;
		a.pArrayProp = pArray;
		a.nDediCategory = t.nDediCategory;
		a.nArray = nArray;
		a.nS30Count = t.nS30Counts[nArray];
		a.nDediCount = *reinterpret_cast<int*>(pArray + SP_NELEMENTS);
		a.nTemplateOffset = *reinterpret_cast<int*>(pTemplate + SP_OFFSET);
		a.nStride = *reinterpret_cast<int*>(pArray + SP_ELEMSTRIDE);
		a.pfnOriginal = pfnOriginal;
		for (int k = 0; k < kMaxElements; ++k)
			a.map[k] = -1;
		a.nRangesOffset = -1;
		a.nTraced = 0;

		*reinterpret_cast<DTExtendProxyFn*>(pTemplate + 0x60) = SNDCS30_ProxyAt(s_nArrays, std::make_integer_sequence<int, kMaxArrays>{});
		*reinterpret_cast<int*>(pArray + SP_NELEMENTS) = a.nS30Count;
		Msg(eDLL_T::SERVER, "[SNDC-S30] %s.%s: %d -> %d elements (element +%d, stride %d, type %d)\n", t.pszName,
			kArrayNames[nArray], a.nDediCount, a.nS30Count, a.nTemplateOffset, a.nStride, *reinterpret_cast<int*>(pTemplate + SP_TYPE));
		Msg(eDLL_T::SERVER, "[SNDC-S30]   table %p props %p array prop %p\n", pTable, pProps, pArray);
		++s_nArrays;
		++nReshaped;
	}
}

int SNDCS30_ReshapeSendTables(void** ppTables, int nTables, bool bFinal)
{
	if (!Bridge_ClientIsS30())
		return 0;
	int nReshaped = 0;
	std::vector<uint8_t*> seen;
	auto visit = [&](uint8_t* pTable)
	{
		if (!pTable || !DTExtend_IsSafeToRead(pTable, ST_NETTABLENAME + 8))
			return;
		for (uint8_t* p : seen)
			if (p == pTable)
				return;
		seen.push_back(pTable);
		const char* const pszName = *reinterpret_cast<const char**>(pTable + ST_NETTABLENAME);
		if (!pszName)
			return;
		if (strstr(pszName, "ScriptNetData"))
			Msg(eDLL_T::SERVER, "[SNDC-S30] table '%s' (%d props)\n", pszName, *reinterpret_cast<int*>(pTable + ST_NPROPS));
		for (const RemapTable_t& t : kTables)
			if (!strcmp(t.pszName, pszName))
				SNDCS30_ReshapeTable(pTable, t, nReshaped);
	};
	for (int i = 0; i < nTables; ++i)
		visit(static_cast<uint8_t*>(ppTables[i]));
	for (int i = 0; i < s_cachedSendTableCount; ++i)
		visit(reinterpret_cast<uint8_t*>(s_cachedSendTablePtrs[i]));
	// S30 calls the time array m_float32s. Renamed last: the S21 pass in between
	// (PLAYER_EXCLUSIVE_EXPANDED's layout) finds it as m_times.
	if (bFinal)
	{
		static const char* const s_pszFloat32s = "m_float32s";
		for (int i = 0; i < s_nArrays; ++i)
		{
			if (s_arrays[i].nArray != 3)
				continue;
			uint8_t* const pArray = const_cast<uint8_t*>(s_arrays[i].pArrayProp);
			*reinterpret_cast<const char**>(pArray + SP_VARNAME) = s_pszFloat32s;
			*reinterpret_cast<const char**>(pArray - SP_SIZE + SP_VARNAME) = s_pszFloat32s;
		}
	}
	Msg(eDLL_T::SERVER, "[SNDC-S30] %d script netvar arrays in the S30 client's shape (%zu tables visited)\n", nReshaped, seen.size());
	return nReshaped;
}

//-----------------------------------------------------------------------------
// The client's table frames
//-----------------------------------------------------------------------------
static void SNDCS30_SaveTable(void)
{
	FILE* pFile = nullptr;
	if (fopen_s(&pFile, s_pszTableFile, "wb") || !pFile)
		return;
	const uint32_t n = static_cast<uint32_t>(s_table.size());
	fwrite(&n, 4, 1, pFile);
	for (const S30NetVar_t& v : s_table)
	{
		const uint8_t nLen = static_cast<uint8_t>(v.name.size());
		fwrite(&v.nCategory, 1, 1, pFile);
		fwrite(&v.nType, 1, 1, pFile);
		fwrite(&v.nSlot, 1, 1, pFile);
		fwrite(&v.nSlot2, 1, 1, pFile);
		fwrite(&v.flMin, 4, 1, pFile);
		fwrite(&v.flMax, 4, 1, pFile);
		fwrite(&nLen, 1, 1, pFile);
		fwrite(v.name.data(), 1, nLen, pFile);
	}
	fclose(pFile);
}

static void SNDCS30_LoadTable(void)
{
	FILE* pFile = nullptr;
	if (fopen_s(&pFile, s_pszTableFile, "rb") || !pFile)
		return;
	uint32_t n = 0;
	std::vector<S30NetVar_t> table;
	bool bOk = fread(&n, 4, 1, pFile) == 1 && n <= 1024;
	for (uint32_t i = 0; bOk && i < n; ++i)
	{
		S30NetVar_t v;
		uint8_t nLen = 0;
		char szName[256];
		bOk = fread(&v.nCategory, 1, 1, pFile) == 1 && fread(&v.nType, 1, 1, pFile) == 1 && fread(&v.nSlot, 1, 1, pFile) == 1
			&& fread(&v.nSlot2, 1, 1, pFile) == 1 && fread(&v.flMin, 4, 1, pFile) == 1 && fread(&v.flMax, 4, 1, pFile) == 1
			&& fread(&nLen, 1, 1, pFile) == 1 && fread(szName, 1, nLen, pFile) == nLen;
		if (bOk)
		{
			v.name.assign(szName, nLen);
			table.push_back(std::move(v));
		}
	}
	fclose(pFile);
	if (bOk && !table.empty())
	{
		s_table = std::move(table);
		s_bTableDirty = true;
		Msg(eDLL_T::SERVER, "[SNDC-S30] %zu S30 netvars from %s\n", s_table.size(), s_pszTableFile);
	}
}

bool SNDCS30_OnClientTable(CClient* /*pClient*/, const uint8_t* pFrame, int nBytes)
{
	int off = 0;
	auto take = [&](void* p, int n) -> bool
	{
		if (off + n > nBytes)
			return false;
		memcpy(p, pFrame + off, n);
		off += n;
		return true;
	};
	uint32_t nMagic = 0;
	uint16_t nTotal = 0, nFirst = 0;
	uint8_t nCount = 0;
	if (!take(&nMagic, 4) || nMagic != BRIDGE_C2S_NETVARTABLE_MAGIC || !take(&nTotal, 2) || !take(&nFirst, 2) || !take(&nCount, 1)
		|| !nTotal || nTotal > 1024 || nFirst + nCount > nTotal)
		return false;
	if (nTotal != s_nIncomingTotal)
	{
		s_incoming.assign(nTotal, S30NetVar_t());
		s_received.assign(nTotal, false);
		s_nIncomingTotal = nTotal;
	}
	for (int i = 0; i < nCount; ++i)
	{
		S30NetVar_t v;
		uint8_t nLen = 0;
		char szName[256];
		if (!take(&v.nCategory, 1) || !take(&v.nType, 1) || !take(&v.nSlot, 1) || !take(&v.nSlot2, 1) || !take(&v.flMin, 4)
			|| !take(&v.flMax, 4) || !take(&nLen, 1) || !take(szName, nLen))
			return false;
		v.name.assign(szName, nLen);
		s_incoming[nFirst + i] = std::move(v);
		s_received[nFirst + i] = true;
	}
	for (bool b : s_received)
		if (!b)
			return true;
	// Complete: a repeat of the table in use changes nothing.
	bool bSame = s_incoming.size() == s_table.size();
	for (size_t i = 0; bSame && i < s_table.size(); ++i)
	{
		const S30NetVar_t &a = s_incoming[i], &b = s_table[i];
		bSame = a.name == b.name && a.nCategory == b.nCategory && a.nType == b.nType && a.nSlot == b.nSlot;
	}
	if (!bSame)
	{
		s_table = s_incoming;
		s_bTableDirty = true;
		SNDCS30_SaveTable();
		Msg(eDLL_T::SERVER, "[SNDC-S30] S30 client netvar table: %zu vars\n", s_table.size());
	}
	s_received.assign(s_received.size(), false);
	return true;
}

//-----------------------------------------------------------------------------
// Slot maps
//-----------------------------------------------------------------------------
// An SNDC entity gets the dirty bit its own setters set (0xC158B0: edict index at
// +0x58, flags at [[0xD4EC368] + 0x78] + 0x40 + 2 * index), so the new mapping is
// sent at once rather than at the vars' next change.
static void SNDCS30_MarkChanged(uintptr_t pEntity)
{
	if (!pEntity)
		return;
	const uintptr_t nBase = g_GameDll.GetModuleBase();
	__try
	{
		const uint16_t nEdict = *reinterpret_cast<const uint16_t*>(pEntity + 0x58);
		const uintptr_t pGlobals = *reinterpret_cast<const uintptr_t*>(nBase + 0xD4EC368);
		if (nEdict == 0xFFFF || !pGlobals)
			return;
		volatile SHORT* const pFlags = reinterpret_cast<volatile SHORT*>(*reinterpret_cast<const uintptr_t*>(pGlobals + 0x78) + 0x40 + 2 * nEdict);
		InterlockedOr16(pFlags, 0x200);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

// Entity from a handle (dedi entity table 0x241F1A8, 48-byte slots, serial at +8).
static uintptr_t SNDCS30_EntityFromHandle(uint32_t nHandle)
{
	if (nHandle == 0xFFFFFFFFu)
		return 0;
	const uintptr_t pSlot = g_GameDll.GetModuleBase() + 0x241F1A8 + 0x30 * (nHandle & 0xFFFF);
	if (*reinterpret_cast<const uint32_t*>(pSlot + 8) != (nHandle >> 16))
		return 0;
	return *reinterpret_cast<const uintptr_t*>(pSlot);
}

static void SNDCS30_MarkAllChanged(void)
{
	const uintptr_t nBase = g_GameDll.GetModuleBase();
	SNDCS30_MarkChanged(*reinterpret_cast<const uintptr_t*>(nBase + 0x295075E8)); // the GLOBAL SNDC entity
	SNDCS30_MarkChanged(reinterpret_cast<uintptr_t>(g_pScriptNetDataNonRewindEnt));
	for (int i = 1; i <= MAX_PLAYERS; ++i)
	{
		__try
		{
			const uintptr_t pPlayer = *reinterpret_cast<const uintptr_t*>(nBase + 0x241F1A8 + 0x30 * i);
			if (!pPlayer)
				continue;
			// The player's PLAYER_GLOBAL and PLAYER_EXCLUSIVE entities (0xDD9B73).
			SNDCS30_MarkChanged(SNDCS30_EntityFromHandle(*reinterpret_cast<const uint32_t*>(pPlayer + 0x7A80)));
			SNDCS30_MarkChanged(SNDCS30_EntityFromHandle(*reinterpret_cast<const uint32_t*>(pPlayer + 0x7A84)));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
}

static void SNDCS30_BuildMaps(void)
{
	for (int i = 0; i < s_nArrays; ++i)
		for (int k = 0; k < kMaxElements; ++k)
			s_arrays[i].map[k] = -1;
	int nMapped = 0, nMissing = 0, nTypeDiffers = 0;
	for (const S30NetVar_t& v : s_table)
	{
		if (v.nCategory > 6 || v.nType > 8)
			continue;
		const int nCategory = kS30ToDediCategory[v.nCategory];
		const int nArray = kS30TypeToArray[v.nType];
		int nDediType = -1;
		const int nDediSlot = ScriptNetData_FindVarSlotAndType(v.name.c_str(), nCategory, &nDediType);
		if (nDediSlot < 0 || nDediType < 0 || nDediType > 7)
		{
			// S30-only, or registered in another category: stays at its default on the client.
			Msg(eDLL_T::SERVER, "[SNDC-S30]   not on the dedi: %s (S30 category %d, type %d)\n", v.name.c_str(), v.nCategory, v.nType);
			++nMissing;
			continue;
		}
		if (nArray == 2 && (nDediType == 1 || nDediType == 2))
		{
			// S30 BIG_INT, dedi INT / UNSIGNED_INT: widened from the dedi's m_ranges.
			bool bPlaced = false;
			for (int i = 0; i < s_nArrays; ++i)
			{
				RemapArray_t& a = s_arrays[i];
				if (a.nDediCategory != nCategory || a.nArray != 2 || v.nSlot >= a.nS30Count)
					continue;
				for (int r = 0; r < s_nArrays; ++r)
				{
					const RemapArray_t& ranges = s_arrays[r];
					if (ranges.nArray == 1 && ranges.pszTable == a.pszTable && nDediSlot < ranges.nDediCount)
					{
						a.nRangesOffset = ranges.nTemplateOffset;
						a.map[v.nSlot] = static_cast<int16_t>(nDediSlot | (nDediType == 1 ? kFromRangeSigned : kFromRangeUnsigned));
						bPlaced = true;
					}
				}
			}
			nMapped += bPlaced;
			continue;
		}
		if (kDediTypeToArray[nDediType] != nArray)
		{
			if (++nTypeDiffers <= 16)
				Warning(eDLL_T::SERVER, "[SNDC-S30] %s: S30 type %d, dedi type %d -- not sent\n", v.name.c_str(), v.nType, nDediType);
			continue;
		}
		bool bPlaced = false;
		for (int i = 0; i < s_nArrays; ++i)
		{
			RemapArray_t& a = s_arrays[i];
			if (a.nDediCategory != nCategory || a.nArray != nArray || v.nSlot >= a.nS30Count || nDediSlot >= a.nDediCount)
				continue;
			a.map[v.nSlot] = static_cast<int16_t>(nDediSlot);
			bPlaced = true;
		}
		nMapped += bPlaced;
	}
	Msg(eDLL_T::SERVER, "[SNDC-S30] slot maps: %d of %zu S30 netvars mapped, %d not registered on the dedi, %d of another type\n",
		nMapped, s_table.size(), nMissing, nTypeDiffers);
}

void SNDCS30_Frame(void)
{
	if (!s_nArrays)
		return;
	static bool s_bLoaded = false;
	if (!s_bLoaded)
	{
		s_bLoaded = true;
		SNDCS30_LoadTable();
	}
	if (s_table.empty())
		return;
	// The dedi's registrations: they change on every level load.
	static int s_nFrame = 0;
	static int s_nLastCount = -1;
	static uint64_t s_nLastSum = 0;
	if (!s_bTableDirty && (++s_nFrame & 31))
		return;
	uint64_t nSum = 0;
	const int nCount = ScriptNetData_RegisteredSignature(&nSum);
	if (!s_bTableDirty && nCount == s_nLastCount && nSum == s_nLastSum)
		return;
	if (!nCount)
		return;
	s_nLastCount = nCount;
	s_nLastSum = nSum;
	s_bTableDirty = false;
	SNDCS30_BuildMaps();
	SNDCS30_MarkAllChanged();
}
