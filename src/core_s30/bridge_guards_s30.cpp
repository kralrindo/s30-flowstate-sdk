//=============================================================================//
//
// Purpose: guards for states the S3 dedi sends that S30's own servers never do.
//
// C_DynamicProp::OnDataChanged (S30.1 0x929D00): on DATA_UPDATE_CREATED it loads
// the entity's studio data (+0x1000) only when its model (vtable +0x60 on the
// renderable at +0x10) exists, then reads the studio data unconditionally:
//
//   0x929D59  cmp edi, 1 / jne 0x929DB4          ; created only
//   ...       studio data load when the model exists
//   0x929D80  mov rax, [rbx+1000h]               ; patched -> stub
//   0x929D87  mov rcx, [rax+8]                    ; faults on a model-less prop
//   0x929D8B  xor eax, eax                        ; stub resumes here
//   0x929DB4  epilogue                            ; stub exits here when null
//
// A dynamic prop the dedi networks with no model (model index -1) faulted there:
// the client crash on mp_rr_olympus_mu3_uh right after signon (minidump
// 20261026_204328, entity vtable 0x18AA998 = C_DynamicProp, m_nModelIndex -1).
// The stub takes the function's own early-out (the path of every non-created
// update) when the studio data is still null after the load attempt; the base
// OnDataChanged call before it is untouched.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "bridge_guards_s30.h"
#include "s30_image.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternDynamicPropCreated =
	"83 FF 01 75 ?? 48 83 BB 00 10 00 00 00 75 18 48 8B 43 10 48 8D 4B 10 FF 50 60 48 85 C0 74 08 48 8B CB "
	"E8 ?? ?? ?? ?? 48 8B 83 00 10 00 00 48 8B 48 08";
static const int kPatchOffset = 0x27;      // mov rax, [rbx+1000h]
static const int kPatchLength = 11;        // + mov rcx, [rax+8]
static const int ENT_MODELINDEX = 0x60;

static void Guards_OnModellessProp(const uint8_t* pEntity)
{
	static volatile LONG s_nGuarded = 0;
	if (InterlockedIncrement(&s_nGuarded) > 10)
		return;
	__try
	{
		S30Log("[GUARD] C_DynamicProp %p created with no model (model index %d) -- model setup skipped\n", pEntity,
			*reinterpret_cast<const int*>(pEntity + ENT_MODELINDEX));
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
	}
}

// Executable page within 2 GB of the image (the patch is a rel32 jmp).
static uint8_t* Guards_AllocNearImage(uintptr_t nBase, size_t nImageSize)
{
	for (uintptr_t a = (nBase + nImageSize + 0xFFFF) & ~static_cast<uintptr_t>(0xFFFF); a < nBase + 0x7FFF0000ull; a += 0x10000)
	{
		void* const p = VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (p)
			return static_cast<uint8_t*>(p);
	}
	return nullptr;
}

bool S30Guards_Install(void)
{
	int nMatches = 0;
	uint8_t* const pCreated = S30Pattern_FindUnique(s_pszPatternDynamicPropCreated, &nMatches);
	if (!pCreated)
	{
		S30Log("[GUARD] C_DynamicProp::OnDataChanged unresolved (%d matches) -- model-less props crash the client\n", nMatches);
		return false;
	}
	const uintptr_t nBase = S30Image_Base();
	uint8_t* const pSite = pCreated + kPatchOffset;
	const uint8_t* const pResume = pSite + kPatchLength;
	const uint8_t* const pExit = pCreated + 5 + static_cast<int8_t>(pCreated[4]); // jne target
	const IMAGE_NT_HEADERS* const pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(nBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(nBase)->e_lfanew);
	uint8_t* const pStub = Guards_AllocNearImage(nBase, pNt->OptionalHeader.SizeOfImage);
	if (!pStub)
	{
		S30Log("[GUARD] no stub page near the image -- model-less props crash the client\n");
		return false;
	}
	// rbx = entity. Both exits only need rbx/rdi/rsp; rax is dead at the resume
	// (xor eax, eax) and rcx/rdx/r8-r11 are dead on the epilogue path. The frame
	// is 16-byte aligned with its own shadow space, so the log call needs no setup.
	uint8_t* p = pStub;
	const uint8_t prologue[] = {
		0x48, 0x8B, 0x83, 0x00, 0x10, 0x00, 0x00,   // mov rax, [rbx+1000h]
		0x48, 0x85, 0xC0,                           // test rax, rax
		0x74, 0x10,                                 // jz null
		0x48, 0x8B, 0x48, 0x08,                     // mov rcx, [rax+8]
	};
	memcpy(p, prologue, sizeof(prologue)); p += sizeof(prologue);
	*p++ = 0x48; *p++ = 0xB8;                                     // mov rax, imm64
	const uint64_t nResume = reinterpret_cast<uint64_t>(pResume);
	memcpy(p, &nResume, 8); p += 8;
	*p++ = 0xFF; *p++ = 0xE0;                                     // jmp rax
	// null:
	*p++ = 0x48; *p++ = 0x8B; *p++ = 0xCB;                        // mov rcx, rbx
	*p++ = 0x48; *p++ = 0xB8;                                     // mov rax, imm64
	const uint64_t nLog = reinterpret_cast<uint64_t>(&Guards_OnModellessProp);
	memcpy(p, &nLog, 8); p += 8;
	*p++ = 0xFF; *p++ = 0xD0;                                     // call rax
	*p++ = 0x48; *p++ = 0xB8;                                     // mov rax, imm64
	const uint64_t nExit = reinterpret_cast<uint64_t>(pExit);
	memcpy(p, &nExit, 8); p += 8;
	*p++ = 0xFF; *p++ = 0xE0;                                     // jmp rax
	FlushInstructionCache(GetCurrentProcess(), pStub, p - pStub);

	DWORD nOld = 0;
	if (!VirtualProtect(pSite, kPatchLength, PAGE_EXECUTE_READWRITE, &nOld))
		return false;
	pSite[0] = 0xE9;                                              // jmp rel32 stub
	const int32_t nRel = static_cast<int32_t>(reinterpret_cast<intptr_t>(pStub) - reinterpret_cast<intptr_t>(pSite + 5));
	memcpy(pSite + 1, &nRel, 4);
	memset(pSite + 5, 0xCC, kPatchLength - 5);
	VirtualProtect(pSite, kPatchLength, nOld, &nOld);
	FlushInstructionCache(GetCurrentProcess(), pSite, kPatchLength);
	S30Log("[GUARD] C_DynamicProp::OnDataChanged studio read 0x%llX -> null-checked stub %p\n",
		static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pSite) - nBase), pStub);
	return true;
}
