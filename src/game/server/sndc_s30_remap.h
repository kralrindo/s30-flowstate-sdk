#ifndef SNDC_S30_REMAP_H
#define SNDC_S30_REMAP_H
//=============================================================================//
//
// Purpose: script netvars in the S30.1 client's layout (+bridge_client_set s30).
//
// Both sides give each netvar the next slot of its (category, internal type) in
// their own registration order, and their arrays have different counts, so the
// client bound none of the dedi's DT_ScriptNetData* arrays (s30/docs/21, Cause A).
// The S30 client sends its netvar table by name (C2S 'NVT1'); the dedi sends each
// array with S30's count, every element read from the dedi slot of the same name.
//
//=============================================================================//

class CClient;

// C2S frame from the S30 client: [u32 'NVT1'][u16 total][u16 first][u8 n] then n x
// [u8 S30 category][u8 S30 type][u8 slot][u8 second slot][f32 min][f32 max][u8 len][name].
constexpr uint32_t BRIDGE_C2S_NETVARTABLE_MAGIC = 0x3154564Eu; // 'NVT1'
bool SNDCS30_OnClientTable(CClient* pClient, const uint8_t* pFrame, int nBytes);

// SendTable init (dt_extend, before the S21 wrapper deep-clone): S30 counts, m_times
// renamed m_float32s, element proxies remapped. Returns the arrays reshaped.
int SNDCS30_ReshapeSendTables(void** ppTables, int nTables, bool bFinal);

// Server frame, before the snapshots: rebuilds the slot maps when the client table
// or the dedi's registrations changed.
void SNDCS30_Frame(void);

#endif // SNDC_S30_REMAP_H
