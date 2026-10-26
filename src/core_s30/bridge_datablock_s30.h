#ifndef CORE_S30_BRIDGE_DATABLOCK_H
#define CORE_S30_BRIDGE_DATABLOCK_H
//=============================================================================//
//
// Purpose: the S3 dedi's signon DataBlock for the S30 bridge.
//
// After CONNACCEPT the dedi sends the signon message stream as a DataBlock:
// connectionless 'O' (0x4F) fragments, acknowledged with 'P' (0x50). The
// reassembled block is a compression byte plus LZ4 data holding S3 net
// messages (7-bit type + body). Format from the S21 bridge
// (net_bridge_process.cpp S21Bridge_Handle0x4F, net_bridge_install.cpp
// S21Bridge_SendDataBlockAck / OnDataBlockComplete).
//
//=============================================================================//
#include <cstdint>

typedef int (*S30DataBlockSend_t)(const uint8_t* pData, int nLen);

// Resets the reassembly state; pfnSend sends on the bridge socket.
void S30DataBlock_Reset(S30DataBlockSend_t pfnSend);
// One 0x4F datagram (with its ffffffff header). Returns false if malformed.
bool S30DataBlock_OnFragment(const uint8_t* pData, int nLen);

// The last completed transfer, decompressed (S3 message stream); null if none.
// The buffer stays valid until the next transfer completes.
const uint8_t* S30DataBlock_GetSignon(int* pnSize, int* pnTransferNr);
// Hands each completed transfer to the caller once (a malloc'd copy, 16 zero
// bytes past the end; the caller frees it); null if none is pending.
uint8_t* S30DataBlock_TakeSignon(int* pnSize, int* pnTransferNr);

#endif // CORE_S30_BRIDGE_DATABLOCK_H
