//=============================================================================//
//
// Purpose: S3 dedi connection, stage 2: the netchannel.
//
// S30.1 facts (s30/docs/12, verified in the S30 IDB):
//   0x29FF80  CNetChan::ProcessPacket(chan, packet): Seek(packet+56, 0), header
//             (0x29EB10: in seq -> chan+8, out seq ack -> chan+12), last
//             received -> chan+8432 (= net time 0x2801CD8), handler (chan+8568)
//             slot 5 PacketStart(handler, chan+8, chan+12), subchannel,
//             ProcessMessages (0x29E2C0), handler slot 6 PacketEnd.
//   netpacket: +0 sender netadr, +48 data, +56 bf_read, +120 size.
//   CNetChan: message array (pointer) +0x2180, count +0x2198; RegisterMessage
//             (0x2A1580) stores each message at array[GetType()].
//   message vtable: 3 Process, 4 ReadFromBuffer; handler at +24 (client state+16).
//   0x2FF0C0  CClientState::SetSignonState(cl, state, spawn, msg): writes cl+196;
//             states 0..10 as S21 (9 MAYRECONNECT, 10 CHANGELEVEL).
// The S3 wire format, the format bridges and the C2S packet are the S21
// bridge's (engine/client/net_bridge_process.cpp, net_bridge_c2s.cpp), which
// are proven against this dedi; the S21 -> S30 type and format changes are
// docs/11.
//
//=============================================================================//
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "thirdparty/detours/include/detours.h"

#include "bridge_netchan_s30.h"
#include "bridge_signon_s30.h"
#include "bridge_datablock_s30.h"
#include "bridge_bits_s30.h"
#include "bridge_scriptremote_s30.h"
#include "bridge_usermessage_s30.h"
#include "bridge_c2s_s30.h"
#include "bridge_hud_s30.h"
#include "bridge_props_s30.h"
#include "engine/client/net_bridge_skip.h" // engine-free S3 message body widths
#include "concommand_s30.h"
#include "console_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

//-----------------------------------------------------------------------------
// Engine (S30.1)
//-----------------------------------------------------------------------------
// CNetChan::ProcessPacket prologue through its Seek(packet+56, 0) and the remote
// address check (unique in S30.1).
static const char* const s_pszPatternProcessPacket =
	"48 83 EC 28 48 89 5C 24 30 48 89 74 24 40 48 8B F2 48 89 7C 24 20 48 8B F9 B1 09 E8 ?? ?? ?? ?? "
	"B1 12 E8 ?? ?? ?? ?? 33 D2 48 8D 4E 38 E8 ?? ?? ?? ?? 48 8D 8F C8 32 00 00";
static const int s_nProcessPacketSeekCall = 0x2D;   // call bf_read::Seek (0x5E2C90)
static const int s_nProcessPacketNetTime = 0x96;    // movsd xmm3, [net time]
static const char* const s_pszPatternSetSignonState =
	"40 55 56 57 41 56 41 57 48 8D AC 24 50 FB FF FF 48 81 EC B0 05 00 00 48 8B F9 48 63 F2 48 8B 49 68 4D 8B F1";

static const int NC_IN_SEQ = 8;
static const int NC_OUT_SEQ_ACK = 12;
static const int NC_LAST_RECEIVED = 8432;
static const int NC_HANDLER = 8568;
static const int NC_MESSAGES = 0x2180;
static const int NC_MESSAGE_COUNT = 0x2198;
static const int PKT_DATA = 48;
static const int PKT_SIZE = 120;

typedef void (*CNetChan_ProcessPacket_t)(uint8_t* pChan, uint8_t* pPacket);
typedef char (*bf_read_Seek_t)(uint8_t* pBf, int64_t nBit);
typedef char (*CClientState_SetSignonState_t)(uint8_t* pCl, int nState, int nSpawn, void* pMsg);

// OodleLZ_Compress (0xFE4E60, statically linked Oodle 2: 'oo2::OodleLZ_Compress').
static const char* const s_pszPatternOodleCompress =
	"40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FF FF FF 48 81 EC B8 01 00 00 48 C7 45 D8 FE FF FF FF "
	"48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 A0 00 00 00 4D 8B E1 4C 89 4C 24 68 49 8B F8";
typedef int64_t (*OodleLZ_Compress_t)(int nCompressor, const void* pRaw, int64_t nRaw, void* pComp, int nLevel,
	const void* pOptions, const void* pDictionaryBase, const void* pLrm, void* pScratch, int64_t nScratch);
static OodleLZ_Compress_t v_OodleCompress = nullptr;

// The client's RecvTable list, as RecvTable_RecvClassInfos (0x251450) walks it:
// 16-byte elements (RecvTable* at +0, next index u16 at +10), head index u16,
// 0xFFFF ends; the RecvTable's name pointer at +0x4C8; names compared ignoring case.
static const char* const s_pszPatternRecvTableWalk =
	"0F B7 05 ?? ?? ?? ?? 66 41 3B C4 74 ?? 48 8B 0D ?? ?? ?? ?? 0F B7 D8 48 8B D6 48 03 DB 48 8B 0C D9 48 8B 89 C8 04 00 00 E8";
static const uint16_t* s_pRecvTableHead = nullptr;
static uint8_t* const* s_ppRecvTableList = nullptr;
static const int RT_NAME = 0x4C8;
// RecvTable (0x500 bytes): RecvProp** +8, count +0x10, child DTs +0x18 (count
// +0x4B8), decoder +0x4C0, name +0x4C8, initialized +0x4D0, in main list +0x4D1.
static const int RT_SIZE = 0x500;
static const int RT_INITIALIZED = 0x4D0;
static const int RT_IN_MAIN_LIST = 0x4D1;
// That list's CUtlLinkedList::InsertBefore (0x2639D0; five template copies share
// the prologue, this one loads the list pointer at +0x3C).
static const char* const s_pszPatternListInsert =
	"48 89 5C 24 08 57 48 83 EC 20 49 8B F8 0F B7 DA E8 ?? ?? ?? ?? 44 0F B7 C8 41 BB FF FF 00 00 66 45 3B CB 75 0F";
static const int s_nListInsertListLoad = 0x3C;
typedef uint16_t (*RecvTableListInsert_t)(void* pList, uint16_t nBefore, void* const* ppElement);
static RecvTableListInsert_t v_RecvTableListInsert = nullptr;

// CL_ParseClassInfo_EndClasses (0x2FDB60, this = handler): server classes at
// +0x460 (32 bytes: ClientClass* +0, name +8, datatable +16), count +0x468; each
// is matched by name (ignoring case) against the client's ClientClass list
// (client interface 0x28019C8 vtable +0x60, else g_pClientClassHead 0x2801718;
// ClientClass: name +0x10, RecvTable +0x18, next +0x20, class id +0x28) and a
// missing one is a fatal Error ("Client missing DT class %s").
static const char* const s_pszPatternEndClasses =
	"48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 30 48 63 81 68 04 00 00 45 33 C0 4C 8B E9 85 C0 0F 8E";
static const int s_nEndClassesInterface = 0x5A; // mov r15, [client interface]
static const int s_nEndClassesHead = 0x98;      // mov rbx, [g_pClientClassHead]
typedef char (*EndClasses_t)(uint8_t* pThis);
static EndClasses_t v_EndClasses = nullptr;
static void* const* s_ppClientInterface = nullptr;
static uint8_t** s_ppClientClassHead = nullptr;

// CompareRecvPropToSendProp tree builder (0x256440, called per decoder by
// CreateDecoders 0x257E30): every SendProp (136 bytes: type +0, array element
// +24, element count +40, name +64, flags +88, child SendTable +112; props at
// SendTable+0, count +8, name +1208) needs a RecvProp of the same name, type,
// inside-array flag and element count (RecvProp: type +0, child RecvTable
// +0x20, name +0x28, inside-array +0x30, array element +0x38, count +0x5C;
// RecvProp** at RecvTable+8, count +16), else the decoder fails.
static const char* const s_pszPatternMatchProps =
	"48 89 5C 24 08 4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 33 D2 4D 8B F1 49 8B C0 48 8B F1 44 8B E2 41 39 50 08 0F 8E";
typedef char (*MatchProps_t)(void* pMap, const char* pszName, uint8_t* pSendTable, uint8_t* pRecvTable, int nUnused);
static MatchProps_t v_MatchProps = nullptr;
// Its mismatch exit (+0xA4: xor al, al; jmp end) and next-prop block (+0x217):
// the exit is pointed at the next prop (the S21 bridge's decoder builder patch).
static const int s_nMatchPropsFail = 0xA4;
static const int s_nMatchPropsNext = 0x217;
static const uint8_t s_matchPropsFailBytes[] = { 0x32, 0xC0, 0xE9, 0x96, 0x01, 0x00, 0x00 };
static bool s_bMatcherLenient = false;
// Per-decoder finisher CreateDecoders calls once the props are assigned
// (0x255F30). CRecvDecoder: root RecvTable +0, flat SendProp* +0x18, RecvProp*
// +0x4080 (count +0x4098), owner RecvTable* +0x40A0; an unmatched prop is left
// NULL (and its owner unset), which every decode path dereferences.
static const char* const s_pszPatternDecoderDone =
	"48 8B C4 55 48 81 EC 50 01 00 00 80 3D ?? ?? ?? ?? 00 48 8B E9 0F 84 ?? ?? ?? ?? 48 89 58 08 48 8D 0D ?? ?? ?? ?? 48 89 70 10 48 8D 15";
typedef void (*DecoderDone_t)(uint8_t* pDecoder);
static DecoderDone_t v_DecoderDone = nullptr;

// Client PrecacheWeapon(name) (0x8B63C0): a temporary weaponx entity gets the
// name's index in the server's WeaponNames table (0x3FD1A38, FindStringIndex =
// vtable +0x50; 0xFFFF only raises a script error), is precached and removed.
static const char* const s_pszPatternPrecacheWeapon =
	"48 89 74 24 10 57 48 83 EC 20 FF 05 ?? ?? ?? ?? 48 8B F1 B1 08 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 4C 8B 42 18 48 8D 15";
static const int s_nPrecacheWeaponTable = 0x73; // mov rcx, [WeaponNames]
typedef char (*PrecacheWeapon_t)(const char* pszName);
static PrecacheWeapon_t v_PrecacheWeapon = nullptr;
static void* const* s_ppWeaponNames = nullptr;

// Client PrecacheImpactEffectTable(name) (0x8A9310): AddString into the server's
// ImpactEffectTableNames (0x3FD1A40; the dedi creates it with 256 entries), then
// loads the table; a full table is a fatal Error. -1 is the engine's own
// "no table" result (the script native 0xC63790 for an empty name).
static const char* const s_pszPatternPrecacheImpact =
	"48 89 5C 24 08 57 48 83 EC 30 48 8B D9 B1 11 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 41 B9 FF FF FF FF 4C 8B C3 48 C7 44 24 20 00 00 00 00 33 D2 48 8B 01 FF 50 48";
static const int s_nPrecacheImpactTable = 0x14; // mov rcx, [ImpactEffectTableNames]
typedef int (*PrecacheImpact_t)(const char* pszName);
static PrecacheImpact_t v_PrecacheImpact = nullptr;
static void* const* s_ppImpactNames = nullptr;

static CNetChan_ProcessPacket_t v_ProcessPacket = nullptr;
static bf_read_Seek_t v_bfSeek = nullptr;
static CClientState_SetSignonState_t v_SetSignonState = nullptr;
static const double* s_pNetTime = nullptr;

// S3 dedi -> S30 message types (docs/11); -1: no S30 counterpart.
static int Netchan_S30Type(int nS3)
{
	switch (nS3)
	{
	case 0: case 1: case 2: case 3: return nS3;
	case 4: return 5;   case 5: return 6;   case 6: return 7;   case 7: return 12;
	case 8: return 13;  case 9: return 14;  case 11: return 15; case 12: return 16;
	case 13: return 17; case 14: return 18; case 15: return 19; case 16: return 20;
	case 17: return 21; case 18: return 22; case 19: return 23; case 20: return 24;
	case 22: return 25; case 23: return 26; case 24: return 27; case 25: return 28;
	case 26: return 29; case 27: return 30; case 29: return 32; case 30: return 33;
	case 31: return 34; case 33: return 36; case 40: return 53; case 41: return 51;
	default: return -1;
	}
}

//-----------------------------------------------------------------------------
// State
//-----------------------------------------------------------------------------
static const int kSubchanFragment = 560;     // S3_MAX_FRAGMENT_PER_PACKET
static const int kReliableMax = 0x40000;
static const ULONGLONG kSignonEchoIntervalMs = 250;
static const ULONGLONG kSignonEchoWindowMs = 5000;  // bridge_signon_echo_ms
static const ULONGLONG kUserInfoIntervalMs = 250;   // kBridgeUserInfoIntervalMs
static const int kUserInfoCopies = 3;               // kBridgeUserInfoCopies
static const ULONGLONG kEngineFlushMs = 10;
static const ULONGLONG kKeepaliveMs = 250;          // bridge_c2s_keepalive_ms

static volatile LONG s_bActive = 0;
static S30NetchanSend_t s_pfnSend = nullptr;

// S2C, engine thread
struct Reliable_t
{
	bool bActive;
	bool bCompressed;
	int nTotal;
	int nReceived;
	int nUncompressed;
	uint32_t nEntrySeq;
	uint8_t* pBuf;
	int nCapacity;
};
static Reliable_t s_rel = {};
static bool s_bSubSeqRebased = false;
static bool s_bSawSendTables = false;
static bool s_bPdefOk = false;
static int s_nSignonSeqKey = -1;
static int s_nSignonSeqMax = -1;
static bool s_bDeferredSignon3 = false;
static uint8_t s_deferredSignon3[1024];
static int s_nDeferredSignon3Bytes = 0;
static int s_nDeferredSignon3Spawn = -1;

// Shared with the C2S builder (bridge poll thread and engine thread).
static SRWLOCK s_lock = SRWLOCK_INIT;
static uint32_t s_nInSeq = 0;                // last S2C sequence
static uint32_t s_nC2SSeq = 0;
static uint32_t s_nServerNonce = 0;
static bool s_bNeedNonceAck = false;
static uint32_t s_nServerSubSeqRecv = 0;
static int s_nSignonSeqSpawn = -1;
static int s_nLastSentSignon = -1;
struct PendingSignon_t { int nState, nSpawn; };
static PendingSignon_t s_signonQueue[16];
static int s_nSignonQHead = 0;
static int s_nSignonQCount = 0;
static int s_nEchoState = -1;
static int s_nEchoSpawn = 0;
static ULONGLONG s_nEchoUntilMs = 0;
static ULONGLONG s_nEchoNextMs = 0;
// Tick of the last snapshot the engine processed: acked in every C2S packet with
// S3 clc_ClientTick (the dedi's delta base; it holds a FULL until that tick is acked).
static int s_nAckTick = -1;
// When the engine last processed a snapshot: while they flow, the dedi's svc_ServerTick
// (same tick, same ServerTickInfo) is not passed on (see case 22).
static volatile ULONGLONG s_nLastSnapshotMs = 0;
static const ULONGLONG kServerTickSnapshotWindowMs = 500;
// The engine's last clc_Move, transcoded to S3 (bridge_usercmd), written into the next C2S packet.
static uint8_t s_pendingMove[2048];
static int s_nPendingMoveBits = 0;
// Relayed client messages (bridge_c2s): sent until nSendsLeft runs out.
struct C2SSlot_t
{
	uint8_t data[1100];
	int nBits;
	int nSendsLeft;
	ULONGLONG nSpacingMs;
	ULONGLONG nNextDueMs;
};
static const int kC2SRing = 32;
static C2SSlot_t s_c2sRing[kC2SRing];
static float s_flTickInterval = 0.0f;

// Queued for the engine socket when a DataBlock completes; the ProcessPacket
// detour dispatches the DataBlock for it. Its first word is no S3 header the
// engine treats specially (-1 connectionless, -2 split, -3 compressed).
static const uint8_t s_wakeMarker[16] = { 'R', '5', 'F', 'S', '3', '0', 'W', 'A', 'K', 'E', 'D', 'B', 0, 0, 0, 0 };
static bool s_bPendingUserInfo = false;
static int s_nUserInfoSendsLeft = 0;
static ULONGLONG s_nUserInfoNextMs = 0;
static ULONGLONG s_nLastFlushMs = 0;

// counters
static volatile LONG s_nPackets = 0;
static volatile LONG s_nStale = 0;
static volatile LONG s_nBadNonce = 0;
static volatile LONG s_nFlushes = 0;
static volatile LONG s_nDispatched = 0;
static volatile LONG s_nSkipped = 0;

//-----------------------------------------------------------------------------
// Engine message dispatch
//-----------------------------------------------------------------------------
static void* Netchan_Msg(uint8_t* pChan, int nType)
{
	void* const* const pArray = *reinterpret_cast<void* const* const*>(pChan + NC_MESSAGES);
	const int nCount = *reinterpret_cast<const int*>(pChan + NC_MESSAGE_COUNT);
	if (!pArray || nType < 0 || nType >= nCount)
		return nullptr;
	return pArray[nType];
}

// The fields NET_GetPacket (0x2A5CB0) sets on the packet's bf_read, then Seek(0).
static void Netchan_BfInit(uint8_t* pBf, const uint8_t* pData, int nBytes)
{
	memset(pBf, 0, S30BfRead::SIZE);
	*reinterpret_cast<const char**>(pBf) = "bridge.s3";
	*reinterpret_cast<int64_t*>(pBf + S30BfRead::OFF_DATABITS) = 8LL * nBytes;
	*reinterpret_cast<int64_t*>(pBf + S30BfRead::OFF_DATABYTES) = nBytes;
	*reinterpret_cast<const uint8_t**>(pBf + S30BfRead::OFF_PDATAIN) = pData;
	*reinterpret_cast<const uint8_t**>(pBf + S30BfRead::OFF_PBUFEND) = pData + nBytes;
	*reinterpret_cast<const uint8_t**>(pBf + S30BfRead::OFF_PDATA) = pData;
	v_bfSeek(pBf, 0);
}

void S30Netchan_BfInit(uint8_t* pBf, const uint8_t* pData, int nBytes)
{
	Netchan_BfInit(pBf, pData, nBytes);
}

// Records a caught fault (code, module-relative address, data address) for the log.
static int Netchan_FaultFilter(EXCEPTION_POINTERS* pInfo, const char* pszWhere)
{
	const EXCEPTION_RECORD* const pRec = pInfo ? pInfo->ExceptionRecord : nullptr;
	char szAddress[128] = "?";
	if (pRec)
		S30Log_FormatAddress(pRec->ExceptionAddress, szAddress, sizeof(szAddress));
	S30Log("[NETCHAN] fault in %s: code 0x%08lX at %s, data 0x%llX\n", pszWhere, pRec ? pRec->ExceptionCode : 0, szAddress,
		pRec && pRec->NumberParameters >= 2 ? static_cast<unsigned long long>(pRec->ExceptionInformation[1]) : 0ULL);
	// The filter runs above the faulting frames, so they are in this trace.
	void* pFrames[32];
	const USHORT nFrames = RtlCaptureStackBackTrace(0, 32, pFrames, nullptr);
	char szTrace[1024];
	int nLen = 0;
	szTrace[0] = '\0';
	for (USHORT i = 0; i < nFrames && nLen < 960; ++i)
	{
		char szFrame[96];
		S30Log_FormatAddress(pFrames[i], szFrame, sizeof(szFrame));
		if (!strncmp(szFrame, "r5apex", 6) || !strncmp(szFrame, "client_s30", 10))
			nLen += snprintf(szTrace + nLen, sizeof(szTrace) - nLen, " %s", szFrame);
	}
	S30Log("[NETCHAN]   stack:%s\n", szTrace);
	return EXCEPTION_EXECUTE_HANDLER;
}

static bool Netchan_CallRead(void* pMsg, uint8_t* pBf, bool* pbFault)
{
	__try
	{
		return reinterpret_cast<char (*)(void*, void*)>((*reinterpret_cast<void***>(pMsg))[4])(pMsg, pBf) != 0;
	}
	__except (Netchan_FaultFilter(GetExceptionInformation(), "ReadFromBuffer"))
	{
		*pbFault = true;
		return false;
	}
}

static bool Netchan_CallProcess(void* pMsg, bool* pbFault)
{
	__try
	{
		return reinterpret_cast<char (*)(void*)>((*reinterpret_cast<void***>(pMsg))[3])(pMsg) != 0;
	}
	__except (Netchan_FaultFilter(GetExceptionInformation(), "Process"))
	{
		*pbFault = true;
		return false;
	}
}

static bool Netchan_HasRecvTable(const char* pszName)
{
	if (!s_pRecvTableHead || !s_ppRecvTableList || !*s_ppRecvTableList)
		return true; // unresolved: leave the table to the engine
	const uint8_t* const pList = *s_ppRecvTableList;
	int nGuard = 0;
	for (uint16_t i = *s_pRecvTableHead; i != 0xFFFF && nGuard < 0x10000; ++nGuard)
	{
		const uint8_t* const pTable = *reinterpret_cast<uint8_t* const*>(pList + 16 * i);
		const char* const pszTable = pTable ? *reinterpret_cast<const char* const*>(pTable + RT_NAME) : nullptr;
		if (pszTable && !_stricmp(pszTable, pszName))
			return true;
		i = *reinterpret_cast<const uint16_t*>(pList + 16 * i + 10);
	}
	return false;
}

// Stub RecvTables for the dedi's SendTables S30 lacks (the S21 bridge's stub
// RecvTable): no props, so every SendProp ends on a dummy RecvProp. Registered
// in the client's list before the SendTable arrives, so the engine builds a
// decoder for it; kept for the process (the list keeps them across connects).
struct StubTable_t
{
	const char* pszName;
	uint8_t* pTable;
	bool bRegistered;
};
static StubTable_t s_stubTables[256];
static int s_nStubTables = 0;

static StubTable_t* Netchan_StubTable(const char* pszName)
{
	for (int i = 0; i < s_nStubTables; ++i)
	{
		if (!_stricmp(s_stubTables[i].pszName, pszName))
			return &s_stubTables[i];
	}
	if (s_nStubTables >= static_cast<int>(sizeof(s_stubTables) / sizeof(s_stubTables[0])))
		return nullptr;
	uint8_t* const pTable = static_cast<uint8_t*>(calloc(RT_SIZE, 1));
	if (!pTable)
		return nullptr;
	StubTable_t& s = s_stubTables[s_nStubTables++];
	s.pszName = _strdup(pszName);
	s.pTable = pTable;
	s.bRegistered = false;
	*reinterpret_cast<const char**>(pTable + RT_NAME) = s.pszName;
	pTable[RT_INITIALIZED] = 1;
	return &s;
}

static bool Netchan_RegisterStubTable(StubTable_t& s)
{
	if (s.bRegistered)
		return true;
	if (!v_RecvTableListInsert || !s_ppRecvTableList)
		return false;
	void* const pElement = s.pTable;
	const uint16_t nIndex = v_RecvTableListInsert(const_cast<uint8_t**>(s_ppRecvTableList), 0xFFFF, &pElement);
	if (nIndex == 0xFFFF)
		return false;
	s.pTable[RT_IN_MAIN_LIST] = 1;
	s.bRegistered = Netchan_HasRecvTable(s.pszName);
	return s.bRegistered;
}

// Stub classes for the dedi's server classes S30 lacks (the S21 bridge's stub
// ClientClass): the network name and the stub RecvTable of the server's
// datatable (the registered one when its SendTable came, so it has a decoder).
struct StubClass_t
{
	uint8_t* pClass;
	const char* pszName;
	const char* pszTable;
};
static StubClass_t s_stubClasses[512];
static int s_nStubClasses = 0;

static uint8_t* Netchan_StubClass(const char* pszName, const char* pszTable)
{
	for (int i = 0; i < s_nStubClasses; ++i)
	{
		if (!_stricmp(s_stubClasses[i].pszName, pszName) && !_stricmp(s_stubClasses[i].pszTable, pszTable))
			return s_stubClasses[i].pClass;
	}
	if (s_nStubClasses >= static_cast<int>(sizeof(s_stubClasses) / sizeof(s_stubClasses[0])))
		return nullptr;
	StubTable_t* const pStubTable = Netchan_StubTable(pszTable);
	uint8_t* const pClass = pStubTable ? static_cast<uint8_t*>(calloc(64, 1)) : nullptr;
	if (!pClass)
		return nullptr;
	StubClass_t& s = s_stubClasses[s_nStubClasses++];
	s.pClass = pClass;
	s.pszName = _strdup(pszName);
	s.pszTable = pStubTable->pszName;
	*reinterpret_cast<const char**>(pClass + 0x10) = s.pszName;
	*reinterpret_cast<uint8_t**>(pClass + 0x18) = pStubTable->pTable;
	return pClass;
}

// Dummy RecvProps for unmatched SendProps (the S21 bridge's '?_unmatched'): the
// SendProp's own type, so its bits are read with its encoding, and a no-op proxy,
// so nothing is written (S30's type decoders, e.g. Int 0x252A30, store through
// the proxy when there is one). Arrays get an element dummy; their decoder
// (0x253F70) steps by the RecvProp's stride (+0x58, 0 here).
static const int RP_TYPE = 0x00;
static const int RP_NAME = 0x28;
static const int RP_ARRAY_ELEMENT = 0x38;
static const int RP_PROXY = 0x48;
static const int RP_SIZE = 0x80;

static void Netchan_DummyProxy(void*, void*, void*)
{
}

static uint8_t s_dummyProps[16][RP_SIZE];
static uint8_t s_dummyArrays[16][RP_SIZE];
static bool s_bDummyProp[16];
static bool s_bDummyArray[16];

static uint8_t* Netchan_DummyProp(int nType, int nElementType)
{
	static const char s_szName[] = "?_unmatched";
	if (nType < 0 || nType >= 16 || (nType == 5 && (nElementType < 0 || nElementType >= 16 || nElementType == 5)))
		return nullptr;
	if (nType == 5)
	{
		uint8_t* const p = s_dummyArrays[nElementType];
		if (!s_bDummyArray[nElementType])
		{
			*reinterpret_cast<int*>(p + RP_TYPE) = 5;
			*reinterpret_cast<const char**>(p + RP_NAME) = s_szName;
			*reinterpret_cast<uint8_t**>(p + RP_ARRAY_ELEMENT) = Netchan_DummyProp(nElementType, -1);
			*reinterpret_cast<void**>(p + RP_PROXY) = reinterpret_cast<void*>(&Netchan_DummyProxy);
			s_bDummyArray[nElementType] = true;
		}
		return p;
	}
	uint8_t* const p = s_dummyProps[nType];
	if (!s_bDummyProp[nType])
	{
		*reinterpret_cast<int*>(p + RP_TYPE) = nType;
		*reinterpret_cast<const char**>(p + RP_NAME) = s_szName;
		*reinterpret_cast<uint8_t**>(p + RP_ARRAY_ELEMENT) = p;
		*reinterpret_cast<void**>(p + RP_PROXY) = reinterpret_cast<void*>(&Netchan_DummyProxy);
		s_bDummyProp[nType] = true;
	}
	return p;
}

static int s_nDummiedProps = 0;

static int Netchan_FillDecoder(uint8_t* pDecoder)
{
	uint8_t** const ppProps = *reinterpret_cast<uint8_t***>(pDecoder + 0x4080);
	uint8_t** const ppOwners = *reinterpret_cast<uint8_t***>(pDecoder + 0x40A0);
	uint8_t* const* const ppSend = *reinterpret_cast<uint8_t* const* const*>(pDecoder + 0x18);
	uint8_t* const pRoot = *reinterpret_cast<uint8_t**>(pDecoder);
	const int nProps = *reinterpret_cast<const int*>(pDecoder + 0x4098);
	if (!ppProps || !ppOwners || !ppSend || !pRoot)
		return nProps > 0 ? -1 : 0;
	int nFilled = 0;
	for (int i = 0; i < nProps; ++i)
	{
		if (ppProps[i])
			continue;
		const uint8_t* const pSend = ppSend[i];
		const int nType = pSend ? *reinterpret_cast<const int*>(pSend) : -1;
		// SendProps S30 re-typed: an adapter writes S30's field (bridge_props, docs/20).
		uint8_t* const pAdapter = S30Props_Adapter(*reinterpret_cast<const char* const*>(pRoot + RT_NAME), pSend);
		if (pAdapter)
		{
			ppProps[i] = pAdapter;
			ppOwners[i] = pRoot;
			S30LogFile("[NETCHAN] decoder '%s': %s adapted to S30's field\n", *reinterpret_cast<const char* const*>(pRoot + RT_NAME),
				*reinterpret_cast<const char* const*>(pSend + 64));
			continue;
		}
		const uint8_t* const pElement = nType == 5 ? *reinterpret_cast<const uint8_t* const*>(pSend + 24) : nullptr;
		uint8_t* const pDummy = Netchan_DummyProp(nType, pElement ? *reinterpret_cast<const int*>(pElement) : -1);
		if (!pDummy)
		{
			S30Log("[NETCHAN] decoder '%s' prop %d: no dummy for send type %d\n",
				*reinterpret_cast<const char* const*>(pRoot + RT_NAME), i, nType);
			continue;
		}
		ppProps[i] = pDummy;
		ppOwners[i] = pRoot;
		++nFilled;
	}
	return nFilled;
}

static void h_DecoderDone(uint8_t* pDecoder)
{
	if (s_bActive && s_bMatcherLenient)
	{
		int nFilled = 0;
		__try
		{
			nFilled = Netchan_FillDecoder(pDecoder);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			nFilled = -1;
		}
		if (nFilled)
		{
			s_nDummiedProps += nFilled > 0 ? nFilled : 0;
			const uint8_t* const pRoot = *reinterpret_cast<uint8_t* const*>(pDecoder);
			S30LogFile("[NETCHAN] decoder '%s': %d unmatched prop(s) on dummies%s\n",
				pRoot ? *reinterpret_cast<const char* const*>(pRoot + RT_NAME) : "?", nFilled, nFilled < 0 ? " (FAULT)" : "");
		}
	}
	v_DecoderDone(pDecoder);
}

static uint8_t* Netchan_ClientClasses(void)
{
	void* const pInterface = s_ppClientInterface ? *s_ppClientInterface : nullptr;
	if (!pInterface)
		return s_ppClientClassHead ? *s_ppClientClassHead : nullptr;
	__try
	{
		return reinterpret_cast<uint8_t* (*)(void*)>((*reinterpret_cast<void***>(pInterface))[12])(pInterface);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return nullptr;
	}
}

static char Netchan_EndClassesWithStubs(uint8_t* pThis, uint8_t* pFirst, uint8_t* pOldHead)
{
	*s_ppClientClassHead = pFirst;
	char bResult = 0;
	__try
	{
		bResult = v_EndClasses(pThis);
	}
	__finally
	{
		// Also on Host_EndGame's longjmp: the stubs never stay in the client's list.
		*s_ppClientClassHead = pOldHead;
	}
	return bResult;
}

// S30 makes a missing client class fatal; the dedi's classes S30 dropped get a
// stub for the engine's own matching, then leave the list again (the server
// class entries keep pointing at them, as after the S21 bridge's patch).
static char h_EndClasses(uint8_t* pThis)
{
	if (!s_bActive || !s_ppClientClassHead)
		return v_EndClasses(pThis);
	const int nClasses = *reinterpret_cast<const int*>(pThis + 0x468);
	uint8_t* const pServer = *reinterpret_cast<uint8_t**>(pThis + 0x460);
	uint8_t* const pOldHead = *s_ppClientClassHead;
	uint8_t* const pList = Netchan_ClientClasses();
	if (!pServer || nClasses <= 0 || pList != pOldHead)
	{
		if (pList != pOldHead)
			S30Log("[NETCHAN] client class list %p is not g_pClientClassHead %p -- no stub classes\n", pList, pOldHead);
		return v_EndClasses(pThis);
	}
	uint8_t* pFirst = pOldHead;
	int nStubs = 0;
	char szStubs[1024];
	int nStubsLen = 0;
	szStubs[0] = '\0';
	// A stub needs a create function: CopyNewEntity calls ClientClass +0 (entnum,
	// serial) for every entity of the class (0x2EF0B4). Stubs borrow info_target's:
	// an invisible point entity; its props come through the stub's own decoder, all
	// no-op dummies, so it keeps its defaults. +8 (create a temp entity) stays null:
	// the event queue (0x2F6550) skips an event whose class has none, so a dedi temp
	// entity S30 does not know is dropped instead of running info_target's.
	const uint8_t* pDonor = nullptr;
	for (const uint8_t* p = pOldHead; p && !pDonor; p = *reinterpret_cast<uint8_t* const*>(p + 0x20))
	{
		if (!_stricmp(*reinterpret_cast<const char* const*>(p + 0x10), "CInfoTarget"))
			pDonor = p;
	}
	if (!pDonor)
		S30Log("[NETCHAN] no CInfoTarget ClientClass -- stub classes have no create function\n");
	for (int i = 0; i < nClasses; ++i)
	{
		const char* const pszName = *reinterpret_cast<const char* const*>(pServer + 32 * i + 8);
		const char* const pszTable = *reinterpret_cast<const char* const*>(pServer + 32 * i + 16);
		if (!pszName || !pszTable)
			continue; // the engine reports it ("class %d not initialized")
		bool bFound = false;
		for (const uint8_t* p = pOldHead; p && !bFound; p = *reinterpret_cast<uint8_t* const*>(p + 0x20))
			bFound = !_stricmp(*reinterpret_cast<const char* const*>(p + 0x10), pszName);
		if (bFound)
			continue;
		uint8_t* const pStub = Netchan_StubClass(pszName, pszTable);
		if (!pStub)
		{
			S30Log("[NETCHAN] no stub class for '%s' (%s)\n", pszName, pszTable);
			continue;
		}
		if (pDonor)
			memcpy(pStub, pDonor, 0x8); // create
		*reinterpret_cast<uint8_t**>(pStub + 0x20) = pFirst;
		pFirst = pStub;
		++nStubs;
		if (nStubsLen < 900)
			nStubsLen += snprintf(szStubs + nStubsLen, sizeof(szStubs) - nStubsLen, " %s", pszName);
	}
	if (!nStubs)
		return v_EndClasses(pThis);
	S30Log("[NETCHAN] %d of %d server classes have no S30 ClientClass, stubbed:%s\n", nStubs, nClasses, szStubs);
	const char bResult = Netchan_EndClassesWithStubs(pThis, pFirst, pOldHead);
	S30Log("[NETCHAN] EndClasses with stubs: %d\n", bResult);
	return bResult;
}

// Logs why a SendTable does not match its RecvTable, with the matcher's own rules.
static int s_nPropMismatches = 0;

static void Netchan_LogMismatch(const char* pszPath, const char* pszProp, const char* pszWhy, int nSendType, int nRecvType)
{
	++s_nPropMismatches;
	S30LogFile("[PROPS] %s.%s: %s (send type %d, recv type %d)\n", pszPath, pszProp, pszWhy, nSendType, nRecvType);
}

static void Netchan_DiagProps(const char* pszPath, const uint8_t* pSend, const uint8_t* pRecv, int nDepth)
{
	if (!pSend || nDepth > 16)
		return;
	const uint8_t* const pSendProps = *reinterpret_cast<const uint8_t* const*>(pSend);
	const int nSend = *reinterpret_cast<const int*>(pSend + 8);
	for (int i = 0; i < nSend; ++i)
	{
		const uint8_t* const sp = pSendProps + 136 * i;
		if (*reinterpret_cast<const uint8_t*>(sp + 88) & 0xC0)
			continue;
		const char* const pszName = *reinterpret_cast<const char* const*>(sp + 64);
		if (!pRecv)
		{
			Netchan_LogMismatch(pszPath, pszName, "no RecvTable", *reinterpret_cast<const int*>(sp), -1);
			continue;
		}
		const uint8_t* rp = nullptr;
		const uint8_t* const* const ppRecv = *reinterpret_cast<const uint8_t* const* const*>(pRecv + 8);
		const int nRecv = *reinterpret_cast<const int*>(pRecv + 16);
		for (int j = 0; j < nRecv && !rp; ++j)
		{
			const char* const pszRecv = *reinterpret_cast<const char* const*>(ppRecv[j] + 0x28);
			if (pszRecv && !_stricmp(pszRecv, pszName))
				rp = ppRecv[j];
		}
		if (!rp)
		{
			Netchan_LogMismatch(pszPath, pszName, "no RecvProp", *reinterpret_cast<const int*>(sp), -1);
			continue;
		}
		const uint8_t* s = sp;
		const uint8_t* r = rp;
		while (s && r)
		{
			const int nSendType = *reinterpret_cast<const int*>(s), nRecvType = *reinterpret_cast<const int*>(r);
			const bool bSendInside = (*reinterpret_cast<const uint32_t*>(s + 88) & 0x80) != 0;
			if (nSendType != nRecvType || (r[0x30] != 0) != bSendInside)
			{
				Netchan_LogMismatch(pszPath, pszName, nSendType != nRecvType ? "type" : "inside-array flag", nSendType, nRecvType);
				break;
			}
			if (nSendType != 5)
			{
				if (nSendType == 10)
				{
					char szPath[512];
					snprintf(szPath, sizeof(szPath), "%s.%s", pszPath, pszName);
					Netchan_DiagProps(szPath, *reinterpret_cast<const uint8_t* const*>(s + 112), *reinterpret_cast<const uint8_t* const*>(r + 0x20), nDepth + 1);
				}
				break;
			}
			if (*reinterpret_cast<const int*>(r + 0x5C) != *reinterpret_cast<const int*>(s + 40))
			{
				char szWhy[64];
				snprintf(szWhy, sizeof(szWhy), "array count %d vs %d", *reinterpret_cast<const int*>(s + 40), *reinterpret_cast<const int*>(r + 0x5C));
				Netchan_LogMismatch(pszPath, pszName, szWhy, nSendType, nRecvType);
				break;
			}
			r = *reinterpret_cast<const uint8_t* const*>(r + 0x38);
			s = *reinterpret_cast<const uint8_t* const*>(s + 24);
		}
	}
}

static void Netchan_DiagPropsSafe(const char* pszName, const uint8_t* pSend, const uint8_t* pRecv)
{
	__try
	{
		Netchan_DiagProps(pszName, pSend, pRecv, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		S30Log("[NETCHAN] fault walking the props of '%s'\n", pszName);
	}
}

static thread_local int s_nMatchDepth = 0;

static char h_MatchProps(void* pMap, const char* pszName, uint8_t* pSendTable, uint8_t* pRecvTable, int nUnused)
{
	++s_nMatchDepth;
	const char bResult = v_MatchProps(pMap, pszName, pSendTable, pRecvTable, nUnused);
	--s_nMatchDepth;
	// Lenient (patched) builds always succeed, so the walk runs for every decoder.
	if ((!bResult || s_bMatcherLenient) && !s_nMatchDepth && s_bActive && s_nPropMismatches < 5000)
	{
		const int nBefore = s_nPropMismatches;
		Netchan_DiagPropsSafe(pszName ? pszName : "?", pSendTable, pRecvTable);
		if (s_nPropMismatches != nBefore)
			S30LogFile("[NETCHAN] decoder '%s' does not match: %d prop(s)\n", pszName ? pszName : "?", s_nPropMismatches - nBefore);
	}
	return bResult;
}

// FindStringIndex (INetworkStringTable vtable +0x50) on a string table global.
static int Netchan_TableIndex(void* const* ppTable, const char* pszName)
{
	void* const pTable = ppTable ? *ppTable : nullptr;
	if (!pTable || !pszName)
		return -2;
	__try
	{
		return reinterpret_cast<int (*)(void*, const char*)>((*reinterpret_cast<void***>(pTable))[10])(pTable, pszName);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return -3;
	}
}

// A weapon the dedi never precached (S30-only, e.g. mp_ability_boost_kit_phase_
// rewind_offhand) gets no settings, and removing the temporary entity reads them
// (0x75B20B). The dedi cannot spawn it either, so its precache is skipped.
static char h_PrecacheWeapon(const char* pszName)
{
	if (s_bActive)
	{
		const int nIndex = Netchan_TableIndex(s_ppWeaponNames, pszName);
		if (nIndex == 0xFFFF)
		{
			static int s_nWeaponsSkipped = 0;
			if (++s_nWeaponsSkipped <= 200)
				S30Log("[NETCHAN] client PrecacheWeapon '%s' skipped: not in the server's WeaponNames\n", pszName ? pszName : "(null)");
			return 1;
		}
	}
	return v_PrecacheWeapon(pszName);
}

// The same for impact tables the dedi never precached (S30 heirlooms, abilities).
static int h_PrecacheImpact(const char* pszName)
{
	if (s_bActive && pszName && *pszName)
	{
		const int nIndex = Netchan_TableIndex(s_ppImpactNames, pszName);
		if (nIndex == 0xFFFF)
		{
			static int s_nImpactSkipped = 0;
			if (++s_nImpactSkipped <= 200)
				S30Log("[NETCHAN] client PrecacheImpactEffectTable '%s' skipped: not in the server's ImpactEffectTableNames\n", pszName);
			return -1;
		}
	}
	return v_PrecacheImpact(pszName);
}

// Kraken (8), SuperFast (1), as the S21 bridge.
static int64_t Netchan_CallOodle(const uint8_t* pRaw, uint32_t nRaw, uint8_t* pOut)
{
	__try
	{
		return v_OodleCompress(8, pRaw, nRaw, pOut, 1, nullptr, nullptr, nullptr, nullptr, 0);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		S30Log("[NETCHAN] fault in OodleLZ_Compress\n");
		return 0;
	}
}

static void Netchan_CallHandler(uint8_t* pChan, int nSlot)
{
	void* const pHandler = *reinterpret_cast<void**>(pChan + NC_HANDLER);
	if (!pHandler)
		return;
	void** const pVtable = *reinterpret_cast<void***>(pHandler);
	__try
	{
		if (nSlot == 5)
			reinterpret_cast<void (*)(void*, int, int)>(pVtable[5])(pHandler,
				*reinterpret_cast<int*>(pChan + NC_IN_SEQ), *reinterpret_cast<int*>(pChan + NC_OUT_SEQ_ACK));
		else
			reinterpret_cast<void (*)(void*)>(pVtable[6])(pHandler);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		S30Log("[NETCHAN] fault in netchannel handler slot %d\n", nSlot);
	}
}

// ReadFromBuffer + Process of S30 message nType30 over a bf_read of pData.
// nStartBit: where its body starts; *pnEndBit: where ReadFromBuffer stopped.
static bool Netchan_Run(uint8_t* pChan, int nType30, const uint8_t* pData, int nBytes, int nStartBit, int* pnEndBit,
	const char* pszWhat, int nS3)
{
	void* const pMsg = Netchan_Msg(pChan, nType30);
	if (!pMsg)
	{
		S30Log("[NETCHAN] S3 %d (%s): S30 message %d not registered\n", nS3, pszWhat, nType30);
		return false;
	}
	alignas(16) uint8_t bf[S30BfRead::SIZE];
	Netchan_BfInit(bf, pData, nBytes);
	if (nStartBit)
		v_bfSeek(bf, nStartBit);
	bool bFault = false;
	const bool bRead = Netchan_CallRead(pMsg, bf, &bFault) && !S30BfRead::Overflowed(bf);
	if (pnEndBit)
		*pnEndBit = static_cast<int>(S30BfRead::BitsRead(bf));
	bool bProcess = false;
	if (bRead)
		bProcess = Netchan_CallProcess(pMsg, &bFault);
	InterlockedIncrement(&s_nDispatched);
	static volatile LONG s_nLog = 0;
	const LONG n = InterlockedIncrement(&s_nLog);
	if (n <= 200 || !bRead || bFault || nType30 == 6 || (nType30 >= 26 && nType30 <= 30))
		S30Log("[NETCHAN] S3 %d (%s) -> S30 %d: read %d process %d%s, %d bits\n", nS3, pszWhat, nType30,
			bRead, bProcess, bFault ? " FAULT" : "", (pnEndBit ? *pnEndBit : 0) - nStartBit);
	return bRead && !bFault;
}

//-----------------------------------------------------------------------------
// C2S (S21 bridge S21Bridge_BuildS3Packet / QueueSignon), under s_lock
//-----------------------------------------------------------------------------
static void Netchan_QueueSignonLocked(int nState, int nSpawn)
{
	// The dedi reconnects on a rung above CONNECTED whose spawn count is not its own.
	if (nState > 2 && nSpawn < 0)
	{
		if (s_nSignonSeqSpawn < 0)
		{
			S30Log("[NETCHAN] C2S SignonState(%d) refused: no server spawn count yet\n", nState);
			return;
		}
		nSpawn = s_nSignonSeqSpawn;
	}
	if (s_nSignonQCount >= 16)
	{
		S30Log("[NETCHAN] signon queue full, dropped C2S SignonState(%d)\n", nState);
		return;
	}
	s_signonQueue[(s_nSignonQHead + s_nSignonQCount) & 15] = { nState, nSpawn };
	++s_nSignonQCount;
	if (nState >= 2 && nState <= 8)
	{
		const ULONGLONG nNow = GetTickCount64();
		s_nEchoState = nState;
		s_nEchoSpawn = nSpawn;
		s_nEchoUntilMs = nNow + kSignonEchoWindowMs;
		s_nEchoNextMs = nNow + kSignonEchoIntervalMs;
	}
	S30Log("[NETCHAN] C2S SignonState(%d) spawn %d queued\n", nState, nSpawn);
}

static void Netchan_WriteSignonState(S30BitWriter_t& w, int nState, int nSpawn)
{
	w.Write(5, 7);              // net_SignonState (dedi type 5)
	w.Write(static_cast<uint8_t>(nState), 8);
	w.Write(static_cast<uint32_t>(nSpawn), 32);
	w.WriteString("", 1);       // map name
	w.WriteString("", 1);       // game mode
	w.Write(0, 32);             // int64 low
	w.Write(0, 32);             // int64 high
	w.WriteString("", 1);       // playlist
}

// UserInfo the dedi expects after CONNECTED (S21 bridge s_userInfoKV; the S21
// bridge sends each ConVar's live value, these are its fallbacks).
static const char* const s_userInfoKV[][2] = {
	{ "cl_updaterate_mp", "20" }, { "cl_predict", "1" }, { "cl_predictweapons", "1" }, { "cl_lagcompensation", "1" },
	{ "cl_fovScale", "1.54715" }, { "net_maxroutable", "1200" }, { "net_wifi", "0" }, { "hasMic", "1" },
	{ "persistence_clForceNew", "0" }, { "match_partySize", "" }, { "match_myDatacenter", "east us" },
	{ "match_playlist", "" }, { "match_partyChangeNum", "" }, { "match_searching", "0" }, { "match_myTeam", "0" },
	{ "match_goodReputation", "1" }, { "match_teamNoFill", "0" }, { "match_myBestDatacenter", "east us" },
	{ "match_myRankedDatacenter", "east us" }, { "match_partySub", "" }, { "voice_loopback", "0" },
	{ "weapon_setting_autocycle_on_empty", "0" }, { "laserSightColor", "0000" }, { "laserSightColorCustomized", "0" },
	{ "hasAnyAssetsWithDiscardedStreamableData", "0" }, { "cl_isUnderAge", "0" }, { "hasPartialInstall", "0" },
	{ "inPartyChat", "0" }, { "nucleus_id", "0" }, { "nucleus_pid", "unknown" }, { "pin_sid", "0:1776272811:2" },
	{ "pin_plat_id", "0" }, { "net_minimumPacketLossDC", "0" }, { "community", "" }, { "stryder_security", "" },
	{ "locationInfo", "51" }, { "locationInfo_nucleus", "" }, { "twitch_prime_rewards", "" },
	{ "player_setting_stickysprintforward", "0" }, { "player_setting_holdtosprint", "0" },
	{ "toggle_on_jump_to_deactivate", "1" }, { "mantle_boost_input_setting", "2" },
	// The dedi's mod policy gate disconnects an active client whose sdk_mods does not parse
	// (ModSystem_ParseAttestation): "<ids>#<FNV-1a 64 of ids>", "-" for none. S30 loads no SDK mods.
	{ "sdk_mods", "-#af63a04c86018698" },
};

static int Netchan_BuildPacketLocked(uint8_t* pOut, int nOutSize)
{
	S30BitWriter_t w(pOut, nOutSize);
	w.Write(++s_nC2SSeq, 32);
	w.Write(s_nInSeq, 32);
	w.Write(0, 8);              // flags: no reliable data (the S21 bridge sends none)

	if (s_nServerNonce)
	{
		w.Write(1, 1);
		w.Write(0xFDBAC34Du, 32);
		if (s_bNeedNonceAck)
		{
			w.Write(1, 1);
			w.Write(s_nServerNonce, 32);
			s_bNeedNonceAck = false;
		}
		else
			w.Write(0, 1);
		w.Write(s_nServerSubSeqRecv, 32);
		w.Write(s_nServerSubSeqRecv & 0x3FF, 10);
	}
	else
		w.Write(0, 1);

	// net_SignonState rungs, unreliable (the dedi sends no netchan during signon).
	int nDrained = 0;
	while (nDrained < 8 && s_nSignonQCount > 0 && (nOutSize * 8 - w.m_nPos) >= 256 * 8)
	{
		const PendingSignon_t& p = s_signonQueue[s_nSignonQHead];
		Netchan_WriteSignonState(w, p.nState, p.nSpawn);
		S30Log("[NETCHAN] C2S SignonState(%d) spawn %d sent (seq %u)\n", p.nState, p.nSpawn, s_nC2SSeq);
		s_nSignonQHead = (s_nSignonQHead + 1) & 15;
		--s_nSignonQCount;
		++nDrained;
	}
	const ULONGLONG nNow = GetTickCount64();
	// Rungs up to SPAWN stay open until a different S2C rung answers; FIRST_SNAP
	// and FULL are receipts, repeated inside the window.
	const bool bEchoOpen = s_nEchoState <= 6 || nNow < s_nEchoUntilMs;
	if (!nDrained && s_nEchoState >= 2 && bEchoOpen && nNow >= s_nEchoNextMs && (nOutSize * 8 - w.m_nPos) >= 256 * 8)
	{
		Netchan_WriteSignonState(w, s_nEchoState, s_nEchoSpawn);
		s_nEchoNextMs = nNow + kSignonEchoIntervalMs;
	}

	if (s_bPendingUserInfo && s_nUserInfoSendsLeft > 0 && nNow >= s_nUserInfoNextMs)
	{
		s_nUserInfoNextMs = nNow + kUserInfoIntervalMs;
		const int nVars = static_cast<int>(sizeof(s_userInfoKV) / sizeof(s_userInfoKV[0]));
		w.Write(4, 7);          // NET_SetConVar (dedi type 4)
		w.Write(static_cast<uint8_t>(nVars), 8);
		for (int i = 0; i < nVars; ++i)
		{
			w.WriteString(s_userInfoKV[i][0], 256);
			w.WriteString(s_userInfoKV[i][1], 256);
		}
		--s_nUserInfoSendsLeft;
		S30Log("[NETCHAN] C2S UserInfo (%d ConVars, %d copies left)\n", nVars, s_nUserInfoSendsLeft);
		if (s_nUserInfoSendsLeft <= 0 || s_nSignonSeqMax >= 4)
			s_bPendingUserInfo = false;
	}

	if (s_nPendingMoveBits > 0 && (nOutSize * 8 - w.m_nPos) >= s_nPendingMoveBits + 128)
	{
		S30BitReader_t move(s_pendingMove, (s_nPendingMoveBits + 7) / 8);
		w.WriteFrom(move, s_nPendingMoveBits);
		s_nPendingMoveBits = 0;
	}

	// Relayed client messages (bridge_c2s), each sent nSends times nSpacingMs apart:
	// the dedi drops repeats by their sequence (S21 C2SRel_PumpLocked).
	for (int i = 0; i < kC2SRing; ++i)
	{
		C2SSlot_t& e = s_c2sRing[i];
		if (e.nSendsLeft <= 0 || nNow < e.nNextDueMs || (nOutSize * 8 - w.m_nPos) < e.nBits + 256)
			continue;
		S30BitReader_t msg(e.data, (e.nBits + 7) / 8);
		w.WriteFrom(msg, e.nBits);
		--e.nSendsLeft;
		e.nNextDueMs = nNow + e.nSpacingMs;
	}

	if (s_nAckTick >= 0)
	{
		w.Write(60, 7);         // clc_ClientTick (dedi type 60): [32 tick][32 raw tick], raw = delta base
		w.Write(static_cast<uint32_t>(s_nAckTick), 32);
		w.Write(static_cast<uint32_t>(s_nAckTick), 32);
	}

	while (w.Bytes() < 16)
		w.Write(0, 7);          // NOP padding
	if (w.m_nPos % 8)
		w.Write(0, 8 - w.m_nPos % 8);
	return w.m_bOverflow ? 0 : w.Bytes();
}

static bool Netchan_FlushLocked(const char* pszReason)
{
	if (!s_bActive || !s_pfnSend)
		return false;
	uint8_t packet[1200];
	const int nLen = Netchan_BuildPacketLocked(packet, sizeof(packet));
	if (nLen <= 0)
	{
		S30Log("[NETCHAN] C2S packet overflow (%s)\n", pszReason);
		return false;
	}
	const int nSent = s_pfnSend(packet, nLen);
	s_nLastFlushMs = GetTickCount64();
	const LONG n = InterlockedIncrement(&s_nFlushes);
	if (n <= 10 || n % 2000 == 0)
		S30LogFile("[NETCHAN] C2S #%ld (%s): seq %u ack %u, %d bytes sent %d, nonce 0x%08X subAck %u\n",
			n, pszReason, s_nC2SSeq, s_nInSeq, nLen, nSent, s_nServerNonce, s_nServerSubSeqRecv);
	return nSent == nLen;
}

//-----------------------------------------------------------------------------
// S2C message dispatch (S21 bridge S21Bridge_ProcessMessages_Locked)
//-----------------------------------------------------------------------------
static bool Netchan_ReadString(S30BitReader_t& r, char* psz, int nMax)
{
	// Running out of bits is an empty/short string (S21 Bridge_ReadSignonString);
	// a full window without a NUL is a sheared stream.
	for (int i = 0; i < nMax; ++i)
	{
		if (r.BitsLeft() < 8)
		{
			psz[i] = '\0';
			return true;
		}
		psz[i] = static_cast<char>(r.Read(8));
		if (!psz[i])
			return true;
	}
	psz[nMax - 1] = '\0';
	return false;
}

static void Netchan_Pad(int& nBytes)
{
	nBytes = ((nBytes + 3) & ~3) + 4; // the S21 bridge's bf_read edge padding
}

static bool Netchan_SetCvarAccepted(const char* pszName)
{
	static const char* const s_pszBlocked[] = {
		"sdk_", "bridge_", "net_", "cl_", "r_", "mat_", "fps_", "host_", "con_", "exec", "bind", "alias"
	};
	for (const char* p : s_pszBlocked)
	{
		if (!_strnicmp(pszName, p, strlen(p)))
			return false;
	}
	const int nFlags = S30ConCommand_GetFlags(pszName);
	if (nFlags < 0)
		return false;
	const int FCVAR_REPLICATED = 1 << 13, FCVAR_CHEAT = 1 << 14; // tier1/iconvar.h; S30 values checked with sdk_cvar
	static const char* const s_pszServerDriven[] = {
		"mp_gamemode", "sv_lobbyType", "sv_players", "sv_forceChatToTeamOnly", "tether_maxvel"
	};
	for (const char* p : s_pszServerDriven)
	{
		if (!_stricmp(pszName, p))
			return !(nFlags & FCVAR_CHEAT);
	}
	return (nFlags & FCVAR_REPLICATED) && !(nFlags & FCVAR_CHEAT);
}

// Dispatches one S3 SignonState body (after the 7-bit type); false stops the transfer.
static bool Netchan_OnSignonState(uint8_t* pChan, S30BitReader_t& r)
{
	if (r.BitsLeft() < 40)
	{
		S30Log("[NETCHAN] truncated S2C SignonState (%d bits left)\n", r.BitsLeft());
		return false;
	}
	const uint32_t nState = r.Read(8);
	const uint32_t nSpawn = r.Read(32);
	// dedi CHANGELEVEL = 9; S30 9 is MAYRECONNECT and CHANGELEVEL is 10 (as S21).
	const uint32_t nS30State = nState == 9 ? 10u : nState;
	char sz1[256], sz2[256], sz3[256];
	bool bOk = Netchan_ReadString(r, sz1, sizeof(sz1));
	bOk = bOk && Netchan_ReadString(r, sz2, sizeof(sz2));
	const uint64_t nQword = r.Read64();
	bOk = bOk && Netchan_ReadString(r, sz3, sizeof(sz3));
	if (!bOk || r.m_bOverflow)
	{
		S30Log("[NETCHAN] S2C SignonState(%u) body unreadable -- dropped\n", nState);
		return false;
	}
	S30Log("[NETCHAN] S2C SignonState %u -> %u spawn %d map '%s' mode '%s' qw 0x%llX playlist '%s'\n",
		nState, nS30State, static_cast<int>(nSpawn), sz1, sz2, static_cast<unsigned long long>(nQword), sz3);

	if (nS30State == 2 && s_nLastSentSignon > 2)
	{
		S30Log("[NETCHAN] S2C CONNECTED after C2S %d -- dropped (rewind not handled)\n", s_nLastSentSignon);
		return true;
	}
	if (nS30State >= 4)
	{
		AcquireSRWLockExclusive(&s_lock);
		s_bPendingUserInfo = false;
		ReleaseSRWLockExclusive(&s_lock);
	}

	// S30 net_SignonState: [8 state][32 spawn][String(64)][String(32)][64][String(64)] (S21's, docs/11).
	static uint8_t s_buf[1024];
	S30BitWriter_t w(s_buf, sizeof(s_buf));
	w.Write(nS30State, 8);
	w.Write(nSpawn, 32);
	w.WriteString(sz1, 64);
	w.WriteString(sz2, 32);
	w.Write(nQword & 0xFFFFFFFF, 32);
	w.Write(nQword >> 32, 32);
	w.WriteString(sz3, 64);
	int nBytes = w.Bytes();
	Netchan_Pad(nBytes);

	// NEW waits for SendTables/ClassInfo (the dedi's second transfer); C2S NEW
	// goes now so the dedi sends them.
	if (nState == 3 && !s_bSawSendTables)
	{
		memcpy(s_deferredSignon3, s_buf, sizeof(s_deferredSignon3));
		s_nDeferredSignon3Bytes = nBytes;
		s_nDeferredSignon3Spawn = static_cast<int>(nSpawn);
		s_bDeferredSignon3 = true;
		AcquireSRWLockExclusive(&s_lock);
		s_nSignonSeqSpawn = static_cast<int>(nSpawn);
		Netchan_QueueSignonLocked(3, static_cast<int>(nSpawn));
		s_nLastSentSignon = 3;
		ReleaseSRWLockExclusive(&s_lock);
		S30Log("[NETCHAN] S2C SignonState(3) deferred until SendTables; C2S NEW queued\n");
		return true;
	}
	if (s_bDeferredSignon3 && nState > 3 && (s_nDeferredSignon3Spawn < 0 || s_nDeferredSignon3Spawn == static_cast<int>(nSpawn)))
	{
		S30Log("[NETCHAN] replaying deferred SignonState(3) before %u\n", nState);
		Netchan_Run(pChan, 6, s_deferredSignon3, s_nDeferredSignon3Bytes, 0, nullptr, "SignonState(3) deferred", 5);
		s_bDeferredSignon3 = false;
	}

	if (nS30State != 10)
	{
		if (static_cast<int>(nSpawn) >= 0 && static_cast<int>(nSpawn) != s_nSignonSeqKey)
		{
			s_nSignonSeqKey = static_cast<int>(nSpawn);
			s_nSignonSeqMax = -1;
			AcquireSRWLockExclusive(&s_lock);
			s_nSignonSeqSpawn = static_cast<int>(nSpawn);
			ReleaseSRWLockExclusive(&s_lock);
		}
		if (static_cast<int>(nS30State) <= s_nSignonSeqMax)
		{
			S30Log("[NETCHAN] S2C SignonState(%u) already dispatched for spawn %d -- dropped\n", nS30State, s_nSignonSeqKey);
			return true;
		}
		s_nSignonSeqMax = static_cast<int>(nS30State);
	}
	AcquireSRWLockExclusive(&s_lock);
	if (static_cast<int>(nS30State) != s_nEchoState)
		s_nEchoState = -1;
	ReleaseSRWLockExclusive(&s_lock);

	Netchan_Run(pChan, 6, s_buf, nBytes, 0, nullptr, "SignonState", 5);
	return true;
}

// svc_ServerInfo: dedi [prefix][32 f11][8 f12][7 x String(260)][String(128)][8 flags]
// -> S21/S30 [prefix][64 time][32 f11][8 f12][6 x String(260)][String(128)][String(9)][8 flags].
static bool Netchan_OnServerInfo(uint8_t* pChan, S30BitReader_t& r)
{
	const uint32_t nProto = r.Read(16), nCount = r.Read(32), b1 = r.Read(1), b2 = r.Read(1), b3 = r.Read(1);
	const uint32_t f6 = r.Read(16), f7 = r.Read(32), f8 = r.Read(16), f9 = r.Read(8), f10 = r.Read(8);
	const uint32_t f11 = r.Read(32), f12 = r.Read(8);
	char sz[8][260] = {};
	for (int i = 0; i < 7; ++i)
		r.ReadString(sz[i], 260, 260);
	r.ReadString(sz[7], 128, 128);
	const uint32_t nFlags = r.Read(8);
	if (nFlags)
		r.Skip(64000 * 32);
	if (r.m_bOverflow)
	{
		S30Log("[NETCHAN] ServerInfo body overflow\n");
		return false;
	}
	for (int i = 0; i < 8; ++i)
	{
		for (int c = 0; sz[i][c]; ++c)
		{
			const char ch = sz[i][c];
			if (ch == '/' || ch == '\\' || ch == ':' || (ch == '.' && sz[i][c + 1] == '.'))
			{
				sz[i][0] = '\0';
				break;
			}
		}
	}
	S30Log("[NETCHAN] ServerInfo: proto %u count %u ded %u f6 %u f7 %u f8 %u f9 %u f10 %u f11 %u f12 %u flags %u\n",
		nProto, nCount, b1, f6, f7, f8, f9, f10, f11, f12, nFlags);
	float flTickInterval = 0.0f; // f11: the server's tick interval (0.05 on the dedi)
	memcpy(&flTickInterval, &f11, sizeof(flTickInterval));
	if (flTickInterval > 0.0f && flTickInterval < 1.0f)
		s_flTickInterval = flTickInterval;
	S30Log("[NETCHAN] ServerInfo strings: '%s' '%s' '%s' '%s' '%s' '%s' '%s' '%s'\n",
		sz[0], sz[1], sz[2], sz[3], sz[4], sz[5], sz[6], sz[7]);

	static uint8_t s_buf[4096];
	S30BitWriter_t w(s_buf, sizeof(s_buf));
	w.Write(nProto, 16); w.Write(nCount, 32); w.Write(b1, 1); w.Write(b2, 1); w.Write(b3, 1);
	w.Write(f6, 16); w.Write(f7, 32); w.Write(f8, 16); w.Write(f9, 8); w.Write(f10, 8);
	w.Write(static_cast<uint64_t>(time(nullptr)), 64);
	w.Write(f11, 32);
	w.Write(f12, 8);
	for (int i = 0; i < 6; ++i)
		w.WriteString(sz[i], 260);
	w.WriteString(sz[7], 128);
	w.Write(0, 8);              // String(9), empty
	w.Write(nFlags, 8);
	int nBytes = w.Bytes();
	Netchan_Pad(nBytes);
	return Netchan_Run(pChan, 12, s_buf, nBytes, 0, nullptr, "ServerInfo", 7);
}

// svc_CreateStringTable (S21 bridge format bridge): dedi length 22 bits, S30 24
// (S30 reader 0x488220 reads S21's widths, docs/11). The dedi compresses with
// LZSS ([uncomp:32][comp:32]["LZSS" stream]); S30's Process (0x3F5D30) takes
// only [uncomp:32][blob:32] with blob = [0xFFFFFFFD][uncomp:32][Oodle data],
// so the data is unpacked and Oodle-compressed again.
static bool Netchan_OnCreateStringTable(uint8_t* pChan, S30BitReader_t& r)
{
	static const uint32_t kBufCap = 4u << 20;
	static const uint32_t kUncompMax = kBufCap - 4096;
	char szName[260];
	r.ReadString(szName, sizeof(szName), 256);
	const uint32_t nMaxEntries = r.Read(16);
	if (!nMaxEntries)
	{
		S30Log("[NETCHAN] CreateStringTable '%s': maxEntries 0 -- stop\n", szName);
		return false;
	}
	int nEntryBits = 1;
	for (uint32_t m = nMaxEntries >> 1; m; m >>= 1)
		++nEntryBits;
	const uint32_t nEntries = r.Read(nEntryBits);
	const uint32_t nDataBits = r.Read(22);
	const uint32_t bUserData = r.Read(1);
	const uint32_t nUserDataSize = bUserData ? r.Read(12) : 0;
	const uint32_t nUserDataBits = bUserData ? r.Read(4) : 0;
	const uint32_t bCompressed = r.Read(1);
	const uint32_t nFlags = r.Read(2);
	if (r.m_bOverflow || nDataBits > static_cast<uint32_t>(r.BitsLeft()))
	{
		S30Log("[NETCHAN] CreateStringTable '%s': %u data bits past the end -- stop\n", szName, nDataBits);
		return false;
	}
	const uint32_t nDataBytes = (nDataBits + 7) / 8;
	if (S21Bridge_CstCountInvalid(static_cast<int>(nEntries), static_cast<int>(nMaxEntries)) || (bUserData && nUserDataBits > 14)
		|| nDataBytes > kUncompMax)
	{
		S30Log("[NETCHAN] CreateStringTable '%s': entries %u/%u userdata bits %u data %u bytes -- dropped\n",
			szName, nEntries, nMaxEntries, nUserDataBits, nDataBytes);
		r.Skip(static_cast<int>(nDataBits));
		return true;
	}
	uint8_t* const pRaw = static_cast<uint8_t*>(calloc(nDataBytes + 16, 1));
	if (!pRaw)
		return false;
	for (uint32_t i = 0; i < nDataBits; ++i)
	{
		if (r.Read(1))
			pRaw[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
	}

	const uint8_t* pFinal = pRaw;
	uint32_t nFinalBytes = nDataBytes, nFinalBits = nDataBits, bFinalCompressed = bCompressed;
	uint8_t* pOwned = nullptr;
	if (bCompressed)
	{
		uint32_t nUncomp = 0, nComp = 0, nId = 0;
		if (nDataBytes >= 16)
		{
			memcpy(&nUncomp, pRaw, 4);
			memcpy(&nComp, pRaw + 4, 4);
			memcpy(&nId, pRaw + 8, 4);
		}
		if (nId != 0x53535A4Cu || !nUncomp || nUncomp > kUncompMax || nComp < 8 || 8 + nComp > nDataBytes)
		{
			S30Log("[NETCHAN] CreateStringTable '%s': compressed data is not the dedi's LZSS (id 0x%08X uncomp %u comp %u) -- dropped\n",
				szName, nId, nUncomp, nComp);
			free(pRaw);
			return true;
		}
		uint8_t* const pPlain = static_cast<uint8_t*>(malloc(nUncomp + 16));
		const unsigned int nGot = pPlain ? S30Lzss_Uncompress(pRaw + 8, nDataBytes - 8, pPlain, nUncomp) : 0;
		if (nGot != nUncomp)
		{
			S30Log("[NETCHAN] CreateStringTable '%s': LZSS %u of %u bytes -- dropped\n", szName, nGot, nUncomp);
			free(pPlain);
			free(pRaw);
			return true;
		}
		// Oodle's worst case: 274 bytes per 256 KiB block (OodleLZ_GetCompressedBufferSizeNeeded).
		const size_t nOodleCap = nUncomp + 274 * (nUncomp / 0x40000 + 2);
		uint8_t* const pBlob = v_OodleCompress ? static_cast<uint8_t*>(malloc(16 + nOodleCap)) : nullptr;
		int64_t nOodle = 0;
		if (pBlob)
		{
			nOodle = Netchan_CallOodle(pPlain, nUncomp, pBlob + 16);
			if (nOodle <= 0 || static_cast<size_t>(nOodle) > nOodleCap || 16 + nOodle > kUncompMax)
				nOodle = 0;
		}
		if (nOodle > 0)
		{
			const uint32_t nBlob = 8 + static_cast<uint32_t>(nOodle);
			const uint32_t nMagic = 0xFFFFFFFDu;
			memcpy(pBlob + 0, &nUncomp, 4);
			memcpy(pBlob + 4, &nBlob, 4);
			memcpy(pBlob + 8, &nMagic, 4);
			memcpy(pBlob + 12, &nUncomp, 4);
			free(pPlain);
			pOwned = pBlob;
			pFinal = pBlob;
			nFinalBytes = 8 + nBlob;
			nFinalBits = nFinalBytes * 8;
			bFinalCompressed = 1;
			S30LogFile("[NETCHAN] CreateStringTable '%s': LZSS %u -> %u -> Oodle %lld bytes\n",
				szName, nDataBytes, nUncomp, static_cast<long long>(nOodle));
		}
		else
		{
			free(pBlob);
			if (nUncomp * 8ull >= (1u << 24))
			{
				S30Log("[NETCHAN] CreateStringTable '%s': Oodle failed and %u bytes do not fit 24 bits uncompressed -- dropped\n",
					szName, nUncomp);
				free(pPlain);
				free(pRaw);
				return true;
			}
			pOwned = pPlain;
			pFinal = pPlain;
			nFinalBytes = nUncomp;
			nFinalBits = nUncomp * 8;
			bFinalCompressed = 0;
			S30Log("[NETCHAN] CreateStringTable '%s': Oodle unavailable, %u bytes sent uncompressed\n", szName, nUncomp);
		}
	}
	if (nFinalBits >= (1u << 24))
	{
		S30Log("[NETCHAN] CreateStringTable '%s': %u bits do not fit 24 -- dropped\n", szName, nFinalBits);
		free(pOwned);
		free(pRaw);
		return true;
	}

	static uint8_t* s_pBuf = static_cast<uint8_t*>(malloc(kBufCap));
	bool bOk = false;
	if (s_pBuf)
	{
		S30BitWriter_t w(s_pBuf, static_cast<int>(kBufCap));
		w.WriteString(szName, 256);
		w.Write(nMaxEntries, 16);
		w.Write(nEntries, nEntryBits);
		w.Write(nFinalBits, 24);
		w.Write(bUserData, 1);
		if (bUserData)
		{
			w.Write(nUserDataSize, 12);
			w.Write(nUserDataBits, 4);
		}
		w.Write(bFinalCompressed, 1);
		w.Write(nFlags, 2);
		w.WriteBytes(pFinal, static_cast<int>(nFinalBytes));
		int nLen = w.Bytes();
		Netchan_Pad(nLen);
		if (w.m_bOverflow || nLen > static_cast<int>(kBufCap))
			S30Log("[NETCHAN] CreateStringTable '%s': rewrite overflow\n", szName);
		else
		{
			char szWhat[300];
			snprintf(szWhat, sizeof(szWhat), "CreateStringTable '%s' %u/%u", szName, nEntries, nMaxEntries);
			bOk = Netchan_Run(pChan, 16, s_pBuf, nLen, 0, nullptr, szWhat, 12);
		}
	}
	free(pOwned);
	free(pRaw);
	return bOk || true; // a table Process rejects does not shear the stream
}

// Walks an S3 message stream (DataBlock, reliable transfer or packet tail).
// S3 svc_Snapshot body (after the 7-bit type) -> S30's snapshot message (type 50,
// reader 0x48AD70). S30's header is S21's (same ServerTickInfo reader, same reads
// after it), so this is the S21 bridge's re-encode: ServerTickInfo's 1-bit field
// widens to 16 bits (137 -> 152), the payload length from 22 to 24 bits; the entity
// payload is copied as is (decoded with the dedi's own SendTables).
static bool Netchan_OnSnapshot(uint8_t* pChan, S30BitReader_t& r)
{
	const uint32_t nServerCount = r.Read(32);
	const uint32_t nTick1 = r.Read(32), nTick2 = r.Read(32), nCompute = r.Read(16), nStdDev = r.Read(16);
	const uint32_t bConn = r.Read(1), nByte = r.Read(8), nCmdRun = r.Read(32);
	const uint32_t nMaxEntries = r.Read(15);
	const uint32_t bDelta = r.Read(1);
	uint32_t nDeltaFrom = 0, bRawDelta = 0, nRawDeltaFrom = 0;
	if (bDelta)
	{
		nDeltaFrom = r.Read(32);
		bRawDelta = r.Read(1);
		if (bRawDelta)
			nRawDeltaFrom = r.Read(32);
	}
	const uint32_t sh0 = r.Read(32), sh1 = r.Read(32), sh2 = r.Read(32), sh3 = r.Read(16); // 112 bits, same layout
	const uint32_t nLengthBits = r.Read(22);
	if (r.m_bOverflow || static_cast<int>(nLengthBits) > r.BitsLeft())
	{
		S30Log("[NETCHAN] Snapshot: %u payload bits, %d left -- stop\n", nLengthBits, r.BitsLeft());
		return false;
	}

	const int nOutBits = 32 + 152 + 15 + 1 + (bDelta ? 33 + (bRawDelta ? 32 : 0) : 0) + 112 + 24 + static_cast<int>(nLengthBits);
	const int nCapacity = (nOutBits + 7) / 8 + 16;
	static uint8_t* s_pBuf = nullptr;
	static int s_nCapacity = 0;
	if (s_nCapacity < nCapacity)
	{
		free(s_pBuf);
		s_pBuf = static_cast<uint8_t*>(malloc(nCapacity));
		s_nCapacity = s_pBuf ? nCapacity : 0;
		if (!s_pBuf)
			return false;
	}
	S30BitWriter_t w(s_pBuf, s_nCapacity);
	w.Write(nServerCount, 32);
	w.Write(nTick1, 32); w.Write(nTick2, 32); w.Write(nCompute, 16); w.Write(nStdDev, 16);
	w.Write(bConn, 16); w.Write(nByte, 8); w.Write(nCmdRun, 32);
	w.Write(nMaxEntries, 15);
	w.Write(bDelta, 1);
	if (bDelta)
	{
		w.Write(nDeltaFrom, 32);
		w.Write(bRawDelta, 1);
		if (bRawDelta)
			w.Write(nRawDeltaFrom, 32);
	}
	w.Write(sh0, 32); w.Write(sh1, 32); w.Write(sh2, 32); w.Write(sh3, 16);
	w.Write(nLengthBits, 24);
	w.WriteFrom(r, static_cast<int>(nLengthBits));
	int nLen = w.Bytes();
	Netchan_Pad(nLen);
	const bool bOk = !w.m_bOverflow && Netchan_Run(pChan, 50, s_pBuf, nLen, 0, nullptr, "Snapshot", 40);

	static volatile LONG s_nSnapshots = 0;
	const LONG n = InterlockedIncrement(&s_nSnapshots);
	if (n <= 20 || !bOk || n % 1000 == 0)
		S30Log("[NETCHAN] Snapshot #%ld tick %u %s%u, %u payload bits, ok %d\n", n, nTick1,
			bDelta ? "delta from " : "full", bDelta ? nDeltaFrom : 0, nLengthBits, bOk);
	{
		// Clock report (diagnostic): once a second, the ServerTickInfo the client is fed
		// and the server tick rate against wall time.
		static ULONGLONG s_nStartMs = 0, s_nLastMs = 0;
		static uint32_t s_nStartTick = 0, s_nPrevTick = 0;
		static int s_nCount = 0;
		static ULONGLONG s_nMaxGapMs = 0;
		static int s_nMaxTickStep = 0;
		const ULONGLONG nNow = GetTickCount64();
		if (s_nLastMs && nNow - s_nLastMs > s_nMaxGapMs)
			s_nMaxGapMs = nNow - s_nLastMs;
		if (s_nPrevTick && static_cast<int>(nTick1 - s_nPrevTick) > s_nMaxTickStep)
			s_nMaxTickStep = static_cast<int>(nTick1 - s_nPrevTick);
		s_nLastMs = nNow;
		s_nPrevTick = nTick1;
		++s_nCount;
		if (!s_nStartMs)
		{
			s_nStartMs = nNow;
			s_nStartTick = nTick1;
		}
		else if (nNow - s_nStartMs >= 1000)
		{
			const double flWall = (nNow - s_nStartMs) / 1000.0;
			S30LogFile("[CLOCK] snapshots %d in %.3fs: ticks +%d (%.0f%% of wall at %.3f) max step %d, max gap %llu ms | tick2 %u compute %u stddev %u struggling %u cpu %u cmdTick %u\n",
				s_nCount, flWall, static_cast<int>(nTick1 - s_nStartTick), 100.0 * (nTick1 - s_nStartTick) * s_flTickInterval / flWall,
				s_flTickInterval, s_nMaxTickStep, s_nMaxGapMs, nTick2, nCompute, nStdDev, bConn, nByte, nCmdRun);
			s_nStartMs = nNow;
			s_nStartTick = nTick1;
			s_nCount = 0;
			s_nMaxGapMs = 0;
			s_nMaxTickStep = 0;
		}
	}
	if (bOk)
	{
		s_nLastSnapshotMs = GetTickCount64();
		AcquireSRWLockExclusive(&s_lock);
		s_nAckTick = static_cast<int>(nTick1);
		ReleaseSRWLockExclusive(&s_lock);
		S30Props_OnSnapshot();
		S30ScriptRemote_OnSnapshot(nTick1);
	}
	return true; // the payload is consumed either way: the stream stays aligned
}

// S3 svc_TempEntities (41) -> S30 51 (S21 48's reader): the length field grows from 22
// to 24 bits, the blob is copied as is (S21 bridge). A temp entity of a class S30
// does not have reaches a stub ClientClass with no create-event function and is
// skipped by the event queue (0x2F6550).
static bool Netchan_OnTempEntities(uint8_t* pChan, S30BitReader_t& r)
{
	const uint32_t nTick = r.Read(32);
	const uint32_t nCount = r.Read(10);
	const uint32_t nLength = r.Read(22);
	if (r.m_bOverflow || static_cast<int>(nLength) > r.BitsLeft())
	{
		S30Log("[NETCHAN] TempEntities: %u bits, %d left -- stop\n", nLength, r.BitsLeft());
		return false;
	}
	static uint8_t s_buf[(66 + (1 << 22)) / 8 + 16];
	S30BitWriter_t w(s_buf, sizeof(s_buf));
	w.Write(nTick, 32);
	w.Write(nCount, 10);
	w.Write(nLength, 24);
	w.WriteFrom(r, static_cast<int>(nLength));
	int nLen = w.Bytes();
	Netchan_Pad(nLen);
	const bool bOk = !w.m_bOverflow && Netchan_Run(pChan, 51, s_buf, nLen, 0, nullptr, "TempEntities", 41);
	static volatile LONG s_nLog = 0;
	const LONG n = InterlockedIncrement(&s_nLog);
	if (n <= 10 || !bOk || n % 500 == 0)
		S30LogFile("[NETCHAN] TempEntities #%ld tick %u, %u entries, %u bits, ok %d\n", n, nTick, nCount, nLength, bOk);
	return true; // consumed either way: the stream stays aligned
}

// S3 net_ScriptMessage (68, the dedi SDK's S2C ScriptRemote lane; the S21 bridge's
// S21Bridge_HandleS2CScriptRemote). A call frame is [32 magic 'SR2D'][32 snapshot
// tick][8 isUI][8 replay][8 name length][name][8 args][typed args]; the untyped ones
// run on the S30 VMs (bridge_scriptremote_s30), each name logged once here.
static bool Netchan_OnScriptMessage(S30BitReader_t& r)
{
	const uint32_t bTyped = r.Read(1);
	const uint32_t nBytes = r.Read(16);
	if (r.m_bOverflow || static_cast<int>(nBytes) * 8 > r.BitsLeft())
	{
		S30Log("[NETCHAN] ScriptMessage: %u bytes, %d bits left -- stop\n", nBytes, r.BitsLeft());
		return false;
	}
	uint8_t frame[1024];
	const uint32_t nKeep = nBytes < sizeof(frame) ? nBytes : static_cast<uint32_t>(sizeof(frame));
	r.ReadBytes(frame, static_cast<int>(nKeep));
	r.Skip(static_cast<int>(nBytes - nKeep) * 8);

	static volatile LONG s_nMessages = 0;
	const LONG n = InterlockedIncrement(&s_nMessages);
	uint32_t nMagic = 0;
	if (nKeep >= 4)
		memcpy(&nMagic, frame, 4);
	char szName[65] = "";
	if (!bTyped && nMagic == 0x53523244u && nKeep >= 11)
	{
		const uint32_t nName = frame[10];
		if (nName && nName <= 64 && 11 + nName <= nKeep)
		{
			memcpy(szName, frame + 11, nName);
			szName[nName] = '\0';
		}
	}
	// Log each function name once (up to 128 names), then every 500th message.
	static char s_szSeen[128][65];
	static int s_nSeen = 0;
	bool bNew = szName[0] != '\0';
	for (int i = 0; i < s_nSeen && bNew; ++i)
		bNew = strcmp(s_szSeen[i], szName) != 0;
	if (bNew && s_nSeen < 128)
		strcpy_s(s_szSeen[s_nSeen++], szName);
	if ((bNew && s_nSeen <= 128) || n % 500 == 0)
		S30LogFile("[NETCHAN] ScriptMessage #%ld: %s%s (typed %u, magic 0x%08X, %u bytes)\n", n,
			szName[0] ? "S2C remote call " : "frame", szName, bTyped, nMagic, nBytes);
	if (!bTyped && nKeep == nBytes)
		S30ScriptRemote_OnFrame(frame, nBytes);
	return true;
}

static void Netchan_Dispatch(uint8_t* pChan, const uint8_t* pData, int nBytes, int nStartBit, const char* pszSource)
{
	S30BitReader_t r(pData, nBytes, nStartBit);
	char szSkipped[256];
	int nSkippedLen = 0;
	szSkipped[0] = '\0';
	int nMessages = 0;
	while (r.BitsLeft() >= 7)
	{
		const int nStart = r.m_nPos;
		const int nS3 = static_cast<int>(r.Read(7));
		if (nS3 == 0)
			continue;
		++nMessages;
		bool bContinue = true;
		switch (nS3)
		{
		case 1: // net_Disconnect
		{
			char szReason[1024];
			Netchan_ReadString(r, szReason, sizeof(szReason));
			S30Log("[NETCHAN] server closed the connection: \"%s\"\n", szReason);
			S30Console_Submit("bridge_disconnect");
			return;
		}
		case 3:  // net_StringCmd, same format
		case 11: // svc_Playlists
		{
			int nEnd = 0;
			bContinue = Netchan_Run(pChan, Netchan_S30Type(nS3), pData, nBytes, r.m_nPos, &nEnd, nS3 == 3 ? "StringCmd" : "Playlists", nS3);
			r.m_nPos = nEnd;
			break;
		}
		case 4: // net_SetConVar: replicated, non-cheat only
		{
			const uint32_t nPairs = r.Read(8);
			static char s_szNames[255][256], s_szValues[255][256];
			int nKept = 0;
			for (uint32_t i = 0; i < nPairs && !r.m_bOverflow; ++i)
			{
				char szName[256], szValue[256];
				if (!Netchan_ReadString(r, szName, 256) || !Netchan_ReadString(r, szValue, 256))
				{
					bContinue = false;
					break;
				}
				if (!Netchan_SetCvarAccepted(szName))
				{
					static volatile LONG s_nDropLog = 0;
					if (InterlockedIncrement(&s_nDropLog) <= 64)
						S30Log("[NETCHAN] SetConVar '%s' = '%s' dropped\n", szName, szValue);
					continue;
				}
				strcpy_s(s_szNames[nKept], szName);
				strcpy_s(s_szValues[nKept], szValue);
				++nKept;
			}
			if (!bContinue || r.m_bOverflow)
			{
				bContinue = false;
				break;
			}
			if (!nKept)
				break;
			static uint8_t s_buf[20480];
			S30BitWriter_t w(s_buf, sizeof(s_buf));
			w.Write(static_cast<uint32_t>(nKept), 8);
			for (int i = 0; i < nKept; ++i)
			{
				w.WriteString(s_szNames[i], 256);
				w.WriteString(s_szValues[i], 256);
			}
			int nLen = w.Bytes();
			Netchan_Pad(nLen);
			if (!w.m_bOverflow)
				Netchan_Run(pChan, 5, s_buf, nLen, 0, nullptr, "SetConVar", 4);
			break;
		}
		case 5:
			bContinue = Netchan_OnSignonState(pChan, r);
			break;
		case 6: // net_MTXUserMsg: [32 n][n bits], consumed
		{
			const uint32_t n = r.Read(32);
			if (n > static_cast<uint32_t>(r.BitsLeft()))
				bContinue = false;
			else
				r.Skip(static_cast<int>(n));
			break;
		}
		case 7:
			bContinue = Netchan_OnServerInfo(pChan, r);
			break;
		case 10: // svc_SetPause: no S30 message
			r.Read(1);
			break;
		case 16: // svc_Print: dedi String -> S30 [1 show][4 prefix][String(2048)]
		{
			char szText[2048];
			Netchan_ReadString(r, szText, sizeof(szText));
			static uint8_t s_buf[2100];
			S30BitWriter_t w(s_buf, sizeof(s_buf));
			w.Write(1, 1);      // ProcessPrint (0x3F4590) only prints when set
			w.Write(0, 4);
			w.WriteString(szText, 2048);
			int nLen = w.Bytes();
			Netchan_Pad(nLen);
			Netchan_Run(pChan, 20, s_buf, nLen, 0, nullptr, "Print", 16);
			break;
		}
		case 22: // svc_ServerTick: dedi 1-bit field -> 16 bits (S21 format bridge)
		{
			const uint32_t t1 = r.Read(32), t2 = r.Read(32), c = r.Read(16), d = r.Read(16), b = r.Read(1), y = r.Read(8), l = r.Read(32);
			// The dedi sends one with every snapshot, and the snapshot carries the same
			// ServerTickInfo. S30 feeds both to its clock drift manager (0x248290, from
			// the ServerTick handler and from snapshot processing); the second call for
			// a tick has zero server time elapsed, puts inf/nan in the rate ring, and the
			// trimmed mean pins the client clock rate at its 0.1 floor until the 0.2 s
			// behind-snap -- the whole game slows and catches up about once a second.
			// While snapshots flow they are the only tick source the client gets.
			if (GetTickCount64() - s_nLastSnapshotMs < kServerTickSnapshotWindowMs)
			{
				static volatile LONG s_nTicksConsumed = 0;
				if (InterlockedIncrement(&s_nTicksConsumed) == 1)
					S30Log("[NETCHAN] svc_ServerTick %u consumed: snapshots carry the tick (one clock drift sample per tick)\n", t1);
				break;
			}
			static uint8_t s_buf[32];
			S30BitWriter_t w(s_buf, sizeof(s_buf));
			w.Write(t1, 32); w.Write(t2, 32); w.Write(c, 16); w.Write(d, 16); w.Write(b, 16); w.Write(y, 8); w.Write(l, 32);
			int nLen = w.Bytes();
			Netchan_Pad(nLen);
			Netchan_Run(pChan, 25, s_buf, nLen, 0, nullptr, "ServerTick", 22);
			break;
		}
		case 23: // svc_PersistenceDefFile: [64 version][16 length][bytes], S30 reader the same
		{
			int nEnd = 0;
			s_bPdefOk = Netchan_Run(pChan, 26, pData, nBytes, r.m_nPos, &nEnd, "PersistenceDefFile", 23);
			r.m_nPos = nEnd;
			bContinue = s_bPdefOk;
			break;
		}
		case 24: // svc_UseCachedPersistenceDefFile
		case 25: // svc_PersistenceBaseline
		case 26: // svc_PersistenceUpdateVar
		case 27: // svc_PersistenceNotifySaved
		{
			if (nS3 != 27 && !s_bPdefOk)
			{
				// Native Process would mark persistence loaded against a pdef this
				// client never parsed (S21 bridge).
				const int nAt = r.m_nPos;
				if (!S21Bridge_SkipNonSendTable(pData, nBytes * 8, r.m_nPos, nS3))
				{
					S30Log("[NETCHAN] S3 %d without a parsed pdef, body not walkable -- stop\n", nS3);
					bContinue = false;
				}
				else
					S30Log("[NETCHAN] S3 %d dropped: no pdef parsed (%d bits)\n", nS3, r.m_nPos - nAt);
				break;
			}
			int nEnd = 0;
			bContinue = Netchan_Run(pChan, Netchan_S30Type(nS3), pData, nBytes, r.m_nPos, &nEnd, "Persistence", nS3);
			r.m_nPos = nEnd;
			break;
		}
		case 28: // svc_DLCNotifyOwnership: [64 bitfield][N x 16 balances], N not on the wire -- consumed
		{
			// S21 bridge auto rule: only when the rest is that body (+ at most a 1-bit pad).
			const int nAfter = r.BitsLeft() - 64;
			if (nAfter < 0 || nAfter % 16 > 1)
			{
				S30Log("[NETCHAN] DLCNotifyOwnership: %d bits left is not 64 + N x 16 -- stop\n", r.BitsLeft());
				bContinue = false;
				break;
			}
			r.Skip(64 + nAfter / 16 * 16);
			S30Log("[NETCHAN] DLCNotifyOwnership consumed (N = %d balances)\n", nAfter / 16);
			break;
		}
		case 32: // svc_PlaylistChange: dedi string, S30 a 7-bit index -- applied by name (bridge_hud)
		{
			char szName[65];
			r.ReadString(szName, sizeof(szName), 64);
			S30Hud_PlaylistChange(szName);
			break;
		}
		case 12:
			bContinue = Netchan_OnCreateStringTable(pChan, r);
			break;
		case 13: // svc_UpdateStringTable: S30 17 reads S21's format ([5 id][1 many][16 count][20 length]);
		{        // the ids follow the bridge's in-order CreateStringTable
			int nEnd = 0;
			bContinue = Netchan_Run(pChan, 17, pData, nBytes, r.m_nPos, &nEnd, "UpdateStringTable", 13);
			r.m_nPos = nEnd;
			break;
		}
		case 40:
			bContinue = Netchan_OnSnapshot(pChan, r);
			break;
		case 18: // svc_FixAngle: S30 22 reads S21 21's format (same reader body, identical writer)
		{
			int nEnd = 0;
			bContinue = Netchan_Run(pChan, 22, pData, nBytes, r.m_nPos, &nEnd, "FixAngle", 18);
			r.m_nPos = nEnd;
			break;
		}
		case 68: // net_ScriptMessage (dedi SDK): [1 typed][16 byteLen][frame], the S2C ScriptRemote lane
			bContinue = Netchan_OnScriptMessage(r);
			break;
		case 14: // svc_VoiceData
		case 15: // svc_DurangoVoiceData
		case 17: // svc_Sounds: [32 tick][8 payload bits][payload], S30 21 reads S21 20's format
		case 19: // svc_CrosshairAngle
		case 20: // svc_GrantClientSidePickup
		case 29: // svc_MatchmakingETAs
		case 30: // svc_MatchmakingStatus
		case 31: // svc_MTXUserInfo
		case 33: // svc_SetTeam
		{        // the S21 bridge dispatches these natively (renumbered); docs/11: same S30 readers
			int nEnd = 0;
			bContinue = Netchan_Run(pChan, Netchan_S30Type(nS3), pData, nBytes, r.m_nPos, &nEnd, "native", nS3);
			r.m_nPos = nEnd;
			break;
		}
		case 37: // svc_UserMessage: [8 type][12 length bits][payload], direct handlers (bridge_usermessage)
			bContinue = S30UserMessage_OnS3(r);
			break;
		case 41: // svc_TempEntities: [32 tick][10 count][22 length][blob] -> S30 51: length 24 bits (S21 bridge)
			bContinue = Netchan_OnTempEntities(pChan, r);
			break;
		case 66: // svc_SetClassVar (dedi SDK): two strings, consumed as on S21
		case 67: // svc_SystemSayText (dedi SDK): two strings + admin bit
		{
			char szFirst[512], szSecond[512];
			if (!Netchan_ReadString(r, szFirst, sizeof(szFirst)) || !Netchan_ReadString(r, szSecond, sizeof(szSecond)))
			{
				bContinue = false;
				break;
			}
			if (nS3 == 67)
				S30Hud_SystemSayText(szFirst, szSecond, r.Read(1) != 0);
			break;
		}
		case 69: // svc_DebugOverlay (dedi SDK): [16 byteLen][bytes], consumed (dev draws)
		case 70: // svc_ChatBuilder (dedi SDK): [16 byteLen][bytes], shown as text (bridge_hud)
		{
			const uint32_t nLen = r.Read(16);
			if (r.m_bOverflow || static_cast<int>(nLen) * 8 > r.BitsLeft() || nLen > 2048)
			{
				bContinue = false;
				break;
			}
			uint8_t payload[2048];
			r.ReadBytes(payload, static_cast<int>(nLen));
			if (nS3 == 70)
				S30Hud_ChatBuilder(payload, static_cast<int>(nLen));
			else
			{
				static bool s_bSeen = false;
				if (!s_bSeen)
				{
					s_bSeen = true;
					S30Log("[NETCHAN] S3 69 (DebugOverlay) consumed, not drawn\n");
				}
			}
			break;
		}
		case 8:  // svc_SendTable: S30 reader the same (docs/11); no S21-style table renames yet
		case 9:  // svc_ClassInfo
		{
			s_bSawSendTables = true;
			char szTable[256] = "";
			bool bNoRecvTable = false;
			if (nS3 == 8) // [1 needs decoder][32 length][name ...]
			{
				S30BitReader_t peek(pData, nBytes, r.m_nPos);
				const uint32_t bNeedsDecoder = peek.Read(1);
				peek.Read(32);
				peek.ReadString(szTable, sizeof(szTable), 256);
				bNoRecvTable = bNeedsDecoder && !Netchan_HasRecvTable(szTable);
				if (bNoRecvTable && s_bMatcherLenient && v_DecoderDone)
				{
					// A stub RecvTable gets the decoder; its props all end on dummies.
					StubTable_t* const pStub = Netchan_StubTable(szTable);
					if (pStub && Netchan_RegisterStubTable(*pStub))
					{
						S30Log("[NETCHAN] SendTable '%s': no S30 RecvTable, stub registered\n", szTable);
						bNoRecvTable = false;
					}
					else
						S30Log("[NETCHAN] SendTable '%s': stub RecvTable not registered\n", szTable);
				}
			}
			int nEnd = 0;
			bool bOk;
			static volatile LONG s_nTableLog = 0;
			if (bNoRecvTable)
			{
				// No S30 RecvTable: the table goes in without a decoder (the S21
				// bridge's fallback when it cannot build a stub RecvTable).
				const uint32_t nLen = S30BitReader_t(pData, nBytes, r.m_nPos + 1).Read(32);
				static uint8_t* s_pTable = static_cast<uint8_t*>(malloc(1 << 20));
				if (!s_pTable || 33 + nLen > (1u << 20) * 8 - 64 || 33 + nLen > static_cast<uint32_t>(r.BitsLeft()))
				{
					S30Log("[NETCHAN] SendTable '%s': %u bits too long -- stop\n", szTable, nLen);
					bContinue = false;
					break;
				}
				S30BitWriter_t w(s_pTable, (33 + nLen + 7) / 8 + 8);
				S30BitReader_t src(pData, nBytes, r.m_nPos + 1);
				w.Write(0, 1);
				w.WriteFrom(src, 32 + static_cast<int>(nLen));
				int nLenBytes = w.Bytes();
				Netchan_Pad(nLenBytes);
				int nCopyEnd = 0;
				bOk = Netchan_Run(pChan, 13, s_pTable, nLenBytes, 0, &nCopyEnd, "SendTable (no decoder)", 8);
				nEnd = r.m_nPos + nCopyEnd;
				S30Log("[NETCHAN] SendTable '%s': no S30 RecvTable, sent without a decoder\n", szTable);
			}
			else
				bOk = Netchan_Run(pChan, Netchan_S30Type(nS3), pData, nBytes, r.m_nPos, &nEnd, nS3 == 8 ? "SendTable" : "ClassInfo", nS3);
			if (!bOk || InterlockedIncrement(&s_nTableLog) <= 3 || nS3 == 9)
				S30LogFile("[NETCHAN] %s at bit %d: %d bits, ok %d\n", nS3 == 8 ? "SendTable" : "ClassInfo", nStart, nEnd - r.m_nPos, bOk);
			r.m_nPos = nEnd;
			bContinue = bOk;
			break;
		}
		default:
		{
			// Body walkable but not dispatched yet (CreateStringTable,
			// PlaylistOverrides, DatatableChecksum, ...).
			if (!S21Bridge_SkipNonSendTable(pData, nBytes * 8, r.m_nPos, nS3))
			{
				S30Log("[NETCHAN] %s: S3 %d at bit %d has no handler -- rest (%d bits) dropped\n",
					pszSource, nS3, nStart, nBytes * 8 - nStart);
				return;
			}
			InterlockedIncrement(&s_nSkipped);
			if (nSkippedLen < 200)
				nSkippedLen += snprintf(szSkipped + nSkippedLen, sizeof(szSkipped) - nSkippedLen, " %d(%d)", nS3, r.m_nPos - nStart);
			break;
		}
		}
		if (!bContinue || r.m_bOverflow)
		{
			S30Log("[NETCHAN] %s: stopped at S3 %d (bit %d of %d)\n", pszSource, nS3, nStart, nBytes * 8);
			return;
		}
	}
	if (szSkipped[0])
		S30Log("[NETCHAN] %s: not dispatched yet:%s\n", pszSource, szSkipped);
	S30LogFile("[NETCHAN] %s: %d messages\n", pszSource, nMessages);
}

//-----------------------------------------------------------------------------
// S2C packets (S21 bridge S21Bridge_Hook_ProcessPacket / ParseSubChannelData)
//-----------------------------------------------------------------------------
static void Netchan_ResetReliable(void)
{
	s_rel.bActive = false;
	s_rel.nTotal = s_rel.nReceived = s_rel.nUncompressed = 0;
}

static void Netchan_AckEntry(uint32_t nEntrySeq)
{
	AcquireSRWLockExclusive(&s_lock);
	if (nEntrySeq + 1 > s_nServerSubSeqRecv)
		s_nServerSubSeqRecv = nEntrySeq + 1;
	ReleaseSRWLockExclusive(&s_lock);
}

static void Netchan_DispatchReliable(uint8_t* pChan)
{
	if (!s_rel.bActive || s_rel.nReceived < s_rel.nTotal)
		return;
	char szWhat[48];
	snprintf(szWhat, sizeof(szWhat), "reliable #%u", s_rel.nEntrySeq);
	if (s_rel.bCompressed)
	{
		uint8_t* const pOut = static_cast<uint8_t*>(malloc(static_cast<size_t>(s_rel.nUncompressed) + 16));
		const unsigned int nOut = pOut && s_rel.nUncompressed > 0 && s_rel.nUncompressed < 0x200000
			? S30Lzss_Uncompress(s_rel.pBuf, s_rel.nTotal, pOut, s_rel.nUncompressed) : 0;
		S30Log("[NETCHAN] %s: %d bytes, LZSS -> %u\n", szWhat, s_rel.nTotal, nOut);
		if (nOut)
		{
			memset(pOut + nOut, 0, 16);
			Netchan_Dispatch(pChan, pOut, static_cast<int>(nOut), 0, szWhat);
		}
		free(pOut);
	}
	else
	{
		S30Log("[NETCHAN] %s: %d bytes\n", szWhat, s_rel.nTotal);
		Netchan_Dispatch(pChan, s_rel.pBuf, s_rel.nTotal, 0, szWhat);
	}
	Netchan_ResetReliable();
}

static bool Netchan_ParseSubchannel(uint8_t* pChan, S30BitReader_t& r)
{
	const uint32_t nMagic = r.Read(32); // the dedi's reader does not check it
	const uint32_t nSubSeq = r.Read(32);
	const uint32_t nReqId = r.Read(10);
	if (nMagic != 0xABCDEF01u)
		S30Log("[NETCHAN] subchannel magic 0x%08X\n", nMagic);
	// SV_ActivateServer restarts the waiting list at entry 0 and resends it until acked.
	if (nSubSeq == 0)
	{
		AcquireSRWLockExclusive(&s_lock);
		if (s_nServerSubSeqRecv > 0 && !s_bSubSeqRebased)
		{
			S30Log("[NETCHAN] reliable receive floor %u reset (list restart)\n", s_nServerSubSeqRecv);
			s_nServerSubSeqRecv = 0;
			Netchan_ResetReliable();
		}
		ReleaseSRWLockExclusive(&s_lock);
		s_bSubSeqRebased = true;
	}
	else
		s_bSubSeqRebased = false;
	if (nReqId == 0 && r.Read(1))
	{
		const uint32_t nNonce = r.Read(32);
		if (nNonce)
		{
			AcquireSRWLockExclusive(&s_lock);
			if (s_nServerNonce && nNonce != s_nServerNonce)
			{
				S30Log("[NETCHAN] dedi re-created its netchannel: nonce 0x%08X -> 0x%08X\n", s_nServerNonce, nNonce);
				s_nServerSubSeqRecv = 0;
				s_bSubSeqRebased = true;
				Netchan_ResetReliable();
			}
			if (s_nServerNonce != nNonce)
				S30Log("[NETCHAN] server nonce 0x%08X\n", nNonce);
			s_nServerNonce = nNonce;
			s_bNeedNonceAck = true;
			ReleaseSRWLockExclusive(&s_lock);
		}
	}

	while (!r.m_bOverflow)
	{
		const uint32_t nEntrySeq = r.Read(32);
		int nChunk = 0;
		AcquireSRWLockShared(&s_lock);
		const uint32_t nFloor = s_nServerSubSeqRecv;
		ReleaseSRWLockShared(&s_lock);
		if (r.Read(1)) // first fragment
		{
			const int nSize = static_cast<int>(r.Read(19));
			if (nSize > kReliableMax)
			{
				S30Log("[NETCHAN] subchannel transfer of %d bytes too large\n", nSize);
				return false;
			}
			nChunk = nSize > kSubchanFragment ? kSubchanFragment : nSize;
			const bool bCompressed = r.Read(1) != 0;
			const int nUncompressed = bCompressed ? static_cast<int>(r.Read(22)) : 0;
			if (nEntrySeq < nFloor)
			{
				r.Skip(nChunk * 8);
				if (!r.Read(1))
					break;
				continue;
			}
			if (s_rel.bActive && s_rel.nReceived < s_rel.nTotal)
				S30Log("[NETCHAN] reliable transfer #%u abandoned at %d/%d bytes\n", s_rel.nEntrySeq, s_rel.nReceived, s_rel.nTotal);
			if (s_rel.nCapacity < nSize + 16)
			{
				free(s_rel.pBuf);
				s_rel.pBuf = static_cast<uint8_t*>(malloc(static_cast<size_t>(nSize) + 16));
				s_rel.nCapacity = s_rel.pBuf ? nSize + 16 : 0;
			}
			if (!s_rel.pBuf)
				return false;
			memset(s_rel.pBuf, 0, nSize + 16);
			s_rel.bActive = true;
			s_rel.bCompressed = bCompressed;
			s_rel.nTotal = nSize;
			s_rel.nReceived = 0;
			s_rel.nUncompressed = nUncompressed;
			s_rel.nEntrySeq = nEntrySeq;
		}
		else
			nChunk = r.Read(1) ? static_cast<int>(r.Read(10)) : kSubchanFragment; // last: size, else full
		if (nChunk > kSubchanFragment)
		{
			S30Log("[NETCHAN] subchannel chunk %d > %d\n", nChunk, kSubchanFragment);
			return false;
		}
		if (nEntrySeq < nFloor)
			r.Skip(nChunk * 8);
		else if (nChunk > 0 && s_rel.bActive && s_rel.nReceived + nChunk <= s_rel.nTotal)
		{
			r.ReadBytes(s_rel.pBuf + s_rel.nReceived, nChunk);
			s_rel.nReceived += nChunk;
			Netchan_AckEntry(nEntrySeq);
		}
		else if (nChunk > 0)
		{
			// No receive buffer for a continuation: skip it so the unreliable part stays aligned.
			r.Skip(nChunk * 8);
			Netchan_AckEntry(nEntrySeq);
			S30Log("[NETCHAN] subchannel data for entry %u with no transfer -- skipped %d bytes\n", nEntrySeq, nChunk);
		}
		// The next first fragment reuses the buffer, so a complete transfer goes now.
		Netchan_DispatchReliable(pChan);
		if (!r.Read(1))
			break;
	}
	return !r.m_bOverflow;
}

static void Netchan_DispatchDataBlock(uint8_t* pChan)
{
	int nSize = 0, nNr = -1;
	uint8_t* const pSignon = S30DataBlock_TakeSignon(&nSize, &nNr);
	if (!pSignon)
		return;
	char szWhat[32];
	snprintf(szWhat, sizeof(szWhat), "DataBlock %d", nNr);
	S30Log("[NETCHAN] dispatching %s (%d bytes)\n", szWhat, nSize);
	Netchan_Dispatch(pChan, pSignon, nSize, 0, szWhat);
	free(pSignon);
}

static void Netchan_OnPacket(uint8_t* pChan, uint8_t* pPacket)
{
	const uint8_t* const pData = *reinterpret_cast<uint8_t* const*>(pPacket + PKT_DATA);
	const int nSize = *reinterpret_cast<const int*>(pPacket + PKT_SIZE);
	if (!pData || nSize < 9)
		return;
	S30BitReader_t r(pData, nSize);
	const uint32_t nSeq = r.Read(32);
	const uint32_t nAck = r.Read(32);
	const uint32_t nFlags = r.Read(8);
	AcquireSRWLockShared(&s_lock);
	const uint32_t nLastSeq = s_nInSeq;
	ReleaseSRWLockShared(&s_lock);
	if (nLastSeq && nSeq <= nLastSeq)
	{
		const LONG n = InterlockedIncrement(&s_nStale);
		if (n <= 8 || n % 1000 == 0)
			S30Log("[NETCHAN] stale packet seq %u <= %u dropped (#%ld)\n", nSeq, nLastSeq, n);
		return;
	}
	const uint32_t nChoked = (nFlags & 0x10) ? r.Read(8) : 0;
	if (r.Read(1)) // nonce section
	{
		const uint32_t nMagic = r.Read(32);
		if (nMagic != 0xFDBAC34Du && nMagic != 0xFDB97A8Du)
		{
			// The dedi drops a packet with a bad nonce magic; so must the bridge.
			const LONG n = InterlockedIncrement(&s_nBadNonce);
			if (n <= 10)
				S30Log("[NETCHAN] bad nonce magic 0x%08X seq %u -- dropped\n", nMagic, nSeq);
			return;
		}
		if (r.Read(1))
			r.Read(32); // the dedi acks the bridge's nonce; the bridge sends no reliable data
		r.Read(32);     // subchannel ack seq
		r.Read(10);
	}
	if (r.m_bOverflow)
		return;

	const LONG nPacket = InterlockedIncrement(&s_nPackets);
	if (nPacket <= 20 || nPacket % 1000 == 0)
		S30LogFile("[NETCHAN] S2C #%ld seq %u ack %u flags 0x%02X choked %u, %d bytes\n", nPacket, nSeq, nAck, nFlags, nChoked, nSize);

	AcquireSRWLockExclusive(&s_lock);
	s_nInSeq = nSeq;
	ReleaseSRWLockExclusive(&s_lock);
	if (static_cast<int>(nSeq) > *reinterpret_cast<int*>(pChan + NC_IN_SEQ))
		*reinterpret_cast<int*>(pChan + NC_IN_SEQ) = static_cast<int>(nSeq);
	if (s_pNetTime)
		*reinterpret_cast<double*>(pChan + NC_LAST_RECEIVED) = *s_pNetTime;

	Netchan_CallHandler(pChan, 5);
	Netchan_DispatchDataBlock(pChan);
	bool bSubOk = true;
	if (nFlags & 0x01)
		bSubOk = Netchan_ParseSubchannel(pChan, r);
	if (bSubOk && r.BitsLeft() >= 7)
		Netchan_Dispatch(pChan, pData, nSize, r.m_nPos, "unreliable");
	Netchan_CallHandler(pChan, 6);
}

static bool Netchan_IsWakeMarker(const uint8_t* pPacket)
{
	const uint8_t* const pData = *reinterpret_cast<uint8_t* const*>(pPacket + PKT_DATA);
	const int nSize = *reinterpret_cast<const int*>(pPacket + PKT_SIZE);
	return pData && nSize == sizeof(s_wakeMarker) && !memcmp(pData, s_wakeMarker, sizeof(s_wakeMarker));
}

static void h_ProcessPacket(uint8_t* pChan, uint8_t* pPacket)
{
	if (!s_bActive || !S30Signon_IsDestNetadr(pPacket))
	{
		v_ProcessPacket(pChan, pPacket);
		return;
	}
	if (Netchan_IsWakeMarker(pPacket))
	{
		Netchan_CallHandler(pChan, 5);
		Netchan_DispatchDataBlock(pChan);
		Netchan_CallHandler(pChan, 6);
		return;
	}
	Netchan_OnPacket(pChan, pPacket);
}

float S30Netchan_TickInterval(void)
{
	return s_flTickInterval;
}

bool S30Netchan_QueueC2S(const uint8_t* pBits, int nBits, int nSends, int nSpacingMs)
{
	if (!s_bActive || nBits <= 0 || nBits > static_cast<int>(sizeof(s_c2sRing[0].data)) * 8 || nSends <= 0)
		return false;
	AcquireSRWLockExclusive(&s_lock);
	C2SSlot_t* pSlot = nullptr;
	for (int i = 0; i < kC2SRing && !pSlot; ++i)
	{
		if (s_c2sRing[i].nSendsLeft <= 0)
			pSlot = &s_c2sRing[i];
	}
	if (pSlot)
	{
		memcpy(pSlot->data, pBits, (nBits + 7) / 8);
		pSlot->nBits = nBits;
		pSlot->nSendsLeft = nSends;
		pSlot->nSpacingMs = static_cast<ULONGLONG>(nSpacingMs);
		pSlot->nNextDueMs = 0;
		Netchan_FlushLocked("c2s");
	}
	ReleaseSRWLockExclusive(&s_lock);
	return pSlot != nullptr;
}

bool S30Netchan_QueueMove(const uint8_t* pBits, int nBits)
{
	if (!s_bActive || nBits <= 0 || nBits > static_cast<int>(sizeof(s_pendingMove)) * 8)
		return false;
	AcquireSRWLockExclusive(&s_lock);
	// Unsent moves are superseded: each batch carries its backup commands.
	memcpy(s_pendingMove, pBits, (nBits + 7) / 8);
	s_nPendingMoveBits = nBits;
	Netchan_FlushLocked("move");
	ReleaseSRWLockExclusive(&s_lock);
	return true;
}

void S30Netchan_WakeForDataBlock(void)
{
	if (!s_bActive)
		return;
	if (!S30Signon_QueueForEngine(s_wakeMarker, sizeof(s_wakeMarker)))
		S30Log("[NETCHAN] DataBlock wake not queued (engine socket not known yet) -- dispatched with the next packet\n");
}

// C2S rungs follow the engine's own signon walk (S21 bridge Hook_CS_SetSignonState).
static char h_SetSignonState(uint8_t* pCl, int nState, int nSpawn, void* pMsg)
{
	const char bResult = v_SetSignonState(pCl, nState, nSpawn, pMsg);
	if (s_bActive && nState >= 2 && nState <= 8)
	{
		AcquireSRWLockExclusive(&s_lock);
		S30Log("[NETCHAN] engine signon %d (spawn %d, result %d)\n", nState, nSpawn, bResult);
		if (nState >= s_nLastSentSignon)
		{
			Netchan_QueueSignonLocked(nState, nSpawn);
			s_nLastSentSignon = nState;
		}
		ReleaseSRWLockExclusive(&s_lock);
	}
	return bResult;
}

//-----------------------------------------------------------------------------
// Interface
//-----------------------------------------------------------------------------
void S30Netchan_Begin(S30NetchanSend_t pfnSend)
{
	AcquireSRWLockExclusive(&s_lock);
	s_pfnSend = pfnSend;
	s_nInSeq = s_nC2SSeq = 0;
	s_nServerNonce = 0;
	s_bNeedNonceAck = false;
	s_nServerSubSeqRecv = 0;
	s_nSignonSeqSpawn = -1;
	s_nLastSentSignon = -1;
	s_nSignonQHead = s_nSignonQCount = 0;
	s_nEchoState = -1;
	s_nAckTick = -1;
	s_nPendingMoveBits = 0;
	memset(s_c2sRing, 0, sizeof(s_c2sRing));
	S30C2S_Reset();
	s_bPendingUserInfo = false;
	s_nUserInfoSendsLeft = 0;
	s_nLastFlushMs = 0;
	ReleaseSRWLockExclusive(&s_lock);
	Netchan_ResetReliable();
	s_bSubSeqRebased = false;
	s_bSawSendTables = false;
	s_bPdefOk = false;
	s_nSignonSeqKey = s_nSignonSeqMax = -1;
	s_bDeferredSignon3 = false;
	InterlockedExchange(&s_bActive, 1);
	// FCVAR_REPLICATED/CHEAT are taken as tier1's (1<<13, 1<<14); these show them on S30.
	S30Log("[NETCHAN] flags: sv_cheats 0x%X, host_timescale 0x%X\n",
		S30ConCommand_GetFlags("sv_cheats"), S30ConCommand_GetFlags("host_timescale"));
}

void S30Netchan_End(void)
{
	InterlockedExchange(&s_bActive, 0);
}

void S30Netchan_OnConnAccept(void)
{
	AcquireSRWLockExclusive(&s_lock);
	Netchan_QueueSignonLocked(2, 0);
	s_nLastSentSignon = 2;
	s_bPendingUserInfo = false;
	Netchan_FlushLocked("CONNECTED");
	s_bPendingUserInfo = true;
	s_nUserInfoSendsLeft = kUserInfoCopies;
	s_nUserInfoNextMs = 0;
	Netchan_FlushLocked("UserInfo");
	ReleaseSRWLockExclusive(&s_lock);
}

void S30Netchan_Pump(void)
{
	if (!s_bActive)
		return;
	AcquireSRWLockExclusive(&s_lock);
	if (GetTickCount64() - s_nLastFlushMs >= kKeepaliveMs)
		Netchan_FlushLocked("keepalive");
	ReleaseSRWLockExclusive(&s_lock);
}

void S30Netchan_OnEngineSend(void)
{
	if (!s_bActive)
		return;
	AcquireSRWLockExclusive(&s_lock);
	if (GetTickCount64() - s_nLastFlushMs >= kEngineFlushMs)
		Netchan_FlushLocked("engine");
	ReleaseSRWLockExclusive(&s_lock);
}

void S30Netchan_PrintStatus(void)
{
	AcquireSRWLockShared(&s_lock);
	S30Log("netchan %s: S2C packets %ld (stale %ld, bad nonce %ld) last seq %u; C2S %ld packets, seq %u\n",
		s_bActive ? "on" : "off", s_nPackets, s_nStale, s_nBadNonce, s_nInSeq, s_nFlushes, s_nC2SSeq);
	S30Log("  server nonce 0x%08X subAck %u, C2S signon %d (echo %d), messages dispatched %ld, skipped %ld\n",
		s_nServerNonce, s_nServerSubSeqRecv, s_nLastSentSignon, s_nEchoState, s_nDispatched, s_nSkipped);
	S30Log("  S2C signon max %d spawn %d, deferred NEW %d, SendTables seen %d, pdef %d\n",
		s_nSignonSeqMax, s_nSignonSeqKey, s_bDeferredSignon3, s_bSawSendTables, s_bPdefOk);
	ReleaseSRWLockShared(&s_lock);
}

bool S30Netchan_Install(void)
{
	int nMatches = 0;
	uint8_t* const pProcess = S30Pattern_FindUnique(s_pszPatternProcessPacket, &nMatches);
	if (!pProcess)
	{
		S30Log("[NETCHAN] CNetChan::ProcessPacket unresolved (matches=%d) -- netchannel bridge off\n", nMatches);
		return false;
	}
	uint8_t* const pSignon = S30Pattern_FindUnique(s_pszPatternSetSignonState, &nMatches);
	if (!pSignon)
	{
		S30Log("[NETCHAN] CClientState::SetSignonState unresolved (matches=%d) -- netchannel bridge off\n", nMatches);
		return false;
	}
	const uint8_t* const pCall = pProcess + s_nProcessPacketSeekCall;
	v_bfSeek = reinterpret_cast<bf_read_Seek_t>(const_cast<uint8_t*>(pCall + 5 + *reinterpret_cast<const int32_t*>(pCall + 1)));
	const uint8_t* const pMovsd = pProcess + s_nProcessPacketNetTime;
	if (pMovsd[0] == 0xF2 && pMovsd[1] == 0x0F && pMovsd[2] == 0x10 && pMovsd[3] == 0x1D)
		s_pNetTime = reinterpret_cast<const double*>(pMovsd + 8 + *reinterpret_cast<const int32_t*>(pMovsd + 4));
	else
		S30Log("[NETCHAN] net time load not found at ProcessPacket+0x%X -- last-received stays stale\n", s_nProcessPacketNetTime);

	const uint8_t* const pWalk = S30Pattern_FindUnique(s_pszPatternRecvTableWalk, &nMatches);
	if (pWalk)
	{
		s_pRecvTableHead = reinterpret_cast<const uint16_t*>(pWalk + 7 + *reinterpret_cast<const int32_t*>(pWalk + 3));
		s_ppRecvTableList = reinterpret_cast<uint8_t* const*>(pWalk + 20 + *reinterpret_cast<const int32_t*>(pWalk + 16));
	}
	else
		S30Log("[NETCHAN] RecvTable list unresolved (matches=%d) -- SendTables passed as sent\n", nMatches);

	v_OodleCompress = reinterpret_cast<OodleLZ_Compress_t>(S30Pattern_FindUnique(s_pszPatternOodleCompress, &nMatches));
	if (!v_OodleCompress)
		S30Log("[NETCHAN] OodleLZ_Compress unresolved (matches=%d) -- string tables go uncompressed\n", nMatches);

	uint8_t* const pEndClasses = S30Pattern_FindUnique(s_pszPatternEndClasses, &nMatches);
	if (pEndClasses && pEndClasses[s_nEndClassesInterface] == 0x4C && pEndClasses[s_nEndClassesInterface + 2] == 0x3D
		&& pEndClasses[s_nEndClassesHead] == 0x48 && pEndClasses[s_nEndClassesHead + 2] == 0x1D)
	{
		const uint8_t* const pI = pEndClasses + s_nEndClassesInterface;
		const uint8_t* const pH = pEndClasses + s_nEndClassesHead;
		s_ppClientInterface = reinterpret_cast<void* const*>(pI + 7 + *reinterpret_cast<const int32_t*>(pI + 3));
		s_ppClientClassHead = reinterpret_cast<uint8_t**>(const_cast<uint8_t*>(pH + 7 + *reinterpret_cast<const int32_t*>(pH + 3)));
		v_EndClasses = reinterpret_cast<EndClasses_t>(pEndClasses);
	}
	else
		S30Log("[NETCHAN] CL_ParseClassInfo_EndClasses unresolved (matches=%d) -- no stub classes\n", nMatches);

	v_MatchProps = reinterpret_cast<MatchProps_t>(S30Pattern_FindUnique(s_pszPatternMatchProps, &nMatches));
	if (!v_MatchProps)
		S30Log("[NETCHAN] CompareRecvPropToSendProp builder unresolved (matches=%d)\n", nMatches);
	uint8_t* const pPrecache = S30Pattern_FindUnique(s_pszPatternPrecacheWeapon, &nMatches);
	if (pPrecache && pPrecache[s_nPrecacheWeaponTable] == 0x48 && pPrecache[s_nPrecacheWeaponTable + 2] == 0x0D)
	{
		const uint8_t* const p = pPrecache + s_nPrecacheWeaponTable;
		s_ppWeaponNames = reinterpret_cast<void* const*>(p + 7 + *reinterpret_cast<const int32_t*>(p + 3));
		v_PrecacheWeapon = reinterpret_cast<PrecacheWeapon_t>(pPrecache);
	}
	else
		S30Log("[NETCHAN] PrecacheWeapon unresolved (matches=%d)\n", nMatches);
	uint8_t* const pImpact = S30Pattern_FindUnique(s_pszPatternPrecacheImpact, &nMatches);
	if (pImpact && pImpact[s_nPrecacheImpactTable] == 0x48 && pImpact[s_nPrecacheImpactTable + 2] == 0x0D)
	{
		const uint8_t* const p = pImpact + s_nPrecacheImpactTable;
		s_ppImpactNames = reinterpret_cast<void* const*>(p + 7 + *reinterpret_cast<const int32_t*>(p + 3));
		v_PrecacheImpact = reinterpret_cast<PrecacheImpact_t>(pImpact);
	}
	else
		S30Log("[NETCHAN] PrecacheImpactEffectTable unresolved (matches=%d)\n", nMatches);
	v_DecoderDone = reinterpret_cast<DecoderDone_t>(S30Pattern_FindUnique(s_pszPatternDecoderDone, &nMatches));
	if (!v_DecoderDone)
		S30Log("[NETCHAN] decoder finisher unresolved (matches=%d) -- unmatched props stay fatal\n", nMatches);

	uint8_t* ppInsert[8];
	const int nInsert = S30Pattern_FindAll(s_pszPatternListInsert, ppInsert, 8);
	for (int i = 0; i < nInsert && s_ppRecvTableList; ++i)
	{
		const uint8_t* const pLoad = ppInsert[i] + s_nListInsertListLoad;
		if (pLoad[0] == 0x48 && pLoad[1] == 0x8B && pLoad[2] == 0x0D
			&& pLoad + 7 + *reinterpret_cast<const int32_t*>(pLoad + 3) == reinterpret_cast<const uint8_t*>(s_ppRecvTableList))
			v_RecvTableListInsert = reinterpret_cast<RecvTableListInsert_t>(ppInsert[i]);
	}
	if (!v_RecvTableListInsert)
		S30Log("[NETCHAN] RecvTable list insert unresolved (%d candidates) -- no stub RecvTables\n", nInsert);

	// Decoder builder: a mismatch goes on to the next prop instead of failing the decoder.
	if (v_MatchProps && v_DecoderDone)
	{
		uint8_t* const pFail = reinterpret_cast<uint8_t*>(v_MatchProps) + s_nMatchPropsFail;
		if (!memcmp(pFail, s_matchPropsFailBytes, sizeof(s_matchPropsFailBytes)))
		{
			DWORD nOld = 0;
			if (VirtualProtect(pFail, sizeof(s_matchPropsFailBytes), PAGE_EXECUTE_READWRITE, &nOld))
			{
				const int32_t nRel = s_nMatchPropsNext - (s_nMatchPropsFail + 5);
				pFail[0] = 0xE9;
				memcpy(pFail + 1, &nRel, 4);
				pFail[5] = pFail[6] = 0x90;
				VirtualProtect(pFail, sizeof(s_matchPropsFailBytes), nOld, &nOld);
				FlushInstructionCache(GetCurrentProcess(), pFail, sizeof(s_matchPropsFailBytes));
				s_bMatcherLenient = true;
			}
		}
		else
			S30Log("[NETCHAN] decoder builder mismatch exit not as expected -- left strict\n");
	}

	v_ProcessPacket = reinterpret_cast<CNetChan_ProcessPacket_t>(pProcess);
	v_SetSignonState = reinterpret_cast<CClientState_SetSignonState_t>(pSignon);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ProcessPacket), h_ProcessPacket);
	DetourAttach(&reinterpret_cast<PVOID&>(v_SetSignonState), h_SetSignonState);
	if (v_EndClasses)
		DetourAttach(&reinterpret_cast<PVOID&>(v_EndClasses), h_EndClasses);
	if (v_MatchProps)
		DetourAttach(&reinterpret_cast<PVOID&>(v_MatchProps), h_MatchProps);
	if (v_DecoderDone)
		DetourAttach(&reinterpret_cast<PVOID&>(v_DecoderDone), h_DecoderDone);
	if (v_PrecacheWeapon)
		DetourAttach(&reinterpret_cast<PVOID&>(v_PrecacheWeapon), h_PrecacheWeapon);
	if (v_PrecacheImpact)
		DetourAttach(&reinterpret_cast<PVOID&>(v_PrecacheImpact), h_PrecacheImpact);
	const LONG nError = DetourTransactionCommit();
	S30Log("[NETCHAN] ProcessPacket @ 0x%llX, SetSignonState @ 0x%llX %s (commit %ld); Seek @ 0x%llX, net time @ 0x%llX\n",
		static_cast<unsigned long long>(S30Pattern_Rva(pProcess)), static_cast<unsigned long long>(S30Pattern_Rva(pSignon)),
		nError == NO_ERROR ? "hooked" : "FAILED", nError,
		static_cast<unsigned long long>(S30Pattern_Rva(reinterpret_cast<const void*>(v_bfSeek))),
		static_cast<unsigned long long>(s_pNetTime ? S30Pattern_Rva(s_pNetTime) : 0));
	return nError == NO_ERROR;
}
