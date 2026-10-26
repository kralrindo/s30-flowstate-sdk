#ifndef CORE_S30_BRIDGE_HUD_H
#define CORE_S30_BRIDGE_HUD_H
//=============================================================================//
//
// Purpose: the dedi SDK's chat lanes (svc_SystemSayText 67, svc_ChatBuilder 70)
// and svc_PlaylistChange (32) on the S30 client. Engine thread.
//
//=============================================================================//
#include <cstdint>

void S30Hud_Install(void);
void S30Hud_SystemSayText(const char* pszPrefix, const char* pszMessage, bool bAdmin);
// The 70 payload after its 16-bit length.
void S30Hud_ChatBuilder(const uint8_t* pPayload, int nBytes);
void S30Hud_PlaylistChange(const char* pszName);

#endif // CORE_S30_BRIDGE_HUD_H
