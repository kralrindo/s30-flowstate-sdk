#ifndef CORE_S30_BRIDGE_ANIMTRACE_H
#define CORE_S30_BRIDGE_ANIMTRACE_H
//=============================================================================//
//
// Purpose: first-person animation trace (diagnostic): the first-person proxy and
// viewmodel animation fields as the wire delivers them and as the client plays
// them, frame by frame.
//
//=============================================================================//
#include <cstdint>

// Wraps the traced props of a finished decoder (bridge_netchan h_DecoderDone).
void S30AnimTrace_OnDecoder(uint8_t* pDecoder);
// Once per presented frame: logs every change in the traced fields.
void S30AnimTrace_OnFrame(void);
// sdk_anim1p [0|1]
void S30AnimTrace_RegisterCommands(void);

#endif // CORE_S30_BRIDGE_ANIMTRACE_H
