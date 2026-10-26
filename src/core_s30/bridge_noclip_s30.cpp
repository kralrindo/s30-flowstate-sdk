//=============================================================================//
//
// Purpose: client noclip prediction (the S21 bridge's [NOCLIP-SIM] on S30).
//
// S30, like S21, has no sv_noclipspeed* convars and its PlayerMove does nothing
// for MOVETYPE_NOCLIP (9): the movetype switch at 0xB26E67 (inside 0xB25BC0,
// game movement in rsi: player +0x08, move data +0x10) sends case 9 to its exit
// 0xB27130 like the movetypes it does not simulate. So a noclipping client never
// moves itself; every snapshot snaps it to the dedi -- the noclip "lag" and its
// prediction errors (m_vecAbsVelocity). The dedi runs S3's FullNoClipMove there.
//
// Case 9's jump table slot is pointed at a stub that runs that step on the move
// data and continues at the switch exit, where the dedi's own PlayerMove goes on
// after FullNoClipMove. The step is the S21 bridge's NoClipSim_Run (its constants
// are the dedi's: sv_noclipspeed 5 / fast 30 / slow 0.25 x sv_maxspeed 320,
// sv_noclipaccelerate 10000 / fast 50000 / slow 10000, sv_friction 4).
//
// S30.1 offsets: move data (probed live in noclip against the player's origin,
// velocity and inputs; S30 inserts the forward/right/up vectors after the moves)
// view angles +0x04, buttons +0x1C, forward/side/up +0x28/+0x2C/+0x30, origin
// +0x10C, velocity +0x118; player m_MoveType +0x3AA, deadflag +0x2614,
// m_hovering +0x30CC, abs velocity +0x160, abs origin +0x16C; client globals
// pointer 0x1B4D4A0 (frametime +0x30). S30 has no m_surfaceFriction; the dedi's
// stays at its ground value while noclipping, 1.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "bridge_noclip_s30.h"
#include "s30_image.h"
#include "s30_log.h"
#include "s30_pattern.h"

// mov rdx,[rsi+8]; movzx eax,byte [rdx+3AAh]; cmp eax,0Fh; ja exit; lea r8,[image]; mov ecx,[r8+rax*4+table]; add rcx,r8; jmp rcx
static const char* const s_pszPatternMoveTypeSwitch =
	"48 8B 56 08 0F B6 82 AA 03 00 00 83 F8 0F 0F 87 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 41 8B 8C 80 ?? ?? ?? ?? 49 03 C8 FF E1";
static const int kMoveTypeNoclip = 9;
static const int kMoveTypeCount = 16;

static const int GM_PLAYER = 0x08;
static const int GM_MOVEDATA = 0x10;
static const int MV_ANGLES = 0x04;
static const int MV_BUTTONS = 0x1C;
static const int MV_FORWARD = 0x28;
static const int MV_SIDE = 0x2C;
static const int MV_UP = 0x30;
static const int MV_ORIGIN = 0x10C;
static const int MV_VELOCITY = 0x118;
static const int PL_MOVETYPE = 0x3AA;
static const int PL_DEADFLAG = 0x2614;
static const int PL_HOVERING = 0x30CC;
static const int PL_ABSVELOCITY = 0x160;
static const int PL_ABSORIGIN = 0x16C;
static const uintptr_t kGlobalsPtr = 0x1B4D4A0;
static const int GLOBALS_FRAMETIME = 0x30;
static const int GLOBALS_INTERVAL = 0x44;

// The S3 dedi's defaults (read from its convars).
static const float kNoclipSpeed = 5.0f, kNoclipSpeedFast = 30.0f, kNoclipSpeedSlow = 0.25f;
static const float kNoclipAccel = 10000.0f, kNoclipAccelFast = 50000.0f, kNoclipAccelSlow = 10000.0f;
static const float kMaxSpeed = 320.0f;
static const float kFriction = 4.0f;
static const float kSurfaceFriction = 1.0f;

// The dedi's case 9: factor/accel from the buttons, then FullNoClipMove.
static void Noclip_Step(uint8_t* pGameMovement)
{
	uint8_t* const pPlayer = *reinterpret_cast<uint8_t**>(pGameMovement + GM_PLAYER);
	uint8_t* const pMove = *reinterpret_cast<uint8_t**>(pGameMovement + GM_MOVEDATA);
	if (!pPlayer || !pMove || pPlayer[PL_MOVETYPE] != kMoveTypeNoclip)
		return;

	const uint8_t* const pGlobals = *reinterpret_cast<uint8_t* const*>(S30Image_Base() + kGlobalsPtr);
	float flFrametime = pGlobals ? *reinterpret_cast<const float*>(pGlobals + GLOBALS_FRAMETIME) : 0.0f;
	if (!(flFrametime > 0.0f) || flFrametime > 0.25f)
		flFrametime = pGlobals ? *reinterpret_cast<const float*>(pGlobals + GLOBALS_INTERVAL) : 0.05f;

	const int nButtons = *reinterpret_cast<const int*>(pMove + MV_BUTTONS);
	float flFactor, flAccel;
	if (nButtons & 0x30000)
	{
		flFactor = kNoclipSpeedFast;
		flAccel = kNoclipAccelFast;
	}
	else if (nButtons & 0x4000004)
	{
		flFactor = kNoclipSpeedSlow;
		flAccel = kNoclipAccelSlow;
	}
	else
	{
		flFactor = kNoclipSpeed;
		flAccel = kNoclipAccel;
	}
	const float flMaxSpeed = flFactor * kMaxSpeed;

	// AngleVectors, degrees.
	const float* const pAng = reinterpret_cast<const float*>(pMove + MV_ANGLES);
	const float flDeg2Rad = 3.14159265358979f / 180.0f;
	const float sp = sinf(pAng[0] * flDeg2Rad), cp = cosf(pAng[0] * flDeg2Rad);
	const float sy = sinf(pAng[1] * flDeg2Rad), cy = cosf(pAng[1] * flDeg2Rad);
	const float sr = sinf(pAng[2] * flDeg2Rad), cr = cosf(pAng[2] * flDeg2Rad);
	const float fwd[3] = { cp * cy, cp * sy, -sp };
	const float right[3] = { -sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp };

	// forward/side scale with sv_maxspeed, up does not (the dedi's own code).
	const float flFwd = flFactor * *reinterpret_cast<const float*>(pMove + MV_FORWARD) * kMaxSpeed;
	const float flSide = flFactor * *reinterpret_cast<const float*>(pMove + MV_SIDE) * kMaxSpeed;
	const float flUp = flFactor * *reinterpret_cast<const float*>(pMove + MV_UP);
	float wish[3];
	for (int i = 0; i < 3; ++i)
		wish[i] = flSide * right[i] + flFwd * fwd[i];
	wish[2] += flUp;

	float flWishSpeed = sqrtf(wish[0] * wish[0] + wish[1] * wish[1] + wish[2] * wish[2]);
	float wishdir[3] = {};
	if (flWishSpeed > 1e-6f)
	{
		for (int i = 0; i < 3; ++i)
			wishdir[i] = wish[i] / flWishSpeed;
	}
	if (flWishSpeed > flMaxSpeed)
	{
		for (int i = 0; i < 3; ++i)
			wish[i] *= flMaxSpeed / flWishSpeed;
		flWishSpeed = flMaxSpeed;
	}

	float* const vel = reinterpret_cast<float*>(pMove + MV_VELOCITY);
	float* const org = reinterpret_cast<float*>(pMove + MV_ORIGIN);
	const bool bNoAccel = pPlayer[PL_DEADFLAG] != 0 || pPlayer[PL_HOVERING] != 0;

	// Accelerate (r5): accelspeed = min(addspeed, accel * frametime * surface
	// friction), |vel| capped at max(old speed, wish speed); skipped while dead or
	// hovering. Then friction; below 1 unit/s the velocity zeroes and there is no move.
	const float flCurSq = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
	const float flCapSq = flCurSq > flWishSpeed * flWishSpeed ? flCurSq : flWishSpeed * flWishSpeed;
	const float flAdd = flWishSpeed - (vel[0] * wishdir[0] + vel[1] * wishdir[1] + vel[2] * wishdir[2]);
	if (flAdd > 0.0f && !bNoAccel)
	{
		float flAccSpeed = flAccel * flFrametime * kSurfaceFriction;
		if (flAccSpeed > flAdd)
			flAccSpeed = flAdd;
		for (int i = 0; i < 3; ++i)
			vel[i] += wishdir[i] * flAccSpeed;
		const float flNewSq = vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2];
		if (flNewSq > flCapSq && flNewSq > 1e-12f)
		{
			const float flClamp = sqrtf(flCapSq / flNewSq);
			for (int i = 0; i < 3; ++i)
				vel[i] *= flClamp;
		}
	}
	const float flSpeed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
	if (flSpeed < 1.0f)
	{
		vel[0] = vel[1] = vel[2] = 0.0f;
	}
	else
	{
		const float flControl = flSpeed > flMaxSpeed * 0.25f ? flSpeed : flMaxSpeed * 0.25f;
		float flNew = flSpeed - kSurfaceFriction * kFriction * flControl * flFrametime;
		if (flNew < 0.0f)
			flNew = 0.0f;
		for (int i = 0; i < 3; ++i)
			vel[i] *= flNew / flSpeed;
		for (int i = 0; i < 3; ++i)
		{
			float v = org[i] + flFrametime * vel[i];
			v = v > 65535.0f ? 65535.0f : v < -65535.0f ? -65535.0f : v;
			org[i] = v;
		}
	}

	static volatile LONG s_nSteps = 0;
	const LONG n = InterlockedIncrement(&s_nSteps);
	if (n <= 3 || n % 2000 == 0)
	{
		const float* const pAbsOrg = reinterpret_cast<const float*>(pPlayer + PL_ABSORIGIN);
		const float* const pAbsVel = reinterpret_cast<const float*>(pPlayer + PL_ABSVELOCITY);
		S30LogFile("[NOCLIP] step #%ld ft %.4f buttons 0x%X move %.2f %.2f %.2f ang %.1f %.1f | mv org %.1f %.1f %.1f vel %.1f %.1f %.1f"
			" | player org %.1f %.1f %.1f vel %.1f %.1f %.1f\n", n, flFrametime, nButtons,
			*reinterpret_cast<const float*>(pMove + MV_FORWARD), *reinterpret_cast<const float*>(pMove + MV_SIDE),
			*reinterpret_cast<const float*>(pMove + MV_UP), pAng[0], pAng[1], org[0], org[1], org[2], vel[0], vel[1], vel[2],
			pAbsOrg[0], pAbsOrg[1], pAbsOrg[2], pAbsVel[0], pAbsVel[1], pAbsVel[2]);
	}
}

static void Noclip_StepSafe(uint8_t* pGameMovement)
{
	__try
	{
		Noclip_Step(pGameMovement);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		static volatile LONG s_nFaults = 0;
		if (InterlockedIncrement(&s_nFaults) <= 3)
			S30Log("[NOCLIP] step faulted (0x%08lX)\n", GetExceptionCode());
	}
}

// Executable page within 2 GB of the image (jump table slots are 32-bit image offsets).
static uint8_t* Noclip_AllocNearImage(uintptr_t nBase, size_t nImageSize)
{
	for (uintptr_t a = (nBase + nImageSize + 0xFFFF) & ~static_cast<uintptr_t>(0xFFFF); a < nBase + 0x7FFF0000ull; a += 0x10000)
	{
		void* const p = VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (p)
			return static_cast<uint8_t*>(p);
	}
	return nullptr;
}

bool S30Noclip_Install(void)
{
	int nMatches = 0;
	uint8_t* const pSwitch = static_cast<uint8_t*>(S30Pattern_FindUnique(s_pszPatternMoveTypeSwitch, &nMatches));
	if (!pSwitch)
	{
		S30Log("[NOCLIP] movetype switch unresolved (%d matches) -- noclip not predicted\n", nMatches);
		return false;
	}
	const uintptr_t nBase = S30Image_Base();
	const uint8_t* const pExit = pSwitch + 20 + *reinterpret_cast<const int32_t*>(pSwitch + 16); // ja target
	int32_t* const pTable = reinterpret_cast<int32_t*>(nBase + *reinterpret_cast<const uint32_t*>(pSwitch + 31));
	if (reinterpret_cast<uintptr_t>(pExit) != nBase + static_cast<uint32_t>(pTable[kMoveTypeNoclip]))
	{
		S30Log("[NOCLIP] case %d is not the switch exit (0x%X vs 0x%llX) -- left alone\n", kMoveTypeNoclip,
			pTable[kMoveTypeNoclip], static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pExit) - nBase));
		return false;
	}
	const IMAGE_NT_HEADERS* const pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase)->e_lfanew);
	uint8_t* const pStub = Noclip_AllocNearImage(nBase, pNt->OptionalHeader.SizeOfImage);
	if (!pStub)
	{
		S30Log("[NOCLIP] no stub page near the image -- noclip not predicted\n");
		return false;
	}
	// At a case entry the frame is 16-byte aligned and the game movement is in rsi;
	// the exit needs only rsi, rbp and the frame, all preserved by the call.
	uint8_t* p = pStub;
	const uint8_t movRcxRsi[] = { 0x48, 0x8B, 0xCE };             // mov rcx, rsi
	const uint8_t subRsp[] = { 0x48, 0x83, 0xEC, 0x20 };          // sub rsp, 20h
	const uint8_t addRsp[] = { 0x48, 0x83, 0xC4, 0x20 };          // add rsp, 20h
	memcpy(p, movRcxRsi, sizeof(movRcxRsi)); p += sizeof(movRcxRsi);
	memcpy(p, subRsp, sizeof(subRsp)); p += sizeof(subRsp);
	*p++ = 0x48; *p++ = 0xB8;                                     // mov rax, imm64
	const uint64_t nStep = reinterpret_cast<uint64_t>(&Noclip_StepSafe);
	memcpy(p, &nStep, 8); p += 8;
	*p++ = 0xFF; *p++ = 0xD0;                                     // call rax
	memcpy(p, addRsp, sizeof(addRsp)); p += sizeof(addRsp);
	*p++ = 0x48; *p++ = 0xB8;                                     // mov rax, imm64
	const uint64_t nExit = reinterpret_cast<uint64_t>(pExit);
	memcpy(p, &nExit, 8); p += 8;
	*p++ = 0xFF; *p++ = 0xE0;                                     // jmp rax
	FlushInstructionCache(GetCurrentProcess(), pStub, p - pStub);

	DWORD nOld = 0;
	if (!VirtualProtect(&pTable[kMoveTypeNoclip], 4, PAGE_EXECUTE_READWRITE, &nOld))
		return false;
	pTable[kMoveTypeNoclip] = static_cast<int32_t>(reinterpret_cast<uintptr_t>(pStub) - nBase);
	VirtualProtect(&pTable[kMoveTypeNoclip], 4, nOld, &nOld);
	S30Log("[NOCLIP] PlayerMove case %d (switch 0x%llX) -> noclip step stub %p\n", kMoveTypeNoclip,
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pSwitch) - nBase), pStub);
	return true;
}
