#ifndef CORE_S30_BRIDGE_PREDCENSUS_H
#define CORE_S30_BRIDGE_PREDCENSUS_H
//=============================================================================//
//
// Purpose: prediction census (diagnostic): per player prop, how often the
// received value differs from what the client held, and how often the wire sends
// 0 against a held value (a field the dedi never feeds).
//
//=============================================================================//
#include <cstdint>

// Wraps the props of the DT_Player decoder (bridge_netchan h_DecoderDone).
void S30PredCensus_OnDecoder(uint8_t* pDecoder);
// sdk_predcensus [0|1|reset]
void S30PredCensus_RegisterCommands(void);

#endif // CORE_S30_BRIDGE_PREDCENSUS_H
