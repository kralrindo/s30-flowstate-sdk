# 6. loader.dll on S30 and the client_s30 product (2026-10-25)

## Loader (`src/loader/loader.cpp`)

S30 images are recognised by PE TimeDateStamp (`kS30Images`; S30.1 = `0x6AAB34CE`). For an S30 host the loader:

| step | S21 (unchanged) | S30 |
|---|---|---|
| worker | `client.dll` | `client_s30.dll` |
| early stage (DllMain) | none | load `client_s30.dll`, call `S30_EarlyInit` (log, network guard, spawn watchdog, offline patches). Fatal if missing or failing. |
| Saved Games | `/Respawn/Apex_fnf` → `Apex_r5f` | → `Apex_s30` (never the live `/Respawn/Apex`) |
| LauncherMain | RVA 0x532180 (DX12) | RVA **0x588C50**, verified: `WinMain` (0x589F30) calls it with `hInstance`; same shape as S21's (`-logtofile`/`-logflush`/`-logtostdout`) |
| SDK init | `SDK_Init` at the LauncherMain gate | same |

The relaunch-with-flags step (`-nodiscord -forceSendTable -novid -windowed -allowmultiple -fnf`) is shared. S30 still
selects `/Respawn/Apex_fnf` on `-fnf` (0x606870); it also accepts `-settingsDir`.

**Why the early stage.** WinMain runs about 15 init calls before LauncherMain, and the S30 host starts resolving Respawn
and EA hosts at once, from a shared `getaddrinfo` wrapper (0x5E2BF4) with 19 callers. The guard therefore cannot wait
for the gate.

## client_s30 (`src/core_s30/`)

A small product with no PCH that links none of the S21 `*_cl` libraries. Their dual-ABI layouts (ConVar, sqvm, ...) are
S21's until shown otherwise; each subsystem moves over only once it is verified on S30.

| file | what |
|---|---|
| `s30_image.h` | supported images (TimeDateStamp, LauncherMain RVA) |
| `s30_log.*` | `platform/logs/client_s30/<time>.log`, share-read, flushed per line |
| `s30_pattern.h` | signature search over `.text`; **only a unique match counts** |
| `net_guard_s30.*` | refuses every non-loopback connect/ConnectEx/sendto/WSASendTo/DNS/WinHTTP/WinINet call and logs the caller |
| `offline_s30.*` | CreateProcess watchdog: refuses EA app / Origin spawns |
| `origin_offline_s30.*` | S30 port of the S21 offline platform patches (below) |
| `crash_s30.*` | fault log + first-fault minidump; logs ExitProcess/TerminateProcess(self)/MessageBox with exe frames |

### Offline platform patches (all byte-validated, written all-or-nothing)

S30 statically links the same `Origin::OriginSDK::Initialize` as S21, with identical branches; only the error-code
register moved (`r12d` → `r13d`).

| | S30.1 RVA | S21 DX12 | patch |
|---|---|---|---|
| D | 0x11D0A47 | 0x107A9F4 | `jnz`→`jmp`: a missing EA app install is not fatal |
| E | 0x11D0A85 | 0x107AA32 | NOP: don't skip to LSX when the EA app runs |
| A1 | 0x11D0A91 | 0x107AA3E | NOP: never take the launch-EA-app branch |
| A2 | 0x11D0AA6 | 0x107AA53 | `mov r13d,0A0020000h` → `42010001h` (start succeeded) |
| C | 0x11CDB30 | 0x1078060 | ready predicate → `xor eax,eax; ret`; the idiom matches twice on S30, and the anchor's call at +0x21 picks this one |
| F | 0x42D1E1 | OriginInitFatalCall | NOP `Error("Failed to get Origin environment setting")` |
| P | 0x42F940 | 0x3F7530 | Origin poll (`OriginUpdate`) → `ret`, as S21 DX12 does |

## Runs (logs in `tests\E2_loader_client_s30\`)

| run | result |
|---|---|
| loader + guard + watchdog | client spawned `EADesktop.exe` at 1.9 s (refused), then exited on its own |
| + D/E/A1/A2/C | no spawn; window at 8 s; exit at 8.9 s via `Error()` → ExitProcess(3) (frame 0x42D1E6: Origin env-setting error) |
| + F | runs to 12.8 s; `Error("[%x] %s")` from the Origin poll (0x42F940) |
| + P | **stays up** (75 s+, stopped by hand), window 3440×1440, no fault, no exit. Refused: Valve relay pings from steamnetworkingsockets (264), stryder stats/PLB, EA telemetry, `api.epicgames.dev`, `api.steampowered.com`. Live `Saved Games\Respawn\Apex` untouched; S30 writes `Apex_s30`. |

## Incidents
- E0/E1 (probe, before the loader) wrote the live `Saved Games\Respawn\Apex` settings (resolution 1280×720 windowed),
  because the probe did not retarget Saved Games. Fixed by the loader retarget; the old values could not be restored.
- E0/E1 had no spawn watchdog, so the S30 client most likely started the EA app then. The watchdog now refuses it.

## Not done yet
- Fixed offline identity (S21 sets the profile-init flag + Nucleus id in its RunFrame hook): needed for connect.
- Platform globals (`g_platState` etc.) and the LauncherMain platState zeroing: untested whether S30 needs them.
- Title screen still shows the EA error (expected); the next step is the SDK console and script/UI control.
- S21 loader behaviour is unchanged by construction (non-S30 timestamps take the old paths) but was not re-run, because
  that would mean writing to `C:\R5Flowstate`.
