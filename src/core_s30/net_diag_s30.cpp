//=============================================================================//
//
// Purpose: -s30_netdiag observers on the S30 client connect path.
//
// S30.1 sites (docs/09):
//   0x2FD3B0  client challenge/resend (S21 DX12 0x2D4A00, "ChallengeBuilder"):
//             runs when the client state at +196 is 1 or the byte at +192 is
//             set; writes 0xFFFFFFFF, type 18, protocol, 64-bit id, platform,
//             a 256-byte string, then sends with encryption mode 2.
//   0x2A6DA0  engine packet send (sock, ?, netadr*, data, len, encrypt mode).
// Observe-only: both hooks log and call through.
//
//=============================================================================//
#include <windows.h>
#include <cstdio>
#include "thirdparty/detours/include/detours.h"
#include "s30_log.h"
#include "s30_pattern.h"
#include "net_diag_s30.h"

typedef void (*Resend_t)(unsigned char* pClient);
typedef __int64 (*SendPacket_t)(int nSock, unsigned int nChannel, void* pAddr, const unsigned char* pData, int nLen, int nEncrypt);

static const char* const s_pszPatternResend =
	"40 55 53 48 8D 6C 24 B1 48 81 EC B8 00 00 00 83 B9 C4 00 00 00 01 48 8B D9 74 0D 80 B9 C0 00 00 00 00 0F 84";
static const char* const s_pszPatternSendPacket =
	"48 89 5C 24 10 48 89 74 24 20 57 41 56 41 57 48 83 EC 40 44 8B 94 24 88 00 00 00 4D 8B D8";

static Resend_t v_Resend = nullptr;
static SendPacket_t v_SendPacket = nullptr;
static ULONGLONG s_nLastResendLog = 0;
static volatile LONG s_nResendCalls = 0;
static volatile LONG s_nSendCalls = 0;

static void h_Resend(unsigned char* pClient)
{
	const LONG nCalls = InterlockedIncrement(&s_nResendCalls);
	const ULONGLONG nNow = GetTickCount64();
	const int nState = *reinterpret_cast<const int*>(pClient + 196);
	if (nState == 1 || pClient[192] || nNow - s_nLastResendLog > 5000)
	{
		if (nNow - s_nLastResendLog > 1000)
		{
			s_nLastResendLog = nNow;
			S30LogFile("[NET-DIAG] resend #%ld: state(+196)=%d +192=%u +193=%u +194=%u retries %d/%d\n", nCalls, nState,
				pClient[192], pClient[193], pClient[194], *reinterpret_cast<const int*>(pClient + 120), *reinterpret_cast<const int*>(pClient + 124));
		}
	}
	v_Resend(pClient);
}

static __int64 h_SendPacket(int nSock, unsigned int nChannel, void* pAddr, const unsigned char* pData, int nLen, int nEncrypt)
{
	if (InterlockedIncrement(&s_nSendCalls) <= 30)
	{
		char szHex[24 * 3 + 1] = {};
		for (int i = 0; i < nLen && i < 24; ++i)
			snprintf(szHex + i * 3, 4, "%02X ", pData[i]);
		S30LogFile("[NET-DIAG] SendPacket sock=%d ch=%u len=%d encrypt=%d: %s\n", nSock, nChannel, nLen, nEncrypt, szHex);
	}
	return v_SendPacket(nSock, nChannel, pAddr, pData, nLen, nEncrypt);
}

void S30NetDiag_Install(void)
{
	if (!strstr(GetCommandLineA(), "-s30_netdiag"))
		return;
	int nResend = 0;
	int nSend = 0;
	v_Resend = reinterpret_cast<Resend_t>(S30Pattern_FindUnique(s_pszPatternResend, &nResend));
	v_SendPacket = reinterpret_cast<SendPacket_t>(S30Pattern_FindUnique(s_pszPatternSendPacket, &nSend));
	if (!v_Resend || !v_SendPacket)
	{
		S30Log("[NET-DIAG] sites unresolved (resend %d, send %d)\n", nResend, nSend);
		return;
	}
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_Resend), h_Resend);
	DetourAttach(&reinterpret_cast<PVOID&>(v_SendPacket), h_SendPacket);
	const LONG nError = DetourTransactionCommit();
	S30Log("[NET-DIAG] resend + SendPacket observers %s (commit %ld)\n", nError == NO_ERROR ? "armed" : "FAILED", nError);
}
