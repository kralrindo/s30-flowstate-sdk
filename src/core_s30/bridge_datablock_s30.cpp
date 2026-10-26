//=============================================================================//
//
// Purpose: the S3 dedi's signon DataBlock for the S30 bridge.
//
// Fragment: ffffffff 4F, transferId u16, transferSize u32, transferNr u16,
// blockNr u16, blockSize u32, data (19-byte header, fragments of 1024 bytes).
// ACK: ffffffff 50, transferId u16, transferNr u16, then a bit stream: flag bit
// (1 = a received-block bitmap follows, one bit per block). transferId+1 with
// the flag clear acknowledges the whole transfer.
// Block: byte 0 = compressed flag, then LZ4 (or raw) S3 net messages.
//
//=============================================================================//
#include "bridge_datablock_s30.h"

#include <windows.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "thirdparty/lz4/lz4.h"
#include "engine/client/net_bridge_skip.h" // engine-free S3 message body widths
#include "s30_log.h"

static const int kHeaderSize = 19;
static const int kMaxFragSize = 1024;     // MAX_DATABLOCK_FRAGMENT_SIZE
static const int kMaxFragments = 4096;    // MAX_DATABLOCK_FRAGMENTS
static const int kMaxTransfer = 4 << 20;  // the dedi's signon buffer

static S30DataBlockSend_t s_pfnSend = nullptr;
static std::vector<uint8_t> s_scratch;
static std::vector<uint8_t> s_received;   // per block: 1 = held
static int s_nTransferId = -1;
static int s_nTransferNr = -1;
static int s_nTransferSize = 0;
static int s_nTotalBlocks = 0;
static int s_nBlocksReceived = 0;
static bool s_bComplete = false;

static SRWLOCK s_signonLock = SRWLOCK_INIT;
static std::vector<uint8_t> s_signon;
static int s_nSignonTransferNr = -1;
static bool s_bSignonPending = false;

static void DataBlock_SendAck(bool bComplete)
{
	if (!s_pfnSend)
		return;
	uint8_t buf[16 + kMaxFragments / 8] = {};
	int nPos = 0;
	memset(buf, 0xFF, 4);
	nPos = 4;
	buf[nPos++] = 0x50;
	const uint16_t nId = static_cast<uint16_t>(bComplete ? s_nTransferId + 1 : s_nTransferId);
	const uint16_t nNr = static_cast<uint16_t>(s_nTransferNr);
	memcpy(buf + nPos, &nId, 2);
	nPos += 2;
	memcpy(buf + nPos, &nNr, 2);
	nPos += 2;
	if (bComplete)
	{
		buf[nPos++] = 0; // flag clear
	}
	else
	{
		uint8_t* const pBits = buf + nPos;
		int nBit = 0;
		pBits[0] |= 1; // bitmap follows
		++nBit;
		for (int i = 0; i < s_nTotalBlocks; ++i, ++nBit)
		{
			if (s_received[i])
				pBits[nBit >> 3] |= static_cast<uint8_t>(1u << (nBit & 7));
		}
		nPos += (nBit + 7) >> 3;
	}
	s_pfnSend(buf, nPos);
}

// Walks an S3 message stream and logs each message's type and bit size.
static void DataBlock_LogMessages(const uint8_t* pData, int nSize)
{
	const int nTotalBits = nSize * 8;
	int nBit = 0;
	int nMessages = 0;
	char szLine[512];
	int nLine = 0;
	while (nBit + 7 <= nTotalBits)
	{
		const int nStart = nBit;
		const uint32_t nType = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBit, 7);
		if (nType == 0)
			continue; // net_NOP / padding
		if (nType == 1)
		{
			S30LogFile("[DATABLOCK] msg net_Disconnect at bit %d -- stop\n", nStart);
			break;
		}
		bool bOk;
		if (nType == 8)
		{
			// svc_SendTable: needs-decoder bit, 32-bit payload length in bits, payload.
			bOk = nBit + 33 <= nTotalBits;
			if (bOk)
			{
				S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBit, 1);
				const uint32_t nLen = S21Bridge_Skip_ReadUBits(pData, nTotalBits, nBit, 32);
				bOk = nLen <= static_cast<uint32_t>(nTotalBits - nBit);
				if (bOk)
					nBit += static_cast<int>(nLen);
			}
		}
		else
			bOk = S21Bridge_SkipNonSendTable(pData, nTotalBits, nBit, nType);
		++nMessages;
		nLine += snprintf(szLine + nLine, sizeof(szLine) - nLine, " %u(%d)", nType, nBit - nStart);
		if (nLine > 400)
		{
			S30LogFile("[DATABLOCK] msgs:%s\n", szLine);
			nLine = 0;
		}
		if (!bOk)
		{
			S30Log("[DATABLOCK] message %d type %u at bit %d: body not walkable -- stop listing\n", nMessages, nType, nStart);
			break;
		}
	}
	if (nLine)
		S30LogFile("[DATABLOCK] msgs:%s\n", szLine);
	S30Log("[DATABLOCK] %d messages walked, %d of %d bits\n", nMessages, nBit, nTotalBits);
}

static void DataBlock_OnComplete(void)
{
	DataBlock_SendAck(true);
	const uint8_t* const pRaw = s_scratch.data();
	std::vector<uint8_t> out;
	if (pRaw[0])
	{
		out.resize(kMaxTransfer);
		const int nOut = LZ4_decompress_safe(reinterpret_cast<const char*>(pRaw + 1), reinterpret_cast<char*>(out.data()),
			s_nTransferSize - 1, kMaxTransfer);
		if (nOut < 0)
		{
			S30Log("[DATABLOCK] transfer %d: LZ4 failed (%d), %d compressed bytes\n", s_nTransferNr, nOut, s_nTransferSize - 1);
			return;
		}
		out.resize(nOut);
	}
	else
		out.assign(pRaw + 1, pRaw + s_nTransferSize);
	S30Log("[DATABLOCK] transfer %d complete: %d blocks, %d bytes -> %d bytes of messages\n",
		s_nTransferNr, s_nTotalBlocks, s_nTransferSize, static_cast<int>(out.size()));
	DataBlock_LogMessages(out.data(), static_cast<int>(out.size()));

	AcquireSRWLockExclusive(&s_signonLock);
	s_signon.swap(out);
	s_nSignonTransferNr = s_nTransferNr;
	s_bSignonPending = true;
	ReleaseSRWLockExclusive(&s_signonLock);
}

void S30DataBlock_Reset(S30DataBlockSend_t pfnSend)
{
	s_pfnSend = pfnSend;
	s_nTransferId = s_nTransferNr = -1;
	s_nTransferSize = s_nTotalBlocks = s_nBlocksReceived = 0;
	s_bComplete = false;
	AcquireSRWLockExclusive(&s_signonLock);
	s_bSignonPending = false;
	ReleaseSRWLockExclusive(&s_signonLock);
}

bool S30DataBlock_OnFragment(const uint8_t* pData, int nLen)
{
	if (nLen < kHeaderSize || pData[4] != 0x4F)
		return false;
	uint16_t nId, nNr, nBlock;
	uint32_t nSize, nBlockSize;
	memcpy(&nId, pData + 5, 2);
	memcpy(&nSize, pData + 7, 4);
	memcpy(&nNr, pData + 11, 2);
	memcpy(&nBlock, pData + 13, 2);
	memcpy(&nBlockSize, pData + 15, 4);
	if (nBlockSize > static_cast<uint32_t>(nLen - kHeaderSize) || nBlockSize > kMaxFragSize)
		return false;

	if (s_bComplete && nNr == s_nTransferNr && nId == s_nTransferId)
	{
		DataBlock_SendAck(true); // the dedi resends a transfer we already hold
		return true;
	}
	if (nNr != s_nTransferNr || s_bComplete)
	{
		if (nSize == 0 || nSize > static_cast<uint32_t>(kMaxTransfer))
			return false;
		const int nBlocks = static_cast<int>((nSize + kMaxFragSize - 1) / kMaxFragSize);
		if (nBlocks > kMaxFragments)
			return false;
		s_nTransferId = nId;
		s_nTransferNr = nNr;
		s_nTransferSize = static_cast<int>(nSize);
		s_nTotalBlocks = nBlocks;
		s_nBlocksReceived = 0;
		s_bComplete = false;
		s_scratch.assign(static_cast<size_t>(nBlocks) * kMaxFragSize, 0);
		s_received.assign(nBlocks, 0);
		S30Log("[DATABLOCK] transfer %d (id %d): %u bytes in %d blocks\n", nNr, nId, nSize, nBlocks);
	}
	if (nBlock >= s_nTotalBlocks)
		return false;
	const bool bLast = nBlock == s_nTotalBlocks - 1;
	const uint32_t nExpect = bLast ? static_cast<uint32_t>(s_nTransferSize - nBlock * kMaxFragSize) : kMaxFragSize;
	if (nBlockSize != nExpect)
		return false;
	if (!s_received[nBlock])
	{
		memcpy(s_scratch.data() + static_cast<size_t>(nBlock) * kMaxFragSize, pData + kHeaderSize, nBlockSize);
		s_received[nBlock] = 1;
		++s_nBlocksReceived;
	}
	if (s_nBlocksReceived == s_nTotalBlocks)
	{
		s_bComplete = true;
		DataBlock_OnComplete();
	}
	else
		DataBlock_SendAck(false);
	return true;
}

const uint8_t* S30DataBlock_GetSignon(int* pnSize, int* pnTransferNr)
{
	AcquireSRWLockShared(&s_signonLock);
	const uint8_t* const p = s_signon.empty() ? nullptr : s_signon.data();
	if (pnSize)
		*pnSize = static_cast<int>(s_signon.size());
	if (pnTransferNr)
		*pnTransferNr = s_nSignonTransferNr;
	ReleaseSRWLockShared(&s_signonLock);
	return p;
}

uint8_t* S30DataBlock_TakeSignon(int* pnSize, int* pnTransferNr)
{
	uint8_t* pCopy = nullptr;
	AcquireSRWLockExclusive(&s_signonLock);
	if (s_bSignonPending && !s_signon.empty())
	{
		pCopy = static_cast<uint8_t*>(malloc(s_signon.size() + 16));
		if (pCopy)
		{
			memcpy(pCopy, s_signon.data(), s_signon.size());
			memset(pCopy + s_signon.size(), 0, 16);
			*pnSize = static_cast<int>(s_signon.size());
			*pnTransferNr = s_nSignonTransferNr;
			s_bSignonPending = false;
		}
	}
	ReleaseSRWLockExclusive(&s_signonLock);
	return pCopy;
}
