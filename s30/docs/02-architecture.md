# 2. How the S21 client talks to the S3 server

Source: `src/engine/client/net_bridge_*`, `AGENTS.md`, launch scripts, and the import tables of both exes.

```
 S21 r5apex(_dx12).exe                                    S3 r5apex_ds.exe
 ├─ import: loader.dll (.l2 section)                      ├─ import: loader.dll
 │   └─ client.dll  (CLIENT_DLL, vpc_cl)                  │   └─ server.dll (DEDICATED, vpc)
 │       ├─ ~760 detours/patterns on S21                  │       ├─ dt_extend: promote S3 classes, publish
 │       ├─ origin_offline / ebisu stubs                  │       │   the props the S21 client expects
 │       ├─ pak/filesystem/script VM hooks                │       ├─ SDK messages (SetClassVar, SystemSayText,
 │       └─ NET BRIDGE (transcoder, ~31k lines)           │       │   net_ScriptMessage = types 66..68)
 │           S2C: S3 wire  ──► S21 msg handlers           │       ├─ datablock sender, precache natives, ...
 │           C2S: S21 msgs ──► S3 wire                    │       └─ Flowstate server scripts
 │                                                        │
 └──────────── UDP 37015, S3 netchannel framing ◄─────────┘
```

1. **Injection.** Each exe has `loader.dll` added to its import table through an appended `.l2` section.
   `loader.dll` picks the product from the host exe name.
2. **Startup.** `client.dll` resolves its signatures and attaches detours. In the dedi, `init_server.cpp`
   walks GetCon/GetFun/GetVar. The client stubs Origin/EA (`ebisusdk/origin_offline.cpp`, `-offline -noorigin`).
3. **Connect.** The `connect ip:port` command (`bridge_connect_cmd.cpp`) goes through the bridge. On the S3
   challenge reply (`ffffffff 49`), the bridge builds the S3 `C2S_CONNECT` bitstream itself
   (`net_bridge_process.cpp`, "Stage 1 -> 2"), with optional join auth and password.
4. **Netchannel.** The bridge parses S3 packet headers itself (seq/ack/flags, the `0xFDBAC34D` subchannel nonce,
   reliable fragments, split packets) and keeps its own sequence space.
5. **Messages.** `s_S3ToS21[69]` / `s_S21ToS3` map the 7-bit message IDs. Each entry is one of:
   - direct (same body);
   - `-2` FORMAT BRIDGE, where the body is rewritten (SignonState 22-bit fields, ServerInfo, Print 4-bit prefix,
     Sounds, ServerTick, PlaylistChange, DatatableChecksum, TempEntities 22→24-bit length);
   - `-1` suppressed or body-skipped (AntiCheat, DLCNotifyOwnership, Menu).

   `svc_Snapshot` is re-encoded. ScriptRemote S2C is injected by wire-carried name (type 68).
6. **Tables.** Both ends run with sendtable transmit (`-forceSendTable` / `sv_allowSendTableTransmitToClients 1`).
   The client builds decoders from the dedi's SendTables and the bridge validates their structure before the
   native parser sees them. **A networked field the S21 client needs is published by the dedi (dt_extend), not
   faked on the client** (AGENTS.md).
7. **C2S.** `net_bridge_c2s*.cpp` (about 3.1k lines) re-encodes usercmds (clc_Move), ClientTick, StringCmd, and
   others into S3 form.
8. **Scripts.** All three VMs compile the same Flowstate script tree. The S21 install ships no `script_*.rpak`, so
   client scripts come from loose files in `platform/scripts`.
9. **Assets.** The client loads S21 rpaks plus Flowstate's own. The dedi loads `paks/Win64_server` and the dedi BSP
   from `vpk/englishserver_<map>.bsp.pak000_dir.vpk`.

## What this means for S30

The design carries over as it is: the dedi keeps speaking S3+SDK, and a client DLL transcodes to the client's
message set. What does not carry over is every engine address, structure offset, message number and message
body format on the client side, plus the script and UI surface the shared Flowstate scripts depend on.
