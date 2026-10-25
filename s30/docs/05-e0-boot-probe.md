# 5. E0: boot probe (2026-10-25)

**Question.** Does the S30.1 exe run an added import? Does it check its own image? Does it touch the network?
How far does it boot with nothing else changed?

## Setup
- `build\s30_client\`: hardlinks to `depot\` for every file except `r5apex_dx12.exe`, which is a real copy patched by
  `s30/tools/add_import.py` (`.l2` section at VA `0xCD84000`, `s30probe.dll!DummyExport` appended as import 33).
  S30's section headers end at `0x3F8` and its data starts at `0x600`, so the patcher raises `SizeOfHeaders` to `0x600`.
  Afterwards the depot exe md5 is unchanged and its hardlink list shows only itself.
- `s30/probe/probe.cpp`, built with `probe/build.bat` (MSVC 18 and the SDK's in-tree Detours), does three things:
  - hooks `connect`, `WSAConnect`, ConnectEx (via `WSAIoctl`), `sendto`, `WSASendTo`, `getaddrinfo`, `GetAddrInfoW/ExW`,
    `gethostbyname`, `WinHttpConnect` and `InternetConnectA/W`, and refuses anything that is not loopback. If the
    hooks fail to commit, the process terminates.
  - logs modules, windows, working set and fault exceptions once a second.
- Args: `-novid -windowed -noorigin -dev -w 1280 -h 720`. No `steam_appid.txt`. Ran for 400 s, then the probe's own
  process was terminated. Log: `tests\E0_boot_probe\probe_run1.log`.

## Results

| check | result |
|---|---|
| added import loads | yes: DllMain ran at t=0, `TimeDateStamp=0x6AAB34CE`, `SizeOfImage=0xCD85000` (patched) |
| image or import-table self-check | none seen in 400 s: no fault, no exit (`.text` patching is still untested) |
| network | guard armed before game code ran. 10 lookups refused, 0 connects or sends attempted: `R5-pc-stats.stryder.respawn.com` (×5, exe+0x5E2BF4), `fastpatch-rules.r5prod.stryder.respawn.com` (×2), `pr-…data.ea.com` (×2, exe+0x1242828), own hostname (×1) |
| boot | 128 modules; D3D12 device up; window `Apex Legends` (class `Respawn001`) at t=4 s; `EOSSDK-Win64-Shipping.dll` loaded; `OriginSDK.dll` not loaded. Working set flat at about 939 MB from t=4 s, with no further activity. |
| faults | 0 |

**Reading.** Injecting through the import table works on S30, and the probe's guard is a sound base for C2. The
client stops right after EOS starts, most likely waiting on the EA/EOS sign-in that the guard blocks. The S21 SDK
gets past the same point with `origin_offline` stubs, so the next step is the S30 equivalent. I could not see the
screen: the screenshot request resolved to the Steam "Apex Legends" entry and was declined. The S30 IDB
(`analysis\idb\r5apex_dx12_s30.exe.i64`) is built, so the stall point can be found statically from the EOS init
path.

## Next
1. Find the wait in IDA, following the EOS platform init and the callers of exe+0x1242828 and exe+0x5E2BF4.
2. Write the offline stub for it in the probe first, then move it into `client_s30`.
3. Reach the main menu (C4) with the guard still showing zero allowed external traffic.
