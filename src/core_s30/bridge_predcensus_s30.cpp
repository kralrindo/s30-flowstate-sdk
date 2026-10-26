//=============================================================================//
//
// Purpose: prediction census (diagnostic).
//
// The S21 bridge's [PRED-UNFED] fix started from a census: fields the client
// predicts that the dedi sends as a constant 0 make every ack look like an error,
// and the restore installs the 0 over the client's value. This wraps the props of
// the DT_Player decoder (its flat list includes DT_LocalPlayerExclusive) and, per
// prop, counts the receives whose value differs from the one the client held and
// those where the wire sent 0 against a non-zero held value. Every 5 s the props
// with the most mismatches are logged.
//
// A received value is compared before it is stored. Props with their own proxy
// keep it; proxy-less props are wrapped only for 4-byte types (Int, Float, Time),
// stored as the decoder would.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include "bridge_predcensus_s30.h"
#include "sdk_commands_s30.h"
#include "s30_log.h"

typedef void (*RecvProxy_t)(const uint8_t* pInfo, uint8_t* pStruct, void* pOut);

static const int RT_NAME = 0x4C8;
static const int RP_TYPE = 0x00;
static const int RP_NAME = 0x28;
static const int RP_PROXY = 0x48;
static const int RP_COPY = 0x68;
static const int DECODE_VALUE = 32;

// Byte / Int16 stores (bridge_animtrace, the S30 RecvTable dump).
static const uintptr_t kProxyByte = 0x605AB0;
static const uintptr_t kProxyInt16 = 0x605AC0;

static const int kMaxWrapped = 768;

struct CensusProp_t
{
	uint8_t prop[0x80];
	RecvProxy_t pfnOriginal;
	const char* pszName;
	int nType;
	int nBytes;          // compared bytes
	volatile LONG nRecv;
	volatile LONG nDiff;
	volatile LONG nWireZero; // wire 0, client held non-zero
	uint32_t nLastWire, nLastHeld;
};
static CensusProp_t s_props[kMaxWrapped];
static int s_nProps = 0;
static volatile LONG s_bEnabled = 1;
static ULONGLONG s_nNextReportMs = 0;

static uintptr_t Census_ImageBase(void)
{
	static uintptr_t s_nBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
	return s_nBase;
}

static void Census_Report(void)
{
	int order[kMaxWrapped];
	int n = 0;
	for (int i = 0; i < s_nProps; ++i)
	{
		if (s_props[i].nDiff)
			order[n++] = i;
	}
	// Most mismatches first.
	for (int i = 1; i < n; ++i)
	{
		const int v = order[i];
		int j = i - 1;
		while (j >= 0 && s_props[order[j]].nDiff < s_props[v].nDiff)
		{
			order[j + 1] = order[j];
			--j;
		}
		order[j + 1] = v;
	}
	S30LogFile("[PREDCENSUS] %d props wrapped, %d with mismatches:\n", s_nProps, n);
	for (int k = 0; k < n && k < 40; ++k)
	{
		const CensusProp_t& p = s_props[order[k]];
		float flWire, flHeld;
		memcpy(&flWire, &p.nLastWire, 4);
		memcpy(&flHeld, &p.nLastHeld, 4);
		S30LogFile("[PREDCENSUS]   %-48s type %d: %ld of %ld differ, %ld wire-0 | last wire 0x%08X (%g) held 0x%08X (%g)\n",
			p.pszName, p.nType, p.nDiff, p.nRecv, p.nWireZero, p.nLastWire, flWire, p.nLastHeld, flHeld);
	}
}

static void Census_OnReceive(CensusProp_t& p, const uint8_t* pInfo, uint8_t* pStruct, void* pOut)
{
	uint32_t nHeld = 0, nWire = 0;
	if (pOut && s_bEnabled)
		memcpy(&nHeld, pOut, p.nBytes);
	if (p.pfnOriginal)
		p.pfnOriginal(pInfo, pStruct, pOut);
	else if (pOut)
		memcpy(pOut, pInfo + DECODE_VALUE, sizeof(int32_t));
	if (!pOut || !s_bEnabled)
		return;
	memcpy(&nWire, pOut, p.nBytes);
	InterlockedIncrement(&p.nRecv);
	if (nWire != nHeld)
	{
		InterlockedIncrement(&p.nDiff);
		if (!nWire)
			InterlockedIncrement(&p.nWireZero);
		p.nLastWire = nWire;
		p.nLastHeld = nHeld;
	}
	const ULONGLONG nNow = GetTickCount64();
	if (nNow >= s_nNextReportMs)
	{
		const bool bFirst = !s_nNextReportMs;
		s_nNextReportMs = nNow + 5000;
		if (!bFirst)
			Census_Report();
	}
}

template <int N>
static void Census_Proxy(const uint8_t* pInfo, uint8_t* pStruct, void* pOut)
{
	Census_OnReceive(s_props[N], pInfo, pStruct, pOut);
}

template <int... I>
static constexpr RecvProxy_t Census_ProxyAt(int n, std::integer_sequence<int, I...>)
{
	constexpr RecvProxy_t table[] = { &Census_Proxy<I>... };
	return table[n];
}

static RecvProxy_t Census_Wrapper(int n)
{
	static const RecvProxy_t* s_pTable = nullptr;
	static RecvProxy_t s_table[kMaxWrapped];
	if (!s_pTable)
	{
		for (int i = 0; i < kMaxWrapped; ++i)
			s_table[i] = Census_ProxyAt(i, std::make_integer_sequence<int, kMaxWrapped>{});
		s_pTable = s_table;
	}
	return s_pTable[n];
}

void S30PredCensus_OnDecoder(uint8_t* pDecoder)
{
	uint8_t** const ppProps = *reinterpret_cast<uint8_t***>(pDecoder + 0x4080);
	const uint8_t* const pRoot = *reinterpret_cast<uint8_t* const*>(pDecoder);
	const int nProps = *reinterpret_cast<const int*>(pDecoder + 0x4098);
	const char* const pszTable = pRoot ? *reinterpret_cast<const char* const*>(pRoot + RT_NAME) : nullptr;
	if (!ppProps || !pszTable || strcmp(pszTable, "DT_Player"))
		return;
	const uintptr_t nBase = Census_ImageBase();
	int nWrapped = 0;
	for (int i = 0; i < nProps && s_nProps < kMaxWrapped; ++i)
	{
		uint8_t* const pProp = ppProps[i];
		if (!pProp)
			continue;
		const int nType = *reinterpret_cast<const int*>(pProp + RP_TYPE);
		const RecvProxy_t pfnOriginal = *reinterpret_cast<RecvProxy_t*>(pProp + RP_PROXY);
		const char* const pszName = *reinterpret_cast<const char* const*>(pProp + RP_NAME);
		if (!pszName || pszName[0] == '?')
			continue; // bridge dummies and adapters
		int nBytes = 0;
		const uintptr_t nProxy = reinterpret_cast<uintptr_t>(pfnOriginal) - nBase;
		if (nProxy == kProxyByte)
			nBytes = 1;
		else if (nProxy == kProxyInt16)
			nBytes = 2;
		else if (nType == 0 || nType == 1 || nType == 9)
			nBytes = 4;
		if (!nBytes || (!pfnOriginal && nType != 0 && nType != 1 && nType != 9))
			continue;
		CensusProp_t& p = s_props[s_nProps];
		memset(p.prop, 0, sizeof(p.prop));
		memcpy(p.prop, pProp, RP_COPY);
		p.pfnOriginal = pfnOriginal;
		p.pszName = pszName;
		p.nType = nType;
		p.nBytes = nBytes;
		*reinterpret_cast<RecvProxy_t*>(p.prop + RP_PROXY) = Census_Wrapper(s_nProps);
		ppProps[i] = p.prop;
		++s_nProps;
		++nWrapped;
	}
	S30LogFile("[PREDCENSUS] %s: %d of %d props wrapped\n", pszTable, nWrapped, nProps);
}

static void Cmd_PredCensus(int nArgs, const char* const* ppszArgs)
{
	if (nArgs >= 2 && !strcmp(ppszArgs[1], "reset"))
	{
		for (int i = 0; i < s_nProps; ++i)
		{
			s_props[i].nRecv = s_props[i].nDiff = s_props[i].nWireZero = 0;
		}
	}
	else if (nArgs >= 2)
		InterlockedExchange(&s_bEnabled, atoi(ppszArgs[1]) != 0);
	Census_Report();
}

void S30PredCensus_RegisterCommands(void)
{
	S30Commands_Register("sdk_predcensus", "sdk_predcensus [0|1|reset] -- player prop wire-vs-client mismatch census", Cmd_PredCensus);
}
