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

### 2026-10-26: server connection stage 1, D1 (docs/09)
- Workspace S3 dedi (setup_s3_server.ps1 / start_s3_server.ps1). Needs r5apexdata.bin. Uses S30's net_prophuff.dat
  (common_early.rpak wrap; differs from S21's in the last byte), per the user.
- S30 connect path mapped; offline identity (Nucleus 9990000, tokens) decoded from IsPlatformLoggedIn and applied,
  connect identity gates opened; native S30 challenge now goes out (type 18, encrypted).
- Bridge handshake ported from S21: bridge_connect -> CONNACCEPT mp_rr_canyonlands_staging_mu1/survival; the dedi has
  the client connected and starts signon. Milestone D1.
- -s30_nettrace (loopback packet trace) and -s30_netdiag (connect path observers).

### 2026-10-26: S30 assets for the S3 dedi (docs/10)
- `tools/dedi_paks.py` rebuilds the dedi's `common_early`, `common_roots`, `common`, `common_mp`, `mp_lobby`,
  `root_lgnd_skins` (all 804 S30 `root_lgnd_skins_*` paks) and `mp_rr_canyonlands_staging_mu1` from S30, same
  names: mdl_ v20 -> v10, arig v7 -> v4, aseq v14 (+asqd) -> v7, stgs v2 -> v1, dtbl/stlt/txls repacked;
  dependencies first, references the dedi cannot resolve at load left unregistered. Gates: re-list of every
  built pak, converted models vs the S21 dedi's copies.
- `tools/dedi_map.py`: staging's server BSP rebuilt from S30's (bspconv -dedi, revpk); the old one named 42
  models S30 does not ship.
- Tool fixes in workspace clones (`s30/patches/`): RSX (mdl_ v20 raw export, asqd extents, layout and mod
  export), rmdlconv (S30 RUI table, scaled u16 offsets: legend models had lost all collision), RePak
  (sub-layout order, vector mods, unregistered refs, per-pak compression); local Oodle DLL for rmdlconv.
- Weapon scripts: 140 model paths take S30's (`weapon_model_paths.py`); the space elevator takes S30's name.
- `common_flowstate` and `sdk_s30_dedi` taken out of the workspace dedi.
- Result: the dedi reaches `ready mp_rr_canyonlands_staging_mu1` with 0 unresolved pak deps; one script error
  left (bot loadout stat tracker columns split by MMR in S30).

## Done
- A1–A4 (inventory, architecture, census, strings diff).
- E0 boot probe.
- C1 loader → client_s30, C2 offline + network guard.
- SDK console with engine log capture and command input (part of C3).
- C4: SDK working on the main menu -- ImGui overlay console/status/connect, engine-registered SDK commands.
- D1: S3 dedi accepts the S30 client (bridge handshake).

## Open issues
- Not yet known whether S30 checks its own image or import table (B6).
- Flowstate client/UI scripts vs S30 natives: biggest unknown (B3).

### 2026-10-26: dedi up on S30 data
- Server scripts adapted to S30 data until the staging map spawns cleanly: entitlements.rson in
  `vpk/server_mp_common` (S30's, plus `pc`/`pc_offer` the S3 loader requires and the 11 entitlement flavors S30's rson
  omits; `entitlements_dedi.py`), ranked `followingPeriod` array, S30 challenge modes, loot weapon mods
  (`loot_weapon_mods.py`; the S3 weapon parser keeps 31 mods per weapon, r5apex_ds+0x8E98C7).
- The SDK's error-model fallback works: missing models are logged and replaced instead of hanging the dedi.

### 2026-10-26: S30 net message map, B1 (docs/11)
- S30 `ConnectionStart` 0x2FA120 registers 59 messages through `RegisterMessage` 0x2A1580 (array at netchan
  +0x2180, count +0x2198, indexed by `GetType`); S21: 0x2D1B10 / 0x27A580 / +0x2190 / +0x21A8, 52 messages.
- S21 IDB built (`analysis/idb/r5apex_dx12_s21.exe.i64`) to compare bodies.
- S21 → S30 server-to-client: 3..9 same, 10..45 +1, 46..54 +3. Two signals agree (registration order + sizes, and
  the handler method each `Process` calls). Decompiled readers: every message the dedi sends reads the same,
  except **svc_Print**, which gains a leading 1-bit flag.
- Client-to-server found by vtable scan: clc_Move 57 → 64 (writer identical), ClientTick 65 → 72 (**now three
  values**), ClientSayText 66 → 73, ClaimClientSidePickup 63 → 70.
- Dedi → S30 table written (docs/11).
- Tools: `pe_netmsg.py`, `pe_netmsg_vtables.py`, `ida_netmsg.py`, `netmsg_match.py`, `ida_decomp_dump.py`,
  `netmsg_bodies.py`, `ida_autoanalyze.py`.

### 2026-10-26: stage 2, step 1: the dedi hands the S30 engine its connection (docs/12)
- `bridge_connect` also runs the engine's native connect (`net_usesocketsforloopback 1`,
  `cl_strict_transport_checks 0`; both DEVELOPMENTONLY, flag cleared on these two only). The engine's datagrams to
  the dedi are dropped; on CONNACCEPT it gets its native type-12 "server map" reply on its own socket
  (`recvfrom` route, `NET_DecodeDatagram` 0x2A5870 detour for plaintext).
- Result: `FullConnect` builds the netchannel, signon 2, `maps/mp_rr_canyonlands_staging_mu1.bsp`, Firing Range
  loading screen, from one `bridge_connect`. New `sdk_clstate`.
- Found: the workspace dedi crashes after `ready` with `+launchplaylist survival_firingrange`, not with `+map`.

## Next action
Stage 2, step 2: route the dedi's netchannel packets to the engine socket and replace `CNetChan::ProcessPacket` with
S3 header parsing plus dispatch into S30's message array (docs/11 type map; Print: extra bit). C2S: the engine's
netchannel output translated to S3 (ClientTick: three values). Still to compare: the usercmd delta encoding inside
clc_Move.
