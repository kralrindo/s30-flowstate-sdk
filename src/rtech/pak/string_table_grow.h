//=============================================================================//
//
// Purpose: Raise SettingsAssets max-entries past stock 8192.
//
//=============================================================================//
#ifndef STRING_TABLE_GROW_H
#define STRING_TABLE_GROW_H

#include "thirdparty/detours/include/idetour.h"

//-----------------------------------------------------------------------------
// ParticleEffectNames max entries (stock 2048). S30 content precaches past 4096
// ("ParticleEffectNames is full" at 0x1000). A power of two up to 0x8000 (uint16
// wire maxEntries; the engine's table code only tests indices against 0xFFFF).
//-----------------------------------------------------------------------------
constexpr int kParticleEffectNamesSize = 8192;

// Particle indices are networked SIGNED (the client picks the networked or the
// client-local particle array by sign), so carrying every entry takes
// log2(size) + 1 bits. dt_extend widens m_effectIndex to this.
constexpr int ParticleEffectIndexBits(const int size)
{
	int bits = 0;
	while ((1 << bits) < size)
		++bits;
	return bits + 1;
}
static_assert((kParticleEffectNamesSize & (kParticleEffectNamesSize - 1)) == 0
	&& kParticleEffectNamesSize >= 0x800 && kParticleEffectNamesSize <= 0x8000,
	"ParticleEffectNames size must be a power of two in [0x800, 0x8000]");
static_assert(ParticleEffectIndexBits(4096) == 13, "4096 entries took 13 signed bits");

class VStringTableGrow : public IDetour
{
	virtual void GetAdr(void) const { }
	virtual void GetFun(void) const { }
	virtual void GetVar(void) const { }
	virtual void GetCon(void) const { }
	virtual void Detour(const bool bAttach) const;
};

#endif // STRING_TABLE_GROW_H
