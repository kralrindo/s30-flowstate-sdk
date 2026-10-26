#ifndef CORE_S30_BRIDGE_PROPS_H
#define CORE_S30_BRIDGE_PROPS_H
//=============================================================================//
//
// Purpose: adapters for the dedi's netvars S30 re-typed (s30/docs/20): class
// mods, overheat, weapon mod masks (remapped to S30's mod order).
//
//=============================================================================//
#include <cstdint>

// The adapter RecvProp for an unmatched SendProp of the root table, or nullptr
// (then it goes on a dummy).
uint8_t* S30Props_Adapter(const char* pszRootTable, const uint8_t* pSendProp);
// A finished decoder (bridge_netchan h_DecoderDone): fixes the dedi's
// m_animPlaybackRate copy that lost SPROP_ROUNDDOWN.
void S30Props_OnDecoder(uint8_t* pDecoder);
// After a snapshot was processed: weapon mod masks whose weapon had no name yet.
void S30Props_OnSnapshot(void);

#endif // CORE_S30_BRIDGE_PROPS_H
