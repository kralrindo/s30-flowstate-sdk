//=============================================================================//
//
// Purpose: the engine's clc_Move to the S3 dedi.
//
// S30.1 facts (s30/docs/12, verified in the S30 IDB):
//   0x2F5320  ClWriteMoveCmds (the S21 SDK's signature): builds a clc_Move on
//             its stack (vtable from the lea at +0x5D: 0x17F0430) -- backup
//             count +0x20 (<= 7), new count +0x24 (<= 15), bf_write at +0x70
//             (data +0x70, current bit +0x80) -- fills it through the client's
//             usercmd delta writer, the first command against a reset command,
//             and sends it with CNetChan::SendNetMsg 0x2A03D0 (chan, msg, reliable, voice).
//   0xB9E820  WriteUsercmd(buf, to, from): S21's field order (the S21 bridge's
//             S21Bridge_ParseS21Cmd) except: no view spring roll after the two
//             spring floats, no impulse before weaponSelect, weaponCustomActivity
//             12 bits (S21 11), the two vector groups after the camera override
//             are nested (the second only written when the first is), a third
//             vector group follows them, and a bit sits between the bullet-trace
//             placeholder and frametime. 0xB9DD00 is S21's normalized-float delta,
//             0xB9DC80 S21's signed delta (N-bit two's complement), 0xB9E560 the
//             reset command (S21's ResetToNullCmd values).
// The dedi side is S21Bridge_EmitS3Cmd with the S21 bridge's default levers.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "thirdparty/detours/include/detours.h"
#include "bridge_usercmd_s30.h"
#include "bridge_c2s_s30.h"
#include "bridge_netchan_s30.h"
#include "bridge_bits_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternWriteMoveCmds = "48 89 5C 24 ?? 48 89 6C 24 ?? 56 41 56 41 57 B8";
static const char* const s_pszPatternSendNetMsg =
	"48 89 54 24 10 55 53 56 57 41 55 41 56 41 57 48 8D AC 24 ?? ?? ?? ?? B8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 2B E0 83 B9 ?? ?? ?? ?? 00";
static const int s_nMoveVtableLea = 0x5D; // lea rax, [rip+vtable] in ClWriteMoveCmds (7 bytes)

// clc_Move (S30.1)
static const int MOVE_BACKUP = 0x20;
static const int MOVE_NEW = 0x24;
static const int MOVE_DATA = 0x70;
static const int MOVE_CURBIT = 0x80;

static const int kS3ClcMove = 46;

typedef bool (*SendNetMsg_t)(void* pChan, void* pMsg, bool bReliable, bool bVoice);
static SendNetMsg_t v_SendNetMsg = nullptr;
static const void* s_pMoveVtable = nullptr;

//-----------------------------------------------------------------------------
// User command state (the S21 bridge's S21BridgeCmd::State)
//-----------------------------------------------------------------------------
struct UserCmd_t
{
	uint32_t commandNumber;
	float snapshotInterpAcc;
	float commandTime;
	float viewAngles[3];
	float viewSpringCorr[2];
	float forwardmove, sidemove, upmove;
	float leftTrigger, rightTrigger;
	uint32_t buttons;
	int32_t weaponSelect;
	int32_t weaponSelectType;
	uint32_t realtimeWeaponMod;
	uint8_t weaponToggleAkimbo;
	int32_t weaponCustomActivity;
	uint32_t meleetarget;
	uint8_t controllerMode, vehicleCameraControls, updateCycleWeapon;
	uint8_t hasWrittenAngle[3];
	uint8_t startEnergize;
	bool ziplinePresent;
	uint32_t ziplineHandle;
	uint32_t ziplineFloats[13];
	struct Ping_t
	{
		bool bPresent;
		uint32_t nType;
		uint32_t longs[4];
		int nCoordBit, nCoordBits; // BitVec3Coord, copied as is from the source batch
	} pings[4];
	uint8_t respawnInputDebounce, queuePrimaryAttack;
	bool useCameraOverride;
	float cameraFloats[6];
	bool hasThirdPersonAttackFocus;
	float thirdPersonFocus[3];
	bool hasKnockBack;
	float knockBackPos[3];
	bool hasExtraVector; // S30's third vector group: no S3 field
	float extraVector[3];
	uint8_t skydiveUnfollow;
	uint32_t baseSnapshotTickCount;
	uint32_t predictedServerEventAck;
	uint8_t extraFlag; // S30's bit before frametime: no S3 field
	float frametime;
};

// CUserCmd::Reset (S30 0xB9E560 == the S21 bridge's ResetToNullCmd).
static void UserCmd_Reset(UserCmd_t& s)
{
	memset(&s, 0, sizeof(s));
	s.commandNumber = 0xFFFFFFFFu;
	s.weaponSelect = -1;
	s.weaponSelectType = -1;
	s.realtimeWeaponMod = 0xFF00u;
	s.weaponCustomActivity = -1;
	s.meleetarget = 0xFFFFFFFFu;
}

static int32_t SignExtend(uint32_t v, int nBits)
{
	return static_cast<int32_t>(v << (32 - nBits)) >> (32 - nBits);
}

static float AsFloat(uint32_t v)
{
	float f;
	memcpy(&f, &v, sizeof(f));
	return f;
}

static uint32_t AsBits(float f)
{
	uint32_t v;
	memcpy(&v, &f, sizeof(v));
	return v;
}

static uint32_t ParseDeltaU(S30BitReader_t& r, int nBits, uint32_t prev) { return r.Read(1) ? r.Read(nBits) : prev; }
static int32_t ParseDeltaS(S30BitReader_t& r, int nBits, int32_t prev) { return r.Read(1) ? SignExtend(r.Read(nBits), nBits) : prev; }
static float ParseDeltaFloat(S30BitReader_t& r, float prev) { return r.Read(1) ? AsFloat(r.Read(32)) : prev; }
static uint32_t ParseDeltaU32Inc(S30BitReader_t& r, uint32_t prev) { return r.Read(1) ? r.Read(32) : prev + 1; }

static float ParseDeltaNormalizedFloat(S30BitReader_t& r, float prev)
{
	if (!r.Read(1))
		return prev;
	if (r.Read(1))
	{
		if (!r.Read(1))
			return 0.0f;
		return r.Read(1) ? -1.0f : 1.0f;
	}
	return AsFloat(r.Read(32));
}

static uint32_t ParseTernaryU32(S30BitReader_t& r, uint32_t prev)
{
	if (!r.Read(1))
		return prev;
	if (!r.Read(1))
		return prev + 1;
	return r.Read(32);
}

static void SkipBitCoord(S30BitReader_t& r)
{
	const uint32_t bInt = r.Read(1), bFract = r.Read(1);
	if (!bInt && !bFract)
		return;
	r.Skip(1 + (bInt ? 16 : 0) + (bFract ? 5 : 0)); // sign, COORD_INTEGER_BITS, COORD_FRACTIONAL_BITS
}

static void ParseDeltaVec3(S30BitReader_t& r, float* pCur, const float* pPrev)
{
	for (int k = 0; k < 3; ++k)
		pCur[k] = ParseDeltaFloat(r, pPrev[k]);
}

// One command in S30's WriteUsercmd order (0xB9E820).
static bool UserCmd_ParseS30(S30BitReader_t& r, UserCmd_t& cur, const UserCmd_t& prev)
{
	cur = prev;
	cur.commandNumber = ParseDeltaU32Inc(r, prev.commandNumber);
	cur.snapshotInterpAcc = ParseDeltaFloat(r, prev.snapshotInterpAcc);
	ParseDeltaVec3(r, cur.viewAngles, prev.viewAngles);
	cur.viewSpringCorr[0] = ParseDeltaFloat(r, prev.viewSpringCorr[0]);
	cur.viewSpringCorr[1] = ParseDeltaFloat(r, prev.viewSpringCorr[1]);
	cur.forwardmove = ParseDeltaNormalizedFloat(r, prev.forwardmove);
	cur.sidemove = ParseDeltaNormalizedFloat(r, prev.sidemove);
	cur.upmove = ParseDeltaFloat(r, prev.upmove);
	cur.leftTrigger = ParseDeltaNormalizedFloat(r, prev.leftTrigger);
	cur.rightTrigger = ParseDeltaNormalizedFloat(r, prev.rightTrigger);
	cur.buttons = ParseDeltaU(r, 32, prev.buttons);
	cur.weaponSelect = ParseDeltaS(r, 5, prev.weaponSelect);
	cur.weaponSelectType = ParseDeltaS(r, 3, prev.weaponSelectType);
	cur.realtimeWeaponMod = ParseDeltaU(r, 16, prev.realtimeWeaponMod);
	cur.weaponToggleAkimbo = static_cast<uint8_t>(r.Read(1));
	cur.weaponCustomActivity = ParseDeltaS(r, 12, prev.weaponCustomActivity);
	cur.meleetarget = ParseDeltaU(r, 32, prev.meleetarget);
	cur.controllerMode = static_cast<uint8_t>(r.Read(1));
	cur.vehicleCameraControls = static_cast<uint8_t>(r.Read(1));
	cur.updateCycleWeapon = static_cast<uint8_t>(r.Read(1));
	for (int i = 0; i < 3; ++i)
		cur.hasWrittenAngle[i] = static_cast<uint8_t>(ParseDeltaU(r, 8, prev.hasWrittenAngle[i]));
	cur.startEnergize = static_cast<uint8_t>(r.Read(1));
	cur.ziplinePresent = r.Read(1) != 0;
	if (cur.ziplinePresent)
	{
		cur.ziplineHandle = r.Read(32);
		for (int k = 0; k < 13; ++k)
			cur.ziplineFloats[k] = r.Read(32);
	}
	if (r.Read(1)) // object placement: handle, then an optional pose (dropped, as the S21 bridge does)
	{
		r.Skip(32);
		if (r.Read(1))
			r.Skip(6 * 32 + 32);
	}
	for (int j = 0; j < 4; ++j)
	{
		UserCmd_t::Ping_t& p = cur.pings[j];
		p.bPresent = r.Read(1) != 0;
		if (!p.bPresent)
			continue;
		p.nType = r.Read(3);
		for (int k = 0; k < 4; ++k)
			p.longs[k] = r.Read(32); // 31 bits + sign: the raw 32-bit pattern
		p.nCoordBit = r.m_nPos;
		const uint32_t bX = r.Read(1), bY = r.Read(1), bZ = r.Read(1);
		if (bX)
			SkipBitCoord(r);
		if (bY)
			SkipBitCoord(r);
		if (bZ)
			SkipBitCoord(r);
		p.nCoordBits = r.m_nPos - p.nCoordBit;
	}
	cur.respawnInputDebounce = static_cast<uint8_t>(r.Read(1));
	cur.queuePrimaryAttack = static_cast<uint8_t>(r.Read(1));
	cur.useCameraOverride = r.Read(1) != 0;
	if (cur.useCameraOverride)
	{
		for (int k = 0; k < 6; ++k)
			cur.cameraFloats[k] = ParseDeltaFloat(r, prev.cameraFloats[k]);
	}
	cur.hasThirdPersonAttackFocus = r.Read(1) != 0;
	cur.hasKnockBack = false;
	if (cur.hasThirdPersonAttackFocus)
	{
		ParseDeltaVec3(r, cur.thirdPersonFocus, prev.thirdPersonFocus);
		cur.hasKnockBack = r.Read(1) != 0;
		if (cur.hasKnockBack)
			ParseDeltaVec3(r, cur.knockBackPos, prev.knockBackPos);
	}
	cur.hasExtraVector = r.Read(1) != 0;
	if (cur.hasExtraVector)
		ParseDeltaVec3(r, cur.extraVector, prev.extraVector);
	cur.skydiveUnfollow = static_cast<uint8_t>(r.Read(1));
	cur.baseSnapshotTickCount = ParseTernaryU32(r, prev.baseSnapshotTickCount);
	cur.predictedServerEventAck = ParseDeltaU(r, 32, prev.predictedServerEventAck);
	r.Read(1); // bullet trace test data: always 0
	cur.extraFlag = static_cast<uint8_t>(r.Read(1));
	cur.frametime = ParseDeltaFloat(r, prev.frametime);
	return !r.m_bOverflow;
}

//-----------------------------------------------------------------------------
// S3 emit (S21Bridge_EmitS3Cmd, default levers)
//-----------------------------------------------------------------------------
static float ClampFinite(float v, float flMax)
{
	return (!(v == v) || v < -flMax || v > flMax) ? 0.0f : v;
}

static void EmitDeltaU(S30BitWriter_t& w, uint32_t cur, int nBits, uint32_t prev)
{
	if (cur == prev)
		w.Write(0, 1);
	else
	{
		w.Write(1, 1);
		w.Write(cur, nBits);
	}
}

static void EmitDeltaS(S30BitWriter_t& w, int32_t cur, int nBits, int32_t prev)
{
	if (cur == prev)
		w.Write(0, 1);
	else
	{
		w.Write(1, 1);
		w.Write(static_cast<uint32_t>(cur) & ((1u << nBits) - 1), nBits); // WriteSBitLong: N-bit two's complement
	}
}

static void EmitDeltaFloat(S30BitWriter_t& w, float cur, float prev)
{
	EmitDeltaU(w, AsBits(cur), 32, AsBits(prev));
}

static void EmitTernaryU32(S30BitWriter_t& w, uint32_t cur, uint32_t prev)
{
	if (cur == prev)
		w.Write(0, 1);
	else if (cur == prev + 1)
	{
		w.Write(1, 1);
		w.Write(0, 1);
	}
	else
	{
		w.Write(3, 2);
		w.Write(cur, 32);
	}
}

static uint8_t s_nPrevImpulseWire = 0;
static const uint8_t kImpulseStartEnergize = 0x80; // the S21 bridge's kS21ExtraFlag_StartEnergize
static const uint8_t kImpulseToggleAkimbo = 0x40;  // kS21ExtraFlag_ToggleAkimbo

static void UserCmd_EmitS3(S30BitWriter_t& w, const UserCmd_t& cur, const UserCmd_t& prev, const uint8_t* pSource, int nSourceBytes)
{
	const float kMax = 1.0e9f, kFocusMax = 100000.0f;
	EmitDeltaU(w, cur.commandNumber, 32, prev.commandNumber + 1); // 1+0/32, default prev+1
	// tick_count: the real tick, a jump vs the previous command clamped to 256 ticks.
	uint32_t nTick = cur.baseSnapshotTickCount;
	if (prev.baseSnapshotTickCount)
	{
		const int64_t d = static_cast<int64_t>(nTick) - prev.baseSnapshotTickCount;
		if (d > 256)
			nTick = prev.baseSnapshotTickCount + 256;
		else if (d < -256)
			nTick = prev.baseSnapshotTickCount - 256;
	}
	w.Write(1, 1);
	w.Write(nTick, 32);
	if (cur.commandTime > 0.0f)
		EmitDeltaFloat(w, ClampFinite(cur.commandTime, 86400.0f), ClampFinite(prev.commandTime, 86400.0f));
	else
		w.Write(0, 1);
	for (int k = 0; k < 3; ++k)
		EmitDeltaFloat(w, ClampFinite(cur.viewAngles[k], kMax), ClampFinite(prev.viewAngles[k], kMax));
	EmitDeltaFloat(w, cur.viewSpringCorr[0], prev.viewSpringCorr[0]);
	EmitDeltaFloat(w, cur.viewSpringCorr[1], prev.viewSpringCorr[1]);
	w.Write(0, 1); // pitchangles.z: S30 has no spring roll
	EmitDeltaFloat(w, ClampFinite(cur.forwardmove, kMax), ClampFinite(prev.forwardmove, kMax));
	EmitDeltaFloat(w, ClampFinite(cur.sidemove, kMax), ClampFinite(prev.sidemove, kMax));
	EmitDeltaFloat(w, ClampFinite(cur.upmove, kMax), ClampFinite(prev.upmove, kMax));
	EmitDeltaU(w, cur.buttons, 32, prev.buttons);
	// impulse: S30 has none; startEnergize and toggle-akimbo ride in its top bits (S21 bridge)
	uint8_t nImpulse = 0;
	if (cur.startEnergize)
		nImpulse |= kImpulseStartEnergize;
	if (cur.weaponToggleAkimbo)
		nImpulse |= kImpulseToggleAkimbo;
	EmitDeltaU(w, nImpulse, 8, s_nPrevImpulseWire);
	s_nPrevImpulseWire = nImpulse;
	const bool bSelectEvent = cur.updateCycleWeapon && cur.weaponSelect >= 0 && cur.weaponSelectType >= 0;
	const bool bEmitSelect = cur.weaponSelect != prev.weaponSelect || bSelectEvent;
	if (bEmitSelect)
	{
		w.Write(1, 1);
		w.Write(static_cast<uint32_t>(cur.weaponSelect) & 0x1F, 5);
		w.Write(1, 1);
		w.Write(static_cast<uint32_t>(cur.weaponSelectType) & 0x7, 3);
	}
	else
	{
		w.Write(0, 1);
		EmitDeltaS(w, cur.weaponSelectType, 3, prev.weaponSelectType);
	}
	EmitDeltaU(w, cur.realtimeWeaponMod, 16, prev.realtimeWeaponMod);
	EmitDeltaS(w, cur.weaponCustomActivity, 11, prev.weaponCustomActivity); // S30 12 bits -> S3 11
	EmitDeltaU(w, cur.meleetarget, 32, prev.meleetarget);
	w.Write(cur.controllerMode ? 1 : 0, 1);
	w.Write(cur.vehicleCameraControls ? 1 : 0, 1);
	w.Write(cur.updateCycleWeapon ? 1 : 0, 1);
	w.Write(0, 36); // dedi-only gated block (+0x48..+0xD7): 2 x 18 gates, never set
	w.Write(cur.ziplinePresent ? 1 : 0, 1);
	if (cur.ziplinePresent)
	{
		w.Write(cur.ziplineHandle, 32);
		for (int k = 0; k < 13; ++k)
			w.Write(AsBits(ClampFinite(AsFloat(cur.ziplineFloats[k]), kMax)), 32);
	}
	for (int j = 0; j < 4; ++j)
	{
		const UserCmd_t::Ping_t& p = cur.pings[j];
		w.Write(p.bPresent ? 1 : 0, 1);
		if (!p.bPresent)
			continue;
		w.Write(p.nType > 4 ? 0 : p.nType, 32); // commandType: the dispatch key
		w.Write(p.longs[0], 32);                // pingType
		w.Write(p.longs[1], 32);                // entityHandle
		w.Write(0, 32);                         // userTicketId: server-assigned
		S30BitReader_t coord(pSource, nSourceBytes, p.nCoordBit);
		w.WriteFrom(coord, p.nCoordBits);
	}
	w.Write(cur.respawnInputDebounce ? 1 : 0, 1);
	w.Write(cur.queuePrimaryAttack ? 1 : 0, 1);
	w.Write(cur.useCameraOverride ? 1 : 0, 1);
	if (cur.useCameraOverride)
	{
		for (int k = 0; k < 6; ++k)
			EmitDeltaFloat(w, ClampFinite(cur.cameraFloats[k], kFocusMax), ClampFinite(prev.cameraFloats[k], kFocusMax));
	}
	w.Write(cur.hasThirdPersonAttackFocus ? 1 : 0, 1);
	if (cur.hasThirdPersonAttackFocus)
	{
		for (int k = 0; k < 3; ++k)
			EmitDeltaFloat(w, ClampFinite(cur.thirdPersonFocus[k], kFocusMax), ClampFinite(prev.thirdPersonFocus[k], kFocusMax));
	}
	w.Write(cur.hasKnockBack ? 1 : 0, 1);
	if (cur.hasKnockBack)
	{
		for (int k = 0; k < 3; ++k)
			EmitDeltaFloat(w, ClampFinite(cur.knockBackPos[k], kFocusMax), ClampFinite(prev.knockBackPos[k], kFocusMax));
	}
	w.Write(cur.skydiveUnfollow ? 1 : 0, 1);
	w.Write(0, 1); // dedi-only bool (the S21 bridge's placement pose trailer flag, off)
	EmitTernaryU32(w, cur.baseSnapshotTickCount, prev.baseSnapshotTickCount);
	w.Write(0, 1); // dedi-only ternary: no change
	EmitDeltaU(w, cur.predictedServerEventAck, 32, prev.predictedServerEventAck);
	w.Write(1, 1); // frametime follows
	EmitDeltaFloat(w, cur.frametime, prev.frametime);
}

//-----------------------------------------------------------------------------
// clc_Move capture
//-----------------------------------------------------------------------------
static float s_flFrametimeEstimate = 0.0f;
static uint32_t s_nEstimateCmd = 0;
static double s_flEstimateWall = 0.0;

static double Usercmd_Now(void)
{
	static LARGE_INTEGER s_freq = {};
	if (!s_freq.QuadPart)
		QueryPerformanceFrequency(&s_freq);
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return static_cast<double>(now.QuadPart) / static_cast<double>(s_freq.QuadPart);
}

// The S21 bridge's TransformOneUsercmd extras: frametime restamp, command time.
static void Usercmd_Finish(UserCmd_t& cur)
{
	if (cur.commandNumber > s_nEstimateCmd)
	{
		const double flNow = Usercmd_Now();
		const uint32_t nCmds = cur.commandNumber - s_nEstimateCmd;
		if (s_flEstimateWall > 0.0 && nCmds <= 64)
		{
			const double dt = flNow - s_flEstimateWall;
			if (dt > 0.0 && dt < 0.5)
			{
				const float flPerCmd = static_cast<float>(dt / nCmds);
				s_flFrametimeEstimate = s_flFrametimeEstimate > 0.0f ? 0.9f * s_flFrametimeEstimate + 0.1f * flPerCmd : flPerCmd;
			}
		}
		s_nEstimateCmd = cur.commandNumber;
		s_flEstimateWall = flNow;
	}
	if (cur.frametime <= 0.0f)
	{
		float ft = s_flFrametimeEstimate > 0.0f ? s_flFrametimeEstimate : 0.00714f;
		cur.frametime = ft < 0.001f ? 0.001f : ft > 0.05f ? 0.05f : ft;
	}
	const float flInterval = S30Netchan_TickInterval();
	if (cur.baseSnapshotTickCount && flInterval > 0.0f)
	{
		const float flAcc = cur.snapshotInterpAcc > -1.0f && cur.snapshotInterpAcc < 2.0f ? cur.snapshotInterpAcc : 0.0f;
		cur.commandTime = (static_cast<float>(cur.baseSnapshotTickCount) + flAcc) * flInterval;
		if (!(cur.commandTime > 0.0f) || cur.commandTime > 86400.0f)
			cur.commandTime = 0.0f;
	}
}

static void Usercmd_OnMove(const uint8_t* pMsg)
{
	const int nBackup = *reinterpret_cast<const int*>(pMsg + MOVE_BACKUP);
	const int nNew = *reinterpret_cast<const int*>(pMsg + MOVE_NEW);
	const uint8_t* const pData = *reinterpret_cast<const uint8_t* const*>(pMsg + MOVE_DATA);
	const int nBits = *reinterpret_cast<const int*>(pMsg + MOVE_CURBIT);
	const int nCmds = nBackup + nNew;
	if (!pData || nBackup < 0 || nBackup > 7 || nNew < 0 || nNew > 15 || nCmds <= 0 || nBits <= 0 || nBits > 64000)
	{
		static volatile LONG s_nBad = 0;
		if (InterlockedIncrement(&s_nBad) <= 5)
			S30Log("[USERCMD] clc_Move header out of range: backup %d new %d, %d bits\n", nBackup, nNew, nBits);
		return;
	}
	const int nSourceBytes = (nBits + 7) / 8;
	S30BitReader_t r(pData, nSourceBytes);
	static uint8_t s_body[2048];
	S30BitWriter_t body(s_body, sizeof(s_body));
	UserCmd_t prev, cur;
	UserCmd_Reset(prev);
	UserCmd_Reset(cur);
	s_nPrevImpulseWire = 0;
	int nDone = 0;
	for (int i = 0; i < nCmds; ++i)
	{
		if (!UserCmd_ParseS30(r, cur, prev))
			break;
		Usercmd_Finish(cur);
		UserCmd_EmitS3(body, cur, prev, pData, nSourceBytes);
		if (body.m_bOverflow)
			break;
		static volatile LONG s_nExtra = 0;
		if ((cur.hasExtraVector || cur.extraFlag) && InterlockedIncrement(&s_nExtra) <= 8)
			S30Log("[USERCMD] cmd %u: S30-only field set (third vector %d, flag %d) -- not sent\n",
				cur.commandNumber, cur.hasExtraVector, cur.extraFlag);
		prev = cur;
		++nDone;
	}
	if (nDone < nCmds)
	{
		static volatile LONG s_nFail = 0;
		if (InterlockedIncrement(&s_nFail) <= 10)
			S30Log("[USERCMD] clc_Move: %d of %d commands parsed (%d of %d bits) -- %s\n",
				nDone, nCmds, r.m_nPos, nBits, nDone ? "sending those" : "dropped");
		if (!nDone)
			return;
	}
	else if (r.m_nPos != nBits)
	{
		static volatile LONG s_nTail = 0;
		if (InterlockedIncrement(&s_nTail) <= 10)
			S30Log("[USERCMD] clc_Move: %d commands read %d of %d bits -- format drift\n", nCmds, r.m_nPos, nBits);
	}
	const int nBackupSent = nDone < nBackup ? nDone : nBackup;
	static uint8_t s_msg[2100];
	S30BitWriter_t w(s_msg, sizeof(s_msg));
	w.Write(kS3ClcMove, 7);
	w.Write(static_cast<uint32_t>(nDone - nBackupSent), 4);
	w.Write(static_cast<uint32_t>(nBackupSent), 3);
	w.Write(static_cast<uint32_t>(body.m_nPos), 16);
	S30BitReader_t bodyReader(s_body, body.Bytes());
	w.WriteFrom(bodyReader, body.m_nPos);
	if (w.m_bOverflow)
		return;
	S30Netchan_QueueMove(s_msg, w.m_nPos);

	static volatile LONG s_nMoves = 0;
	const LONG n = InterlockedIncrement(&s_nMoves);
	const bool bMoving = cur.forwardmove != 0.0f || cur.sidemove != 0.0f || cur.upmove != 0.0f;
	if (n <= 10 || (bMoving && n % 60 == 0) || n % 2000 == 0)
		S30LogFile("[USERCMD] clc_Move #%ld: %d cmds (%d backup), %d -> %d bits; cmd %u tick %u fwd %.2f side %.2f up %.2f buttons 0x%X ft %.4f\n",
			n, nDone, nBackupSent, nBits, body.m_nPos, cur.commandNumber, cur.baseSnapshotTickCount,
			cur.forwardmove, cur.sidemove, cur.upmove, cur.buttons, cur.frametime);
}

static bool h_SendNetMsg(void* pChan, void* pMsg, bool bReliable, bool bVoice)
{
	if (pMsg && s_pMoveVtable && *static_cast<void**>(pMsg) == s_pMoveVtable)
	{
		__try
		{
			Usercmd_OnMove(static_cast<const uint8_t*>(pMsg));
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			static volatile LONG s_nFault = 0;
			if (InterlockedIncrement(&s_nFault) <= 5)
				S30Log("[USERCMD] clc_Move transcode faulted (0x%08lX)\n", GetExceptionCode());
		}
	}
	else if (pMsg)
		S30C2S_OnEngineMessage(pMsg); // the engine's own packets never reach the dedi
	return v_SendNetMsg(pChan, pMsg, bReliable, bVoice);
}

bool S30Usercmd_Install(void)
{
	int nMatchesMove = 0, nMatchesSend = 0;
	uint8_t* const pWriteMove = S30Pattern_FindUnique(s_pszPatternWriteMoveCmds, &nMatchesMove);
	uint8_t* const pSend = S30Pattern_FindUnique(s_pszPatternSendNetMsg, &nMatchesSend);
	if (!pWriteMove || !pSend)
	{
		S30Log("[USERCMD] unresolved (ClWriteMoveCmds %d, SendNetMsg %d matches) -- no input reaches the dedi\n",
			nMatchesMove, nMatchesSend);
		return false;
	}
	const uint8_t* const pLea = pWriteMove + s_nMoveVtableLea;
	if (pLea[0] != 0x48 || pLea[1] != 0x8D || pLea[2] != 0x05)
	{
		S30Log("[USERCMD] clc_Move vtable lea not at ClWriteMoveCmds+0x%X -- no input reaches the dedi\n", s_nMoveVtableLea);
		return false;
	}
	int32_t nRel = 0;
	memcpy(&nRel, pLea + 3, sizeof(nRel));
	s_pMoveVtable = pLea + 7 + nRel;
	v_SendNetMsg = reinterpret_cast<SendNetMsg_t>(pSend);
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_SendNetMsg), h_SendNetMsg);
	const LONG nError = DetourTransactionCommit();
	S30Log("[USERCMD] SendNetMsg @ 0x%llX %s (commit %ld), clc_Move vtable @ 0x%llX\n",
		static_cast<unsigned long long>(S30Pattern_Rva(pSend)), nError == NO_ERROR ? "hooked" : "FAILED", nError,
		static_cast<unsigned long long>(S30Pattern_Rva(s_pMoveVtable)));
	return nError == NO_ERROR;
}
