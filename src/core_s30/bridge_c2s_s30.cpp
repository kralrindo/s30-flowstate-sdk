//=============================================================================//
//
// Purpose: the S30 client's own client-to-server messages, relayed to the S3 dedi.
//
// S30.1 facts (s30/docs/11, docs/15):
//   INetMessage vtable: slot 5 WriteToBuffer(msg, bf_write*), slot 7 GetType.
//   bf_write (as ClWriteMoveCmds 0x2F5320 builds it): data +0, bytes +8, bits
//   +0xC, current bit +0x10, overflow +0x14, assert on overflow +0x15, name +0x18.
//   WriteToBuffer writes the body only; SendNetMsg writes the 7-bit type
//   (t05_wtb starts with the 8-bit count). The writer ORs bits into dwords.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "bridge_c2s_s30.h"
#include "bridge_bits_s30.h"
#include "bridge_netchan_s30.h"
#include "bridge_scriptremote_s30.h"
#include "s30_log.h"

struct BfWrite_t
{
	uint8_t* pData;
	int nDataBytes;
	int nDataBits;
	int nCurBit;
	bool bOverflow;
	bool bAssertOnOverflow;
	const char* pszDebugName;
};
static_assert(sizeof(BfWrite_t) == 32, "S30 bf_write is 32 bytes");

typedef bool (*WriteToBuffer_t)(void* pMsg, BfWrite_t* pBuf);
typedef int (*GetType_t)(void* pMsg);

static const int kS30StringCmd = 3, kS30ScriptMsg = 4, kS30SetConVar = 5, kS30ClaimPickup = 70, kS30SayText = 73, kS30Gamepad = 82;
static const int kRedundantSends = 2;    // bridge_c2s_rel_redundancy / bridge_c2s_stringcmd_redundancy
static const int kResendMs = 50;         // bridge_c2s_rel_resend_ms

static volatile LONG s_nStrCmdSeq = 0;   // the dedi's m_nBridgeStrCmdSeqHigh lane
static volatile LONG s_nRelSeq = 0;      // AcceptBridgeRelSeq: chat and script calls

void S30C2S_Reset(void)
{
	InterlockedExchange(&s_nStrCmdSeq, 0);
	InterlockedExchange(&s_nRelSeq, 0);
}

// The message body serialized by its own writer (no type).
static bool C2S_Serialize(void* pMsg, uint8_t* pOut, int nOutBytes, int* pnBits, int* pnType)
{
	void** const pVtable = *static_cast<void***>(pMsg);
	BfWrite_t bf = { pOut, nOutBytes, nOutBytes * 8, 0, false, false, "bridge.c2s" };
	__try
	{
		*pnType = reinterpret_cast<GetType_t>(pVtable[7])(pMsg);
		if (*pnType != kS30StringCmd && *pnType != kS30ScriptMsg && *pnType != kS30SetConVar && *pnType != kS30ClaimPickup
			&& *pnType != kS30SayText && *pnType != kS30Gamepad)
			return false;
		if (!reinterpret_cast<WriteToBuffer_t>(pVtable[5])(pMsg, &bf) || bf.bOverflow)
		{
			static volatile LONG s_nFail = 0;
			if (InterlockedIncrement(&s_nFail) <= 16)
				S30Log("[C2S] S30 %d: WriteToBuffer failed (%d bits, overflow %d) -- not relayed\n", *pnType, bf.nCurBit, bf.bOverflow);
			return false;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
	*pnBits = bf.nCurBit;
	return bf.nCurBit > 0;
}

static bool C2S_Queue(const S30BitWriter_t& w, int nSends, const char* pszWhat)
{
	if (w.m_bOverflow)
		return false;
	const bool bOk = S30Netchan_QueueC2S(w.m_pData, w.m_nPos, nSends, kResendMs);
	static volatile LONG s_nLog = 0;
	if (!bOk || InterlockedIncrement(&s_nLog) <= 32)
		S30LogFile("[C2S] %s: %d bits, %d sends%s\n", pszWhat, w.m_nPos, nSends, bOk ? "" : " -- queue full, dropped");
	return bOk;
}

bool S30C2S_OnEngineMessage(void* pMsg)
{
	if (!pMsg)
		return false;
	alignas(4) uint8_t src[4096] = {}; // the writer ORs bits into dwords (0x2380C0)
	int nBits = 0, nType = -1;
	if (!C2S_Serialize(pMsg, src, sizeof(src), &nBits, &nType))
		return false;
	S30BitReader_t r(src, (nBits + 7) / 8);
	uint8_t out[4200];
	S30BitWriter_t w(out, sizeof(out));
	switch (nType)
	{
	case kS30StringCmd: // [string] -> S3 3 "brq <seq> <command>"
	{
		char szCmd[1024], szOut[1100];
		if (!r.ReadString(szCmd, sizeof(szCmd), 1024) || !szCmd[0])
			return false;
		const uint32_t nSeq = static_cast<uint32_t>(InterlockedIncrement(&s_nStrCmdSeq));
		snprintf(szOut, sizeof(szOut), "brq %u %s", nSeq, szCmd);
		w.Write(3, 7);
		w.WriteString(szOut, 1024);
		S30Log("[C2S] StringCmd \"%s\" (seq %u)\n", szCmd, nSeq);
		return C2S_Queue(w, 1 + kRedundantSends, "StringCmd");
	}
	case kS30ScriptMsg: // Remote_ServerCallFunction -> S3 68, by name (bridge_scriptremote)
		return S30ScriptRemote_RelayC2S(r, nBits, static_cast<uint32_t>(InterlockedIncrement(&s_nRelSeq)));
	case kS30SetConVar: // [8 count]([name][value])* -> S3 4; the dedi's value buffer is 260 bytes
	{
		const uint32_t nPairs = r.Read(8);
		static char s_szNames[255][256], s_szValues[255][260];
		uint32_t nKept = 0;
		for (uint32_t i = 0; i < nPairs; ++i)
		{
			char szName[256], szValue[4096];
			if (!r.ReadString(szName, sizeof(szName), 256) || !r.ReadString(szValue, sizeof(szValue), 4096))
				return false;
			if (strlen(szValue) >= sizeof(s_szValues[0]))
			{
				S30Log("[C2S] SetConVar %s: value of %zu bytes does not fit the dedi -- not sent\n", szName, strlen(szValue));
				continue;
			}
			strcpy_s(s_szNames[nKept], szName);
			strcpy_s(s_szValues[nKept], szValue);
			++nKept;
		}
		// One message per queue slot (1100 bytes): split the pairs across messages.
		bool bOk = nKept > 0;
		for (uint32_t nFirst = 0; nFirst < nKept && bOk; )
		{
			uint8_t part[1100];
			S30BitWriter_t p(part, sizeof(part));
			uint32_t nCount = 0, nBytes = 2;
			while (nFirst + nCount < nKept
				&& nBytes + strlen(s_szNames[nFirst + nCount]) + strlen(s_szValues[nFirst + nCount]) + 2 < sizeof(part) - 8)
			{
				nBytes += static_cast<uint32_t>(strlen(s_szNames[nFirst + nCount]) + strlen(s_szValues[nFirst + nCount]) + 2);
				++nCount;
			}
			if (!nCount)
				return false;
			p.Write(4, 7);
			p.Write(nCount, 8);
			for (uint32_t i = nFirst; i < nFirst + nCount; ++i)
			{
				p.WriteString(s_szNames[i], 256);
				p.WriteString(s_szValues[i], 260);
			}
			bOk = C2S_Queue(p, 2, "SetConVar");
			nFirst += nCount;
		}
		return bOk;
	}
	case kS30SayText: // [string][1 team] -> S3 61 "brc <seq> <text>"
	{
		char szText[256], szOut[300];
		if (!r.ReadString(szText, sizeof(szText), 256) || !szText[0])
			return false;
		const uint32_t bTeam = r.Read(1);
		const uint32_t nSeq = static_cast<uint32_t>(InterlockedIncrement(&s_nRelSeq));
		snprintf(szOut, sizeof(szOut), "brc %u %s", nSeq, szText);
		w.Write(61, 7);
		w.WriteString(szOut, 256);
		w.Write(bTeam, 1);
		S30Log("[C2S] chat%s \"%s\" (seq %u)\n", bTeam ? " (team)" : "", szText, nSeq);
		return C2S_Queue(w, 1 + kRedundantSends, "ClientSayText");
	}
	case kS30ClaimPickup: // same body -> S3 57
		w.Write(57, 7);
		w.WriteFrom(r, nBits);
		return C2S_Queue(w, 1, "ClaimClientSidePickup");
	case kS30Gamepad: // -> S3 65: S30 added a 32-bit int after the first 40 body bits (docs/15)
	{
		if (nBits < 72)
			return false;
		w.Write(65, 7);
		w.WriteFrom(r, 40);
		r.Skip(32);
		w.WriteFrom(r, nBits - 72);
		return C2S_Queue(w, 1, "GamepadMsg");
	}
	default:
		return false;
	}
}
