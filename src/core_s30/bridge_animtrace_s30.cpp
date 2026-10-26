//=============================================================================//
//
// Purpose: first-person animation trace (diagnostic).
//
// S30 has no animation restart counters on the wire (m_nNewSequenceParity,
// m_nResetEventsParity and m_currentFrameBaseAnimating.animSequenceParity are
// gone; the dedi still sends the first two, they land on dummies), so a restart
// can only be seen in the anchor fields. This trace wraps the RecvProps of the
// anchor fields in the first-person proxy and viewmodel tables: each received
// value is logged with the value the client held at that moment, and every
// presented frame the same fields are read back, so a client-side change (the
// client's own prediction) and a wire-side one (a snapshot or a prediction
// restore) show up apart.
//
// The proxy's third argument is the field itself, so no S30 struct offset is
// assumed; the entity is the proxy's second argument.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "bridge_animtrace_s30.h"
#include "sdk_commands_s30.h"
#include "s30_image.h"
#include "s30_log.h"

typedef void (*RecvProxy_t)(const uint8_t* pInfo, uint8_t* pStruct, void* pOut);

static const int RT_NAME = 0x4C8;       // RecvTable name (bridge_netchan)
static const int RP_TYPE = 0x00;
static const int RP_NAME = 0x28;
static const int RP_PROXY = 0x48;
static const int RP_COPY = 0x68;        // RecvProp size
static const int DECODE_VALUE = 32;     // received value in the decode info (bridge_props)
static const uintptr_t kClientStateClock = 0x1E1BD60 + 0xD8; // clock drift managed client clock
static const int kResetEventsTime = 0xD98;     // C_BaseAnimating m_nResetEventsStartTime (bridge_props)

enum AnimField_t
{
	FIELD_SEQUENCE,
	FIELD_START_TIME,
	FIELD_START_CYCLE,
	FIELD_RATE,
	FIELD_WEAPSTATE,   // DT_WeaponX: the dedi's state, ordinals translated for S21
	FIELD_CUSTOM_ACT,  // DT_WeaponX: custom activity (inspect, ...)
	FIELD_PRED_SEQ,    // DT_WeaponX_PredictingClientOnly: the owner's predicted sequence
	FIELD_COUNT
};
// m_animSequence etc. on the wire, m_currentFrameBaseAnimating.animSequence etc. in the datamap.
static const char* const s_pszFieldSuffix[FIELD_COUNT] = { "animSequence", "animStartTime", "animStartCycle", "animPlaybackRate",
	"weapState", "customActivity", "animSequencePredictingClientOnly" };
static const char* const s_pszFieldShort[FIELD_COUNT] = { "seq", "start", "cycle", "rate", "wstate", "cact", "pseq" };

static bool AnimTrace_IsIntField(int nField)
{
	return nField == FIELD_SEQUENCE || nField == FIELD_WEAPSTATE || nField == FIELD_CUSTOM_ACT || nField == FIELD_PRED_SEQ;
}

struct TracedEntity_t
{
	uint8_t* pStruct;
	const char* pszTable;
	void* pField[FIELD_COUNT];
	uint32_t wire[FIELD_COUNT];        // last received raw value
	bool bWire[FIELD_COUNT];
	uint32_t frame[FIELD_COUNT];       // last value read on a frame
	bool bFrame;
	volatile LONG nWireSinceFrame;     // wire writes since the last frame sample
};
static TracedEntity_t s_entities[32];
static int s_nEntities = 0;
static SRWLOCK s_lock = SRWLOCK_INIT;
static volatile LONG s_bEnabled = 1;
static volatile LONG s_nLines = 0;
static const LONG kMaxLines = 60000;

// One wrapped RecvProp per (table, field); the original proxy is kept beside it.
struct WrappedProp_t
{
	uint8_t prop[0x80];
	RecvProxy_t pfnOriginal;
	const char* pszTable;
	int nField;
	bool bFloat;
};
static WrappedProp_t s_wrapped[32];
static int s_nWrapped = 0;

static bool AnimTrace_Log(void)
{
	return s_bEnabled && InterlockedIncrement(&s_nLines) <= kMaxLines;
}

static float AnimTrace_Clock(void)
{
	const uintptr_t nBase = S30Image_Base();
	return nBase ? *reinterpret_cast<const float*>(nBase + kClientStateClock) : 0.0f;
}

// The Int16 stores share their dword with the next field.
static uint32_t AnimTrace_Mask(int nField, uint32_t v)
{
	return AnimTrace_IsIntField(nField) && nField != FIELD_WEAPSTATE ? v & 0xFFFF : v;
}

static void AnimTrace_Format(char* psz, size_t n, int nField, uint32_t v)
{
	if (nField == FIELD_WEAPSTATE)
		snprintf(psz, n, "%d", static_cast<int>(v));
	else if (AnimTrace_IsIntField(nField))
		snprintf(psz, n, "%d", static_cast<int16_t>(v)); // Int16 stores (proxy 0x605AC0)
	else
	{
		float f;
		memcpy(&f, &v, sizeof(f));
		snprintf(psz, n, "%.4f", f);
	}
}

static TracedEntity_t* AnimTrace_Entity(uint8_t* pStruct, const char* pszTable)
{
	for (int i = 0; i < s_nEntities; ++i)
	{
		if (s_entities[i].pStruct == pStruct)
			return &s_entities[i];
	}
	if (s_nEntities >= static_cast<int>(sizeof(s_entities) / sizeof(s_entities[0])))
		return nullptr;
	TracedEntity_t* const e = &s_entities[s_nEntities++];
	memset(e, 0, sizeof(*e));
	e->pStruct = pStruct;
	e->pszTable = pszTable;
	S30LogFile("[ANIM1P] tracking e%d %p (%s)\n", s_nEntities - 1, pStruct, pszTable);
	return e;
}

static void AnimTrace_OnReceive(WrappedProp_t& w, const uint8_t* pInfo, uint8_t* pStruct, void* pOut)
{
	uint32_t nBefore = 0;
	if (pOut)
		memcpy(&nBefore, pOut, sizeof(nBefore));
	nBefore = AnimTrace_Mask(w.nField, nBefore);
	if (w.pfnOriginal)
		w.pfnOriginal(pInfo, pStruct, pOut);
	else if (pOut)
		memcpy(pOut, pInfo + DECODE_VALUE, sizeof(int32_t)); // the decoder's own store for a proxy-less Int
	if (!pOut || !s_bEnabled)
		return;
	uint32_t nValue = 0;
	memcpy(&nValue, pOut, sizeof(nValue));
	nValue = AnimTrace_Mask(w.nField, nValue);

	AcquireSRWLockExclusive(&s_lock);
	TracedEntity_t* const e = AnimTrace_Entity(pStruct, w.pszTable);
	if (e)
	{
		e->pField[w.nField] = pOut;
		const bool bChanged = !e->bWire[w.nField] || e->wire[w.nField] != nValue;
		// A received value equal to the last one but unlike what the client held:
		// the snapshot (or a restore) overwrote a client-side change.
		const bool bOverwrote = nBefore != nValue;
		if ((bChanged || bOverwrote) && AnimTrace_Log())
		{
			char szOld[32], szNew[32], szHeld[32];
			AnimTrace_Format(szOld, sizeof(szOld), w.nField, e->wire[w.nField]);
			AnimTrace_Format(szNew, sizeof(szNew), w.nField, nValue);
			AnimTrace_Format(szHeld, sizeof(szHeld), w.nField, nBefore);
			S30LogFile("[ANIM1P] wire  e%d %-5s %s -> %s (client held %s%s) clock %.4f\n", static_cast<int>(e - s_entities),
				s_pszFieldShort[w.nField], e->bWire[w.nField] ? szOld : "-", szNew, szHeld,
				bOverwrote ? ", OVERWRITTEN" : "", AnimTrace_Clock());
		}
		e->wire[w.nField] = nValue;
		e->bWire[w.nField] = true;
		InterlockedIncrement(&e->nWireSinceFrame);
	}
	ReleaseSRWLockExclusive(&s_lock);
}

template <int N>
static void AnimTrace_Proxy(const uint8_t* pInfo, uint8_t* pStruct, void* pOut)
{
	AnimTrace_OnReceive(s_wrapped[N], pInfo, pStruct, pOut);
}

typedef void (*WrapperFn_t)(const uint8_t*, uint8_t*, void*);
#define ANIMTRACE_P4(n) &AnimTrace_Proxy<n>, &AnimTrace_Proxy<n + 1>, &AnimTrace_Proxy<n + 2>, &AnimTrace_Proxy<n + 3>
static const WrapperFn_t s_pfnWrappers[32] =
{
	ANIMTRACE_P4(0), ANIMTRACE_P4(4), ANIMTRACE_P4(8), ANIMTRACE_P4(12),
	ANIMTRACE_P4(16), ANIMTRACE_P4(20), ANIMTRACE_P4(24), ANIMTRACE_P4(28),
};

static bool AnimTrace_IsTracedTable(const char* pszTable)
{
	return pszTable && (strstr(pszTable, "FirstPersonProxy") || strstr(pszTable, "ViewModel") || !strcmp(pszTable, "DT_WeaponX"));
}

static int AnimTrace_FieldOf(const char* pszName)
{
	if (!pszName)
		return -1;
	const size_t nName = strlen(pszName);
	for (int f = 0; f < FIELD_COUNT; ++f)
	{
		const size_t nSuffix = strlen(s_pszFieldSuffix[f]);
		if (nName >= nSuffix && !strcmp(pszName + nName - nSuffix, s_pszFieldSuffix[f])
			&& (nName == nSuffix || (nName == nSuffix + 2 && !strncmp(pszName, "m_", 2)) || pszName[nName - nSuffix - 1] == '.'))
			return f;
	}
	return -1;
}

void S30AnimTrace_OnDecoder(uint8_t* pDecoder)
{
	uint8_t** const ppProps = *reinterpret_cast<uint8_t***>(pDecoder + 0x4080);
	const uint8_t* const pRoot = *reinterpret_cast<uint8_t* const*>(pDecoder);
	const int nProps = *reinterpret_cast<const int*>(pDecoder + 0x4098);
	const char* const pszTable = pRoot ? *reinterpret_cast<const char* const*>(pRoot + RT_NAME) : nullptr;
	if (!ppProps || !AnimTrace_IsTracedTable(pszTable))
		return;
	for (int i = 0; i < nProps; ++i)
	{
		uint8_t* const pProp = ppProps[i];
		if (!pProp)
			continue;
		const char* const pszName = *reinterpret_cast<const char* const*>(pProp + RP_NAME);
		const int nField = AnimTrace_FieldOf(pszName);
		const RecvProxy_t pfnOriginal = *reinterpret_cast<RecvProxy_t*>(pProp + RP_PROXY);
		// Proxy-less props are wrapped only for m_weapState (a plain 32-bit Int store);
		// the animation fields without one are stored through a redirect this does not know.
		const bool bPlainInt = !pfnOriginal && nField == FIELD_WEAPSTATE && *reinterpret_cast<const int*>(pProp + RP_TYPE) == 0;
		if (nField < 0 || (!pfnOriginal && !bPlainInt) || s_nWrapped >= static_cast<int>(sizeof(s_wrapped) / sizeof(s_wrapped[0])))
			continue;
		WrappedProp_t& w = s_wrapped[s_nWrapped];
		memset(w.prop, 0, sizeof(w.prop));
		memcpy(w.prop, pProp, RP_COPY);
		w.pfnOriginal = pfnOriginal;
		w.pszTable = pszTable;
		w.nField = nField;
		w.bFloat = *reinterpret_cast<const int*>(pProp + RP_TYPE) == 1;
		*reinterpret_cast<const void**>(w.prop + RP_PROXY) = reinterpret_cast<const void*>(s_pfnWrappers[s_nWrapped]);
		ppProps[i] = w.prop;
		++s_nWrapped;
		S30LogFile("[ANIM1P] traced %s.%s (recv type %d)\n", pszTable, pszName, *reinterpret_cast<const int*>(pProp + RP_TYPE));
	}
}

void S30AnimTrace_OnFrame(void)
{
	if (!s_bEnabled || !s_nEntities)
		return;
	AcquireSRWLockExclusive(&s_lock);
	__try
	{
		for (int i = 0; i < s_nEntities; ++i)
		{
			TracedEntity_t& e = s_entities[i];
			uint32_t cur[FIELD_COUNT] = {};
			bool bAny = false;
			for (int f = 0; f < FIELD_COUNT; ++f)
			{
				if (e.pField[f])
				{
					memcpy(&cur[f], e.pField[f], sizeof(cur[f]));
					cur[f] = AnimTrace_Mask(f, cur[f]);
					bAny = true;
				}
			}
			if (!bAny)
				continue;
			const LONG nWire = InterlockedExchange(&e.nWireSinceFrame, 0);
			if (e.bFrame && memcmp(cur, e.frame, sizeof(cur)) && AnimTrace_Log())
			{
				// Equal to the wire's last values: the wire put it there; otherwise the client did.
				bool bAllWire = true;
				for (int f = 0; f < FIELD_COUNT; ++f)
				{
					if (cur[f] != e.frame[f] && (!e.bWire[f] || cur[f] != e.wire[f]))
						bAllWire = false;
				}
				char sz[FIELD_COUNT][32];
				for (int f = 0; f < FIELD_COUNT; ++f)
					AnimTrace_Format(sz[f], sizeof(sz[f]), f, cur[f]);
				char szChanged[64] = "";
				for (int f = 0; f < FIELD_COUNT; ++f)
				{
					if (cur[f] != e.frame[f])
					{
						strncat_s(szChanged, s_pszFieldShort[f], _TRUNCATE);
						strncat_s(szChanged, " ", _TRUNCATE);
					}
				}
				S30LogFile("[ANIM1P] frame e%d seq %s start %s cycle %s rate %s wstate %s cact %s pseq %s reset %.4f | changed %s| by %s | clock %.4f\n",
					i, sz[FIELD_SEQUENCE], sz[FIELD_START_TIME], sz[FIELD_START_CYCLE], sz[FIELD_RATE], sz[FIELD_WEAPSTATE], sz[FIELD_CUSTOM_ACT], sz[FIELD_PRED_SEQ],
					*reinterpret_cast<const float*>(e.pStruct + kResetEventsTime), szChanged,
					nWire && bAllWire ? "wire" : nWire ? "wire+client" : "client", AnimTrace_Clock());
			}
			memcpy(e.frame, cur, sizeof(cur));
			e.bFrame = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		// A freed entity: start over on the next receive.
		s_nEntities = 0;
	}
	ReleaseSRWLockExclusive(&s_lock);
}

static void Cmd_Anim1p(int nArgs, const char* const* ppszArgs)
{
	if (nArgs >= 2)
	{
		InterlockedExchange(&s_bEnabled, atoi(ppszArgs[1]) != 0);
		InterlockedExchange(&s_nLines, 0);
	}
	S30Log("[ANIM1P] trace %s, %d entities, %d props wrapped\n", s_bEnabled ? "on" : "off", s_nEntities, s_nWrapped);
}

void S30AnimTrace_RegisterCommands(void)
{
	S30Commands_Register("sdk_anim1p", "sdk_anim1p [0|1] -- first-person animation trace (wire vs client, per frame)", Cmd_Anim1p);
}
