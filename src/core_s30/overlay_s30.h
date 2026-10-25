#ifndef CORE_S30_OVERLAY_H
#define CORE_S30_OVERLAY_H
//=============================================================================//
//
// Purpose: ImGui overlay for the S30 client (DX12).
//
//=============================================================================//

// Hooks the DXGI swapchain and the D3D12 command queue through vtables read
// from a throwaway device. Call from SDK_Init (not under the loader lock),
// before the game creates its own device.
bool S30Overlay_Install(void);

bool S30Overlay_IsVisible(void);
void S30Overlay_SetVisible(bool bVisible);
long long S30Overlay_PresentCount(void);
// Writes the next presented frame (overlay included) to pszPath as a BMP.
void S30Overlay_RequestScreenshot(const char* pszPath);

// Panels (overlay_panels_s30.cpp), called inside an ImGui frame.
void S30Panels_Init(void);
void S30Panels_Draw(void);
void S30Panels_ToggleConsole(void);
void S30Panels_OnOpened(void);

#endif // CORE_S30_OVERLAY_H
