//=============================================================================//
//
// Purpose: the dedi's netvars S30 re-typed, adapted on the S30 client (docs/20).
//
// The lenient matcher puts an unmatched SendProp on a dummy RecvProp of its own
// type (bridge_netchan); for the props below an adapter RecvProp of the same send
// type takes its place, so the stream decodes as before, and its proxy writes the
// value into S30's field. S30 decoders call proxy(info, pStruct, pOut): the value
// at info+32, pStruct the owner's object (the root table, i.e. the entity).
//
//   m_classModsActive (DT_Player, DT_PlayerDecoy): S3 Int64 -> S30 Array of 2 x
//     Int64 at +0x2660; the dedi's mask is element 0 (same player settings, same
//     mod indices).
//   m_bIsPlayerOverheating / m_playerOverheatValue / m_timeLastGeneratedPlayer-
//     Overheat: S3 Int / Float / Time -> S30 DataTables of 3 at +6464 (bytes) /
//     +6468 / +6480 (floats); S30's natives read element 0.
//   DT_WeaponX m_modBitfield{FromPlayer,Internal,Current,Disabled}: S3 Int (the
//     dedi's mod order) -> S30 3 x Int32 at +0x17CC/+0x17D8/+0x17E4/+0x17F0 (S30's
//     mod order), remapped by weapon name (kWeaponModRemap; C_WeaponX class name at
//     +6368, the GetWeaponClassName native 0x74D7E0). When the name is not set yet
//     the raw mask is kept and remapped after the snapshot.
// Projectile, loot and trail mod masks stay on dummies: which field names their
// weapon is unproven, and a wrong remap would show wrong attachments.
//   m_nResetEventsParity (any BaseAnimating table): S3 counter bumped by
//     ResetSequence -> S30 m_nResetEventsStartTime, the reset's time (Float,
//     C_BaseAnimating +0xD98). S30's sequence transitioner (0xB91010, called with
//     this +0xB50 and +0xD98) restarts a sequence equal to the playing one only
//     when that time is later than the last restart it saw; left at 0 a same-
//     sequence restart (melee swing, sprint/walk cycle) pops with no blend and its
//     events do not reset. A bump writes the animation's start time (frame at
//     +0xE44, start time +0xE4C), again after the snapshot once that is decoded.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "bridge_props_s30.h"
#include "game/shared/weapon_mod_remap_s30_table.h"
#include "s30_log.h"

static const int RP_TYPE = 0x00;
static const int RP_NAME = 0x28;
static const int RP_ARRAY_ELEMENT = 0x38;
static const int RP_PROXY = 0x48;
static const int RP_SIZE = 0x80;
static const int SP_NAME = 64;
static const int DECODE_VALUE = 32;

static const int PLAYER_CLASSMODS = 0x2660;
static const int BCC_OVERHEATING = 6464;
static const int BCC_OVERHEAT_VALUE = 6468;
static const int BCC_OVERHEAT_TIME = 6480;
static const int WEAPON_CLASSNAME = 6368;
static const int BA_RESET_EVENTS_TIME = 0xD98;
static const int BA_ANIM_START_TIME = 0xE4C;

typedef void (*RecvProxy_t)(const uint8_t* pInfo, uint8_t* pStruct, void* pOut);

//-----------------------------------------------------------------------------
// Weapon mods
//-----------------------------------------------------------------------------
static const WeaponModRemap_t* Props_FindRemap(const char* pszWeapon)
{
	for (const WeaponModRemap_t& r : kWeaponModRemap)
	{
		if (!strcmp(r.pszWeapon, pszWeapon))
			return &r;
	}
	return nullptr; // same order on both sides
}

// Writes the dedi's mask as S30's 3 dwords; false when the weapon name is not set yet.
static bool Props_WriteMods(uint8_t* pWeapon, int nField, uint32_t nDedi)
{
	const char* const pszName = reinterpret_cast<const char*>(pWeapon + WEAPON_CLASSNAME);
	uint32_t s30[3] = {};
	if (!pszName[0])
	{
		s30[0] = nDedi; // until the name arrives
		memcpy(pWeapon + nField, s30, sizeof(s30));
		return false;
	}
	const WeaponModRemap_t* const pRemap = Props_FindRemap(pszName);
	for (int d = 0; d < 32; ++d)
	{
		if (!(nDedi & (1u << d)))
			continue;
		const int s = pRemap ? (d < pRemap->nMods ? pRemap->nS30[d] : -1) : d;
		if (s >= 0 && s < 96)
			s30[s / 32] |= 1u << (s % 32);
	}
	memcpy(pWeapon + nField, s30, sizeof(s30));
	return true;
}

struct PendingMods_t
{
	uint8_t* pWeapon;
	int nField;
	uint32_t nDedi;
	int nTries;
};
static PendingMods_t s_pending[256];
static int s_nPending = 0;

static void Props_WeaponMods(const uint8_t* pInfo, uint8_t* pStruct, int nField)
{
	const uint32_t nDedi = *reinterpret_cast<const uint32_t*>(pInfo + DECODE_VALUE);
	if (Props_WriteMods(pStruct, nField, nDedi))
		return;
	for (int i = 0; i < s_nPending; ++i)
	{
		if (s_pending[i].pWeapon == pStruct && s_pending[i].nField == nField)
		{
			s_pending[i].nDedi = nDedi;
			return;
		}
	}
	if (s_nPending < static_cast<int>(sizeof(s_pending) / sizeof(s_pending[0])))
		s_pending[s_nPending++] = { pStruct, nField, nDedi, 0 };
}

static void Props_ModsFromPlayer(const uint8_t* pInfo, uint8_t* pStruct, void*) { Props_WeaponMods(pInfo, pStruct, 0x17CC); }
static void Props_ModsInternal(const uint8_t* pInfo, uint8_t* pStruct, void*) { Props_WeaponMods(pInfo, pStruct, 0x17D8); }
static void Props_ModsCurrent(const uint8_t* pInfo, uint8_t* pStruct, void*) { Props_WeaponMods(pInfo, pStruct, 0x17E4); }
static void Props_ModsDisabled(const uint8_t* pInfo, uint8_t* pStruct, void*) { Props_WeaponMods(pInfo, pStruct, 0x17F0); }

//-----------------------------------------------------------------------------
// Animation event resets
//-----------------------------------------------------------------------------
struct ResetParity_t
{
	uint8_t* pEntity;
	int nParity;
};
static ResetParity_t s_resetParity[4096]; // open addressing by entity pointer
static uint8_t* s_pendingResets[256];
static int s_nPendingResets = 0;

static ResetParity_t* Props_ResetSlot(uint8_t* pEntity)
{
	const size_t nSlots = sizeof(s_resetParity) / sizeof(s_resetParity[0]);
	size_t i = (reinterpret_cast<uintptr_t>(pEntity) >> 4) % nSlots;
	for (size_t n = 0; n < nSlots; ++n, i = (i + 1) % nSlots)
	{
		if (s_resetParity[i].pEntity == pEntity || !s_resetParity[i].pEntity)
			return &s_resetParity[i];
	}
	return nullptr;
}

// The reset time only ever moves forward: the transitioner compares it to the last one.
static void Props_StampReset(uint8_t* pEntity)
{
	float& flReset = *reinterpret_cast<float*>(pEntity + BA_RESET_EVENTS_TIME);
	const float flStart = *reinterpret_cast<const float*>(pEntity + BA_ANIM_START_TIME);
	if (flStart > flReset)
		flReset = flStart;
}

static void Props_ResetEventsParity(const uint8_t* pInfo, uint8_t* pStruct, void*)
{
	const int nParity = *reinterpret_cast<const int*>(pInfo + DECODE_VALUE);
	ResetParity_t* const pSlot = Props_ResetSlot(pStruct);
	if (!pSlot)
		return;
	const bool bKnown = pSlot->pEntity == pStruct;
	const bool bBumped = bKnown && pSlot->nParity != nParity;
	pSlot->pEntity = pStruct;
	pSlot->nParity = nParity;
	if (!bBumped)
		return;
	Props_StampReset(pStruct); // start time when it was decoded first
	if (s_nPendingResets < static_cast<int>(sizeof(s_pendingResets) / sizeof(s_pendingResets[0])))
		s_pendingResets[s_nPendingResets++] = pStruct;
	static volatile LONG s_nLogged = 0;
	if (InterlockedIncrement(&s_nLogged) <= 10)
		S30LogFile("[PROPS] m_nResetEventsParity %d on %p -> m_nResetEventsStartTime %.4f\n",
			nParity, pStruct, *reinterpret_cast<const float*>(pStruct + BA_RESET_EVENTS_TIME));
}

void S30Props_OnSnapshot(void)
{
	// Resets whose animation start time decoded after the parity.
	for (int i = 0; i < s_nPendingResets; ++i)
	{
		__try
		{
			Props_StampReset(s_pendingResets[i]);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}
	s_nPendingResets = 0;

	// The class name is set when the weapon's data update ran (same snapshot).
	int nKept = 0;
	for (int i = 0; i < s_nPending; ++i)
	{
		PendingMods_t& p = s_pending[i];
		bool bDone = false;
		__try
		{
			bDone = Props_WriteMods(p.pWeapon, p.nField, p.nDedi);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			bDone = true; // the weapon is gone
		}
		if (!bDone && ++p.nTries < 4)
			s_pending[nKept++] = p;
	}
	s_nPending = nKept;
}

//-----------------------------------------------------------------------------
// Class mods, overheat
//-----------------------------------------------------------------------------
static void Props_ClassMods(const uint8_t* pInfo, uint8_t* pStruct, void*)
{
	*reinterpret_cast<uint64_t*>(pStruct + PLAYER_CLASSMODS) = *reinterpret_cast<const uint64_t*>(pInfo + DECODE_VALUE);
	*reinterpret_cast<uint64_t*>(pStruct + PLAYER_CLASSMODS + 8) = 0;
}

static void Props_Overheating(const uint8_t* pInfo, uint8_t* pStruct, void*)
{
	pStruct[BCC_OVERHEATING] = *reinterpret_cast<const int*>(pInfo + DECODE_VALUE) != 0;
}

static void Props_OverheatValue(const uint8_t* pInfo, uint8_t* pStruct, void*)
{
	*reinterpret_cast<float*>(pStruct + BCC_OVERHEAT_VALUE) = *reinterpret_cast<const float*>(pInfo + DECODE_VALUE);
}

static void Props_OverheatTime(const uint8_t* pInfo, uint8_t* pStruct, void*)
{
	*reinterpret_cast<float*>(pStruct + BCC_OVERHEAT_TIME) = *reinterpret_cast<const float*>(pInfo + DECODE_VALUE);
}

//-----------------------------------------------------------------------------
// Adapter table
//-----------------------------------------------------------------------------
struct PropAdapter_t
{
	const char* pszTable; // root table, nullptr = any
	const char* pszProp;
	int nSendType;
	RecvProxy_t pfnProxy;
	uint8_t prop[RP_SIZE];
};
static PropAdapter_t s_adapters[] =
{
	{ "DT_Player", "m_classModsActive", 7, &Props_ClassMods, {} },
	{ "DT_PlayerDecoy", "m_classModsActive", 7, &Props_ClassMods, {} },
	{ nullptr, "m_bIsPlayerOverheating", 0, &Props_Overheating, {} },
	{ nullptr, "m_playerOverheatValue", 1, &Props_OverheatValue, {} },
	{ nullptr, "m_timeLastGeneratedPlayerOverheat", 9, &Props_OverheatTime, {} },
	{ "DT_WeaponX", "m_modBitfieldFromPlayer", 0, &Props_ModsFromPlayer, {} },
	{ "DT_WeaponX", "m_modBitfieldInternal", 0, &Props_ModsInternal, {} },
	{ "DT_WeaponX", "m_modBitfieldCurrent", 0, &Props_ModsCurrent, {} },
	{ "DT_WeaponX", "m_modBitfieldDisabled", 0, &Props_ModsDisabled, {} },
	{ nullptr, "m_nResetEventsParity", 0, &Props_ResetEventsParity, {} },
};

uint8_t* S30Props_Adapter(const char* pszRootTable, const uint8_t* pSendProp)
{
	if (!pSendProp)
		return nullptr;
	const char* const pszProp = *reinterpret_cast<const char* const*>(pSendProp + SP_NAME);
	const int nType = *reinterpret_cast<const int*>(pSendProp);
	for (PropAdapter_t& a : s_adapters)
	{
		if (nType != a.nSendType || !pszProp || _stricmp(a.pszProp, pszProp)
			|| (a.pszTable && (!pszRootTable || _stricmp(a.pszTable, pszRootTable))))
			continue;
		if (!*reinterpret_cast<const char* const*>(a.prop + RP_NAME))
		{
			*reinterpret_cast<int*>(a.prop + RP_TYPE) = a.nSendType;
			*reinterpret_cast<const char**>(a.prop + RP_NAME) = a.pszProp;
			*reinterpret_cast<uint8_t**>(a.prop + RP_ARRAY_ELEMENT) = a.prop;
			*reinterpret_cast<void**>(a.prop + RP_PROXY) = reinterpret_cast<void*>(a.pfnProxy);
		}
		return a.prop;
	}
	return nullptr;
}
