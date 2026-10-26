//=============================================================================//
//
// Purpose: ImGui overlay for the S30 client (DX12).
//
// S30 is DX12-only. The vtables of IDXGISwapChain and ID3D12CommandQueue are
// read from a throwaway device/queue/swapchain, and the shared method
// addresses are detoured:
//   IDXGISwapChain::Present (8), ResizeBuffers (13), IDXGISwapChain1::Present1
//   (22), IDXGISwapChain3::ResizeBuffers1 (39), ID3D12CommandQueue::
//   ExecuteCommandLists (10).
// The game's direct queue is captured in ExecuteCommandLists; the overlay is
// recorded onto the current back buffer and submitted on that queue right
// before the game's own Present. ImGui initialises on the first real Present,
// taking the HWND, buffer count and format from the game's swapchain.
//
// Threads: Present runs on the render thread, the window procedure on the
// window thread. Both touch ImGui only under s_imguiLock.
//
//=============================================================================//
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include "thirdparty/detours/include/detours.h"
#include "thirdparty/imgui/imgui.h"
#include "thirdparty/imgui/backends/imgui_impl_dx12.h"
#include "thirdparty/imgui/backends/imgui_impl_win32.h"
#include "s30_log.h"
#include "overlay_s30.h"
#include "bridge_animtrace_s30.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain* pSwapChain, UINT nSyncInterval, UINT nFlags);
typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain1* pSwapChain, UINT nSyncInterval, UINT nFlags, const DXGI_PRESENT_PARAMETERS* pParams);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT nCount, UINT nWidth, UINT nHeight, DXGI_FORMAT eFormat, UINT nFlags);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers1_t)(IDXGISwapChain3* pSwapChain, UINT nCount, UINT nWidth, UINT nHeight, DXGI_FORMAT eFormat,
	UINT nFlags, const UINT* pNodeMasks, IUnknown* const* ppQueues);
typedef void(STDMETHODCALLTYPE* ExecuteCommandLists_t)(ID3D12CommandQueue* pQueue, UINT nLists, ID3D12CommandList* const* ppLists);

static Present_t v_Present = nullptr;
static Present1_t v_Present1 = nullptr;
static ResizeBuffers_t v_ResizeBuffers = nullptr;
static ResizeBuffers1_t v_ResizeBuffers1 = nullptr;
static ExecuteCommandLists_t v_ExecuteCommandLists = nullptr;

static const UINT s_nMaxBuffers = 8;
static const UINT s_nSrvDescriptors = 64;

struct OverlayFrame_t
{
	ID3D12CommandAllocator* pAllocator;
	ID3D12Resource* pBackBuffer;
	D3D12_CPU_DESCRIPTOR_HANDLE hRtv;
	UINT64 nFenceValue;
};

static CRITICAL_SECTION s_imguiLock;
static volatile LONG s_bVisible = 0;
static bool s_bImGuiReady = false;
static bool s_bFailed = false;

static ID3D12CommandQueue* volatile s_pGameQueue = nullptr;
static IDXGISwapChain3* s_pSwapChain = nullptr;
static ID3D12Device* s_pDevice = nullptr;
static ID3D12DescriptorHeap* s_pRtvHeap = nullptr;
static ID3D12DescriptorHeap* s_pSrvHeap = nullptr;
static ID3D12GraphicsCommandList* s_pCommandList = nullptr;
static ID3D12Fence* s_pFence = nullptr;
static HANDLE s_hFenceEvent = NULL;
static UINT64 s_nFenceValue = 0;
static OverlayFrame_t s_frames[s_nMaxBuffers] = {};
static UINT s_nBuffers = 0;
static DXGI_FORMAT s_eRtvFormat = DXGI_FORMAT_UNKNOWN;
static UINT s_nSrvUsed = 0;
static HWND s_hWnd = NULL;
static WNDPROC v_WndProc = nullptr;
static volatile LONG64 s_nPresents = 0;
static volatile LONG64 s_nOverlayFrames = 0;

long long S30Overlay_PresentCount(void)
{
	return s_nPresents;
}

bool S30Overlay_IsVisible(void)
{
	return s_bVisible != 0;
}

void S30Overlay_SetVisible(bool bVisible)
{
	InterlockedExchange(&s_bVisible, bVisible ? 1 : 0);
	if (bVisible)
		S30Panels_OnOpened();
	if (s_bImGuiReady)
	{
		EnterCriticalSection(&s_imguiLock);
		ImGui::GetIO().MouseDrawCursor = bVisible;
		LeaveCriticalSection(&s_imguiLock);
	}
}

//-----------------------------------------------------------------------------
// Input
//-----------------------------------------------------------------------------
static bool Overlay_IsToggleKey(WPARAM nKey)
{
	return nKey == VK_OEM_3 || nKey == VK_F10; // ` and F10, as in the S21 client
}

static volatile LONG s_nKeyMessages = 0;
static volatile LONG s_nCharMessages = 0;

static LRESULT CALLBACK Overlay_WndProc(HWND hWnd, UINT nMsg, WPARAM wParam, LPARAM lParam)
{
	if (nMsg == WM_KEYDOWN || nMsg == WM_KEYUP)
		InterlockedIncrement(&s_nKeyMessages);
	else if (nMsg == WM_CHAR)
		InterlockedIncrement(&s_nCharMessages);

	if (nMsg == WM_KEYDOWN && Overlay_IsToggleKey(wParam) && !(lParam & (1 << 30)))
	{
		S30Overlay_SetVisible(!S30Overlay_IsVisible());
		S30LogFile("[OVERLAY] toggled %s (window thread %lu, key msgs %ld, char msgs %ld)\n", S30Overlay_IsVisible() ? "open" : "closed",
			GetCurrentThreadId(), s_nKeyMessages, s_nCharMessages);
		return 0;
	}
	if ((nMsg == WM_CHAR && wParam == '`') || (nMsg == WM_KEYUP && Overlay_IsToggleKey(wParam)))
		return 0; // the toggle key itself never reaches the game or the input box

	if (s_bImGuiReady && S30Overlay_IsVisible())
	{
		EnterCriticalSection(&s_imguiLock);
		ImGui_ImplWin32_WndProcHandler(hWnd, nMsg, wParam, lParam);
		LeaveCriticalSection(&s_imguiLock);

		switch (nMsg)
		{
		case WM_INPUT:
			// Raw input must still be released by DefWindowProc.
			return DefWindowProcW(hWnd, nMsg, wParam, lParam);
		case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
		case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
			return 0; // swallowed while the overlay owns input
		default:
			break;
		}
	}
	return CallWindowProcW(v_WndProc, hWnd, nMsg, wParam, lParam);
}

//-----------------------------------------------------------------------------
// Device objects
//-----------------------------------------------------------------------------
static void Overlay_SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* pCpu, D3D12_GPU_DESCRIPTOR_HANDLE* pGpu)
{
	const UINT nIncrement = s_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const UINT nIndex = s_nSrvUsed < s_nSrvDescriptors ? s_nSrvUsed++ : s_nSrvDescriptors - 1;
	pCpu->ptr = s_pSrvHeap->GetCPUDescriptorHandleForHeapStart().ptr + static_cast<SIZE_T>(nIndex) * nIncrement;
	pGpu->ptr = s_pSrvHeap->GetGPUDescriptorHandleForHeapStart().ptr + static_cast<UINT64>(nIndex) * nIncrement;
}

static void Overlay_SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE)
{
}

static void Overlay_WaitForGpu(void)
{
	if (!s_pFence || !s_pGameQueue)
		return;
	++s_nFenceValue;
	if (SUCCEEDED(s_pGameQueue->Signal(s_pFence, s_nFenceValue)) && s_pFence->GetCompletedValue() < s_nFenceValue)
	{
		s_pFence->SetEventOnCompletion(s_nFenceValue, s_hFenceEvent);
		WaitForSingleObject(s_hFenceEvent, 2000);
	}
}

static void Overlay_ReleaseBackBuffers(void)
{
	Overlay_WaitForGpu();
	for (UINT i = 0; i < s_nMaxBuffers; ++i)
	{
		if (s_frames[i].pBackBuffer)
		{
			s_frames[i].pBackBuffer->Release();
			s_frames[i].pBackBuffer = nullptr;
		}
	}
}

static bool Overlay_CreateBackBuffers(void)
{
	const UINT nIncrement = s_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE hRtv = s_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
	for (UINT i = 0; i < s_nBuffers; ++i)
	{
		if (FAILED(s_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&s_frames[i].pBackBuffer))))
			return false;
		D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = s_eRtvFormat;
		rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
		s_pDevice->CreateRenderTargetView(s_frames[i].pBackBuffer, &rtvDesc, hRtv);
		s_frames[i].hRtv = hRtv;
		hRtv.ptr += nIncrement;
	}
	return true;
}

static DXGI_FORMAT Overlay_ViewFormat(DXGI_FORMAT eFormat)
{
	switch (eFormat)
	{
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return eFormat;
	}
}

static bool Overlay_InitImGui(IDXGISwapChain* pSwapChain)
{
	if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&s_pSwapChain))))
		return false;
	if (FAILED(s_pSwapChain->GetDevice(IID_PPV_ARGS(&s_pDevice))))
		return false;

	DXGI_SWAP_CHAIN_DESC desc = {};
	s_pSwapChain->GetDesc(&desc);
	s_hWnd = desc.OutputWindow;
	s_nBuffers = desc.BufferCount < s_nMaxBuffers ? desc.BufferCount : s_nMaxBuffers;
	s_eRtvFormat = Overlay_ViewFormat(desc.BufferDesc.Format);

	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.NumDescriptors = s_nMaxBuffers;
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.NumDescriptors = s_nSrvDescriptors;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(s_pDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&s_pRtvHeap)))
		|| FAILED(s_pDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&s_pSrvHeap))))
		return false;
	for (UINT i = 0; i < s_nBuffers; ++i)
	{
		if (FAILED(s_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_frames[i].pAllocator))))
			return false;
	}
	if (FAILED(s_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_frames[0].pAllocator, nullptr, IID_PPV_ARGS(&s_pCommandList))))
		return false;
	s_pCommandList->Close();
	if (FAILED(s_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_pFence))))
		return false;
	s_hFenceEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
	if (!Overlay_CreateBackBuffers())
		return false;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = "platform/imgui_s30.ini";
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

	// Scale with the window: the default 13 px font is unreadable at 1440p+.
	const float flScale = desc.BufferDesc.Height > 1080 ? desc.BufferDesc.Height / 1080.0f : 1.0f;
	ImFontConfig fontConfig;
	fontConfig.SizePixels = 13.0f * flScale + 2.0f;
	io.Fonts->AddFontDefault(&fontConfig);
	ImGui::GetStyle().ScaleAllSizes(flScale);

	ImGui_ImplWin32_Init(s_hWnd);
	ImGui_ImplDX12_InitInfo initInfo;
	initInfo.Device = s_pDevice;
	initInfo.CommandQueue = s_pGameQueue;
	initInfo.NumFramesInFlight = static_cast<int>(s_nBuffers);
	initInfo.RTVFormat = s_eRtvFormat;
	initInfo.SrvDescriptorHeap = s_pSrvHeap;
	initInfo.SrvDescriptorAllocFn = Overlay_SrvAlloc;
	initInfo.SrvDescriptorFreeFn = Overlay_SrvFree;
	if (!ImGui_ImplDX12_Init(&initInfo))
		return false;

	S30Panels_Init();
	io.MouseDrawCursor = S30Overlay_IsVisible();
	v_WndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(s_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(Overlay_WndProc)));

	char szTitle[128] = {};
	GetWindowTextA(s_hWnd, szTitle, sizeof(szTitle));
	S30Log("[OVERLAY] ImGui %s ready: window \"%s\" %ux%u, %u buffers, format %d, queue %p\n", IMGUI_VERSION, szTitle,
		desc.BufferDesc.Width, desc.BufferDesc.Height, s_nBuffers, static_cast<int>(s_eRtvFormat), static_cast<void*>(s_pGameQueue));
	S30Log("[OVERLAY] press ` or F10 to open the SDK overlay\n");
	return true;
}

//-----------------------------------------------------------------------------
// Frame
//-----------------------------------------------------------------------------
static void Overlay_RenderFrame(void)
{
	const UINT nIndex = s_pSwapChain->GetCurrentBackBufferIndex();
	if (nIndex >= s_nBuffers || !s_frames[nIndex].pBackBuffer)
		return;
	OverlayFrame_t& frame = s_frames[nIndex];

	if (s_pFence->GetCompletedValue() < frame.nFenceValue)
	{
		s_pFence->SetEventOnCompletion(frame.nFenceValue, s_hFenceEvent);
		WaitForSingleObject(s_hFenceEvent, 1000);
	}

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	S30Panels_Draw();
	ImGui::Render();

	frame.pAllocator->Reset();
	s_pCommandList->Reset(frame.pAllocator, nullptr);

	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = frame.pBackBuffer;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	s_pCommandList->ResourceBarrier(1, &barrier);
	s_pCommandList->OMSetRenderTargets(1, &frame.hRtv, FALSE, nullptr);
	ID3D12DescriptorHeap* ppHeaps[] = { s_pSrvHeap };
	s_pCommandList->SetDescriptorHeaps(1, ppHeaps);
	ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), s_pCommandList);
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	s_pCommandList->ResourceBarrier(1, &barrier);
	s_pCommandList->Close();

	ID3D12CommandList* ppLists[] = { s_pCommandList };
	v_ExecuteCommandLists(s_pGameQueue, 1, ppLists);
	frame.nFenceValue = ++s_nFenceValue;
	s_pGameQueue->Signal(s_pFence, frame.nFenceValue);

	const LONG64 nRendered = InterlockedIncrement64(&s_nOverlayFrames);
	if (nRendered == 1 || nRendered % 600 == 0)
	{
		const HRESULT hrRemoved = s_pDevice->GetDeviceRemovedReason();
		S30Log("[OVERLAY] %lld overlay frames rendered, device %s (0x%08lX)\n", nRendered, hrRemoved == S_OK ? "ok" : "REMOVED", hrRemoved);
	}
}

//-----------------------------------------------------------------------------
// Screenshot: copy the current back buffer (overlay included) to a readback
// buffer on the game queue, wait for it, and write a 24-bit BMP. Development
// aid for checking what the overlay draws; 8-bit RGBA/BGRA formats only.
//-----------------------------------------------------------------------------
static volatile LONG s_bScreenshotRequested = 0;
static char s_szScreenshotPath[MAX_PATH] = {};

void S30Overlay_RequestScreenshot(const char* pszPath)
{
	strncpy_s(s_szScreenshotPath, pszPath, _TRUNCATE);
	InterlockedExchange(&s_bScreenshotRequested, 1);
}

static void Overlay_TakeScreenshot(void)
{
	const UINT nIndex = s_pSwapChain->GetCurrentBackBufferIndex();
	ID3D12Resource* const pBack = nIndex < s_nBuffers ? s_frames[nIndex].pBackBuffer : nullptr;
	const bool bBgra = s_eRtvFormat == DXGI_FORMAT_B8G8R8A8_UNORM || s_eRtvFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	const bool bRgba = s_eRtvFormat == DXGI_FORMAT_R8G8B8A8_UNORM || s_eRtvFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	if (!pBack || (!bBgra && !bRgba))
	{
		S30Log("[OVERLAY] screenshot: unsupported back buffer format %d\n", static_cast<int>(s_eRtvFormat));
		return;
	}

	const D3D12_RESOURCE_DESC desc = pBack->GetDesc();
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
	UINT64 nTotalBytes = 0;
	s_pDevice->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &nTotalBytes);

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = nTotalBytes;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	ID3D12Resource* pReadback = nullptr;
	if (FAILED(s_pDevice->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		IID_PPV_ARGS(&pReadback))))
		return;

	OverlayFrame_t& frame = s_frames[nIndex];
	Overlay_WaitForGpu();
	frame.pAllocator->Reset();
	s_pCommandList->Reset(frame.pAllocator, nullptr);
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = pBack;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	s_pCommandList->ResourceBarrier(1, &barrier);
	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = pReadback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = footprint;
	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = pBack;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	s_pCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	s_pCommandList->ResourceBarrier(1, &barrier);
	s_pCommandList->Close();
	ID3D12CommandList* ppLists[] = { s_pCommandList };
	v_ExecuteCommandLists(s_pGameQueue, 1, ppLists);
	Overlay_WaitForGpu();
	frame.nFenceValue = s_nFenceValue;

	const UINT nWidth = static_cast<UINT>(desc.Width);
	const UINT nHeight = desc.Height;
	const UINT nRowOut = (nWidth * 3 + 3) & ~3u;
	uint8_t* pMapped = nullptr;
	FILE* pFile = nullptr;
	if (SUCCEEDED(pReadback->Map(0, nullptr, reinterpret_cast<void**>(&pMapped))) && !fopen_s(&pFile, s_szScreenshotPath, "wb"))
	{
		BITMAPFILEHEADER bfh = {};
		BITMAPINFOHEADER bih = {};
		bfh.bfType = 0x4D42;
		bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
		bfh.bfSize = bfh.bfOffBits + nRowOut * nHeight;
		bih.biSize = sizeof(bih);
		bih.biWidth = static_cast<LONG>(nWidth);
		bih.biHeight = static_cast<LONG>(nHeight); // bottom-up
		bih.biPlanes = 1;
		bih.biBitCount = 24;
		fwrite(&bfh, sizeof(bfh), 1, pFile);
		fwrite(&bih, sizeof(bih), 1, pFile);
		static uint8_t s_row[16384 * 3 + 4];
		for (int y = static_cast<int>(nHeight) - 1; y >= 0; --y)
		{
			const uint8_t* pSrc = pMapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
			for (UINT x = 0; x < nWidth && x < 16384; ++x)
			{
				const uint8_t* px = pSrc + x * 4;
				s_row[x * 3 + 0] = bBgra ? px[0] : px[2];
				s_row[x * 3 + 1] = px[1];
				s_row[x * 3 + 2] = bBgra ? px[2] : px[0];
			}
			fwrite(s_row, 1, nRowOut, pFile);
		}
		fclose(pFile);
		S30Log("[OVERLAY] screenshot %ux%u written: %s\n", nWidth, nHeight, s_szScreenshotPath);
	}
	if (pMapped)
		pReadback->Unmap(0, nullptr);
	pReadback->Release();
}

static void Overlay_OnPresent(IDXGISwapChain* pSwapChain)
{
	InterlockedIncrement64(&s_nPresents);
	if (s_bFailed || !s_pGameQueue)
		return;

	EnterCriticalSection(&s_imguiLock);
	if (!s_bImGuiReady)
	{
		s_bImGuiReady = Overlay_InitImGui(pSwapChain);
		if (!s_bImGuiReady)
		{
			s_bFailed = true;
			S30Log("[OVERLAY] ImGui init FAILED -- overlay disabled\n");
		}
	}
	// Draw only while open: a closed overlay costs nothing per frame.
	if (s_bImGuiReady && S30Overlay_IsVisible())
		Overlay_RenderFrame();
	if (s_bImGuiReady && InterlockedExchange(&s_bScreenshotRequested, 0))
		Overlay_TakeScreenshot();
	LeaveCriticalSection(&s_imguiLock);
}

//-----------------------------------------------------------------------------
// Hooks
//-----------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE h_Present(IDXGISwapChain* pSwapChain, UINT nSyncInterval, UINT nFlags)
{
	if (!(nFlags & DXGI_PRESENT_TEST))
	{
		Overlay_OnPresent(pSwapChain);
		S30AnimTrace_OnFrame();
	}
	return v_Present(pSwapChain, nSyncInterval, nFlags);
}

static HRESULT STDMETHODCALLTYPE h_Present1(IDXGISwapChain1* pSwapChain, UINT nSyncInterval, UINT nFlags, const DXGI_PRESENT_PARAMETERS* pParams)
{
	if (!(nFlags & DXGI_PRESENT_TEST))
	{
		Overlay_OnPresent(pSwapChain);
		S30AnimTrace_OnFrame();
	}
	return v_Present1(pSwapChain, nSyncInterval, nFlags, pParams);
}

static HRESULT STDMETHODCALLTYPE h_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT nCount, UINT nWidth, UINT nHeight, DXGI_FORMAT eFormat, UINT nFlags)
{
	EnterCriticalSection(&s_imguiLock);
	if (s_bImGuiReady)
		Overlay_ReleaseBackBuffers();
	const HRESULT hr = v_ResizeBuffers(pSwapChain, nCount, nWidth, nHeight, eFormat, nFlags);
	if (s_bImGuiReady && SUCCEEDED(hr))
		Overlay_CreateBackBuffers();
	LeaveCriticalSection(&s_imguiLock);
	return hr;
}

static HRESULT STDMETHODCALLTYPE h_ResizeBuffers1(IDXGISwapChain3* pSwapChain, UINT nCount, UINT nWidth, UINT nHeight, DXGI_FORMAT eFormat,
	UINT nFlags, const UINT* pNodeMasks, IUnknown* const* ppQueues)
{
	EnterCriticalSection(&s_imguiLock);
	if (s_bImGuiReady)
		Overlay_ReleaseBackBuffers();
	const HRESULT hr = v_ResizeBuffers1(pSwapChain, nCount, nWidth, nHeight, eFormat, nFlags, pNodeMasks, ppQueues);
	if (s_bImGuiReady && SUCCEEDED(hr))
		Overlay_CreateBackBuffers();
	LeaveCriticalSection(&s_imguiLock);
	return hr;
}

static void STDMETHODCALLTYPE h_ExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT nLists, ID3D12CommandList* const* ppLists)
{
	if (!s_pGameQueue)
	{
		const D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
		if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT
			&& !InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&s_pGameQueue), pQueue, nullptr))
		{
			pQueue->AddRef();
			S30Log("[OVERLAY] captured direct command queue %p\n", static_cast<void*>(pQueue));
		}
	}
	v_ExecuteCommandLists(pQueue, nLists, ppLists);
}

//-----------------------------------------------------------------------------
bool S30Overlay_Install(void)
{
	InitializeCriticalSection(&s_imguiLock);
	if (strstr(GetCommandLineA(), "-s30_overlay"))
		s_bVisible = 1; // start open

	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0, GetModuleHandleW(NULL), NULL, NULL, NULL, NULL, L"R5FS30Dummy", NULL };
	RegisterClassExW(&wc);
	const HWND hDummy = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);

	ID3D12Device* pDevice = nullptr;
	ID3D12CommandQueue* pQueue = nullptr;
	IDXGIFactory4* pFactory = nullptr;
	IDXGISwapChain1* pSwapChain = nullptr;
	bool bOk = false;
	do
	{
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))))
			break;
		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))))
			break;
		if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory))))
			break;
		DXGI_SWAP_CHAIN_DESC1 scDesc = {};
		scDesc.BufferCount = 2;
		scDesc.Width = 64;
		scDesc.Height = 64;
		scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scDesc.SampleDesc.Count = 1;
		if (FAILED(pFactory->CreateSwapChainForHwnd(pQueue, hDummy, &scDesc, nullptr, nullptr, &pSwapChain)))
			break;

		void** ppSwapChainVt = *reinterpret_cast<void***>(pSwapChain);
		void** ppQueueVt = *reinterpret_cast<void***>(pQueue);
		v_Present = reinterpret_cast<Present_t>(ppSwapChainVt[8]);
		v_ResizeBuffers = reinterpret_cast<ResizeBuffers_t>(ppSwapChainVt[13]);
		v_Present1 = reinterpret_cast<Present1_t>(ppSwapChainVt[22]);
		v_ResizeBuffers1 = reinterpret_cast<ResizeBuffers1_t>(ppSwapChainVt[39]);
		v_ExecuteCommandLists = reinterpret_cast<ExecuteCommandLists_t>(ppQueueVt[10]);
		bOk = true;
	} while (false);

	if (pSwapChain) pSwapChain->Release();
	if (pFactory) pFactory->Release();
	if (pQueue) pQueue->Release();
	if (pDevice) pDevice->Release();
	DestroyWindow(hDummy);
	UnregisterClassW(wc.lpszClassName, wc.hInstance);

	if (!bOk)
	{
		S30Log("[OVERLAY] could not create the vtable probe device -- overlay disabled\n");
		return false;
	}

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_Present), h_Present);
	DetourAttach(&reinterpret_cast<PVOID&>(v_Present1), h_Present1);
	DetourAttach(&reinterpret_cast<PVOID&>(v_ResizeBuffers), h_ResizeBuffers);
	DetourAttach(&reinterpret_cast<PVOID&>(v_ResizeBuffers1), h_ResizeBuffers1);
	DetourAttach(&reinterpret_cast<PVOID&>(v_ExecuteCommandLists), h_ExecuteCommandLists);
	const LONG nError = DetourTransactionCommit();
	S30Log("[OVERLAY] DXGI/D3D12 hooks %s (commit %ld)\n", nError == NO_ERROR ? "armed" : "FAILED", nError);
	return nError == NO_ERROR;
}
