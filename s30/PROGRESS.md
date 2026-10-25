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

## Done
- A1–A4 (inventory, architecture, census, strings diff).
- E0 boot probe.
- C1 loader → client_s30, C2 offline + network guard.

## Open issues
- Not yet known whether S30 checks its own image or import table (B6).
- Flowstate client/UI scripts vs S30 natives: biggest unknown (B3).

## Next action
SDK console and engine print capture on S30 (find the real Script_PrintFunc / engine spew on S30.1), then take
control of the UI script flow (loose-script override of `script_ui.rpak`) toward C4 (main menu with SDK console).
