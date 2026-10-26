//=============================================================================//
//
// Purpose: per-usercmd prediction RNG seed, matched with the dedi.
//
// The dedi seeds each command from its command number only (bridge_cmd_seed_parity:
// MD5_PseudoRandom(command_number) & 0x7FFFFFFF). S30's C_UserCmd seed function
// (0xBA5900) also hashes pitch and yaw (mod 360) and stores the seed at cmd+0x12C,
// so every predicted spread / recoil roll would differ from the dedi's. As the
// S21 client's bridge_cmd_seed does, the hook derives it the dedi's way.
// s30/docs/16.
//
//=============================================================================//
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "thirdparty/detours/include/detours.h"
#include "bridge_cmdseed_s30.h"
#include "s30_log.h"
#include "s30_pattern.h"

static const char* const s_pszPatternComputeSeed =
	"48 89 5C 24 18 55 48 8B EC 48 81 EC 80 00 00 00 33 C0 C7 45 A0 01 23 45 67 48 8B D9 C7 45 A4 89";
static const int CMD_COMMANDNUMBER = 0x0;
static const int CMD_RANDOMSEED = 0x12C; // 0xBA5900: *(a1 + 300)

typedef int (*ComputeSeed_t)(uint8_t* pCmd);
static ComputeSeed_t v_ComputeSeed = nullptr;

// RFC 1321 MD5 of the 4-byte command number, the engine's MD5_PseudoRandom: the
// u32 at digest byte 6 (the S21 client's and the dedi's BridgeSeed_FromCommandNumber).
static uint32_t CmdSeed_FromCommandNumber(uint32_t nCommand)
{
	static const uint32_t K[64] = {
		0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
		0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
		0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
		0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
		0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
		0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
		0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
		0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
	static const uint8_t S[64] = {
		7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
		5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
		4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
		6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
	uint8_t block[64] = {};
	memcpy(block, &nCommand, 4);
	block[4] = 0x80;
	block[56] = 32;
	uint32_t m[16];
	memcpy(m, block, 64);
	uint32_t st[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
	uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
	for (int i = 0; i < 64; ++i)
	{
		uint32_t f;
		int g;
		if (i < 16) { f = (b & c) | (~b & d); g = i; }
		else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
		else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
		else { f = c ^ (b | ~d); g = (7 * i) & 15; }
		const uint32_t t = d;
		d = c;
		c = b;
		const uint32_t x = a + f + K[i] + m[g];
		b = b + ((x << S[i]) | (x >> (32 - S[i])));
		a = t;
	}
	st[0] += a; st[1] += b; st[2] += c; st[3] += d;
	uint8_t digest[16];
	memcpy(digest, st, 16);
	uint32_t nOut;
	memcpy(&nOut, digest + 6, 4);
	return nOut & 0x7FFFFFFFu;
}

static int h_ComputeSeed(uint8_t* pCmd)
{
	if (!pCmd)
		return v_ComputeSeed(pCmd);
	uint32_t nCommand;
	memcpy(&nCommand, pCmd + CMD_COMMANDNUMBER, 4);
	const uint32_t nSeed = CmdSeed_FromCommandNumber(nCommand);
	memcpy(pCmd + CMD_RANDOMSEED, &nSeed, 4);
	return static_cast<int>(nSeed);
}

bool S30CmdSeed_Install(void)
{
	// Known answers of MD5_PseudoRandom (the S21 client's self-test): a broken copy keeps the native seed.
	if (CmdSeed_FromCommandNumber(0u) != 0x2D863277u || CmdSeed_FromCommandNumber(1u) != 0x770B7539u
		|| CmdSeed_FromCommandNumber(0x12345678u) != 0x6C8AB800u)
	{
		S30Log("[CMD-SEED] MD5 self-test FAILED -- native seed kept\n");
		return false;
	}
	int nMatches = 0;
	v_ComputeSeed = reinterpret_cast<ComputeSeed_t>(S30Pattern_FindUnique(s_pszPatternComputeSeed, &nMatches));
	if (!v_ComputeSeed)
	{
		S30Log("[CMD-SEED] seed function unresolved (%d matches) -- predicted weapon RNG differs from the dedi\n", nMatches);
		return false;
	}
	const uintptr_t nRva = S30Pattern_Rva(reinterpret_cast<void*>(v_ComputeSeed));
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&reinterpret_cast<PVOID&>(v_ComputeSeed), h_ComputeSeed);
	const LONG nError = DetourTransactionCommit();
	S30Log("[CMD-SEED] seed function 0x%llX %s (command number only, as the dedi)\n", static_cast<unsigned long long>(nRva),
		nError == NO_ERROR ? "hooked" : "FAILED");
	return nError == NO_ERROR;
}
