# Progress

## Log

### 2026-10-25: Phase A recon
- Surveyed the S30.1 depot, the reference install (`C:\R5Flowstate`, read-only) and the tools. See docs/01.
- S30.1 exe is unpacked and analysable directly. The live client (Oct 1 build, packed) is newer, so S30.1 is not live.
- Documented the S21↔S3 connection path (docs/02). The S3 dedi keeps speaking S3+SDK; the client DLL transcodes.
- Signature census: 282 of 760 S21-client signatures match S30 exactly once (37.1 %), 31 match more than once,
  447 are missing. Bridge symbol table: 91 of 218 resolve.
- Strings diff: net convars and sendtable transmit levers still exist; no RTTI in S30; S30 adds
  script-rpak/md5 protocol checks and drops `cl_clock_correction`.
- 5 S30 maps have S21-built dedi data under the same name. First test map: `mp_rr_canyonlands_staging_mu1`.
- IDA Pro 9.1 headless first refused to run ("Cannot continue without a valid license"); the user then fixed the licence.
- Repo set up from `s21-unify` @ `10077bb`; status image generator added.

### 2026-10-25: E0 boot probe (docs/05)
- `tools/add_import.py` adds an import to a copy of the S30 exe the same way S21 Flowstate adds loader.dll
  (`.l2` section). `probe/` holds a minimal injected DLL with a loopback-only network guard and boot logging.
- S30 runs the added import with no self-check in 400 s. The guard refused 10 outbound DNS lookups (stryder stats,
  fastpatch rules, EA telemetry); no connections were attempted.
- Boot reaches the window, D3D12 and EOS init, then idles, most likely waiting on EA/EOS sign-in.
- IDA licence fixed by the user; S30 IDB built (`analysis\idb\r5apex_dx12_s30.exe.i64`).

### 2026-10-25: loader.dll + client_s30 (docs/06)
- loader.dll recognises S30.1 by TimeDateStamp: LauncherMain RVA 0x588C50 (verified via WinMain), worker
  `client_s30.dll`, Saved Games retargeted to `Apex_s30`, and an early stage in DllMain that arms the network guard
  before any host code runs. S21 paths unchanged.
- New product `src/core_s30` (client_s30.dll): log, loopback guard, EA-app spawn watchdog, crash/exit capture, and the
  S30 port of the S21 offline platform patches (D/E/A1/A2/C, plus F = Origin env-setting Error() and P = Origin poll).
- Result: the S30 client boots through Origin init and stays up (75 s+, no fault, no exit), fully guarded.
- Found: the earlier probe runs wrote the live Saved Games settings (now prevented) and most likely started the EA
  app (now refused).
- Census tool: fixed name attribution (names came from the previous statement); counts unchanged.

### 2026-10-25: SDK console + engine/script output (docs/07)
- Engine print path mapped: CoreMsg 0x269B40 (241 refs) -> CoreMsgV 0xD39C30 -> LoggingSystem_LogV 0x5C7680
  (severity filter) -> LogDirect 0x5C6C40. client_s30 hooks CoreMsgV + LogDirect; retail emits almost nothing.
- SDK console window "R5F S30 Client": engine/script/SDK output, commands via Cbuf_AddText (0x248760, identical to
  S21). Verified end to end: `quit` from the console path shuts the engine down cleanly.
- Script print callbacks hooked (0xF5B1D0 client/server, 0xF5B2B0 UI); the UI VM's print pointer is confirmed to be
  the hooked one, but retail UI scripts call printl 0 times at the title screen.
- Retail strips command bodies (echo = return 0): useful commands must come from the SDK.

### 2026-10-26: ImGui overlay + engine ConCommands (docs/08)
- DX12 ImGui overlay in client_s30 (Present/Present1/ResizeBuffers(1)/ExecuteCommandLists hooks, render on the game
  queue): SDK Console, Status and Connect panels, ` / F10 toggle, input routed through a WndProc subclass, scaled for
  1440p. Verified by in-process back-buffer screenshots and posted key input.
- S30 ConCommand/ICvar ABI verified (ConCommandBase lost m_bRegistered at +0x10, now bit 0 of +0x2C; ICvar slot 9
  RegisterConCommand). SDK commands are real engine commands; proven by an engine-dispatched `sdk_status`.
- Test tooling: command drop file, post_keys.ps1, sdk_screenshot, -s30_overlay.

## Done
- A1–A4 (inventory, architecture, census, strings diff).
- E0 boot probe.
- C1 loader → client_s30, C2 offline + network guard.
- SDK console with engine log capture and command input (part of C3).
- C4: SDK working on the main menu -- ImGui overlay console/status/connect, engine-registered SDK commands.

## Open issues
- Not yet known whether S30 checks its own image or import table (B6).
- Flowstate client/UI scripts vs S30 natives: biggest unknown (B3).

## Next action
Server connection (phase D): start the S3 dedi from a workspace copy, trace what the S30 client sends on `connect`
and what the dedi answers, map S30's net message table (B1), then port the bridge's connect handshake. In parallel,
the S30 -> S3 server asset pipeline (phase E).
