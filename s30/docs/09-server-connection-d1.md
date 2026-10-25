# 9. Server connection, stage 1: the S3 dedi accepts the S30 client (2026-10-26)

## Workspace dedi (`s30/tools/setup_s3_server.ps1`, `start_s3_server.ps1`)

`build\s3_server` is copied read-only from `C:\R5Flowstate`:
- the S21-built `r5apex_ds.exe`, `server.dll` and `loader.dll`;
- `cfg`, `bin`, `r2`, `vpk`, `paks\Win64_server`;
- `platform` without logs and demos;
- navmeshes for the test maps;
- **`r5apexdata.bin`**: without it the dedi's search-path setup (`FileSystem_LoadSearchPaths`, r5apex_ds 0x210F70) calls
  `Error("")` and exits with code 1 and no message. Found by breaking on the SDK's Error forwarder in `server.dll`.

**`net_prophuff.dat` (from the user):** the netprop Huffman table must be identical on client and server. S30 ships it
as a `wrap` asset in `common_early.rpak` (GUID `0x323db47102530468`). It differs from the S21 file the dedi used in
the last byte only (S30 `00`, S21 `41`). The setup script exports it with RSX and installs it in the workspace dedi;
the S21 original is kept as `net_prophuff.dat.s21`.

`start_s3_server.ps1` launches `+launchplaylist survival_firingrange` (`mp_rr_canyonlands_staging_mu1`) on port 37015
and waits for `[R5F-HOST] ready`. It only stops dedis that run from the workspace.

## Why S30's own connect sent nothing

| step | finding | fix |
|---|---|---|
| `connect` handlers (0x2F0B50, 0x2F1220) | refuse without an Origin identity unless `ICommandLine` vfunc 14 (retail `return 0`) | patch G: `jnz` → `jmp` on both |
| `CClientState::Connect` (0x2FCDB0, derived vtable 0x17F1828 slot 11) | platform state 1–3 requires `IsPlatformLoggedIn` (0x3092C0) | offline identity (below) |
| challenge/resend (0x2FD3B0, S21 0x2D4A00) | runs once client state +196 is 1; sends type **18**, then the engine encrypts it (mode 2, +28 bytes) | not used for S3; the bridge sends its own |

**Offline identity (`identity_s30.cpp`).** The globals are decoded from `IsPlatformLoggedIn` itself: platform state,
error level, SDK init, Nucleus ID, profile flag, auth code (256 bytes) and NucleusToken (10,240 bytes). Once the
engine's Origin init has finished, client_s30 sets Nucleus 9990000 (the S21 SDK's `FAKE_BASE_NUCLEUD_ID`), the profile
flag, and short auth/token strings; it re-applies them if the engine clears them. `sdk_identity` shows them. With it,
S30's native connect runs to the challenge.

## Bridge handshake (`bridge_handshake_s30.cpp`)

A port of the S21 bridge's socket-level handshake. The bridge owns a dual-stack UDP socket and a polling thread:

```
C2S_CHALLENGE  ff ff ff ff 48 "connect\0" <Nucleus u64>                    21 bytes
S2C_CHALLENGE  ff ff ff ff 49 <challenge u32> ...
C2S_CONNECT    ff ff ff ff 41 <bitstream>                                   337 bytes here
CONNACCEPT     ff ff ff ff 4A <map>\0<mode>\0
```

The `C2S_CONNECT` bitstream is S21's, field for field, which is proven against this dedi:
- protocol 529, authChallenge 2001, challenge echo, authProtocol 2;
- Nucleus ID, persona, password tag;
- sendtable CRC64 `0xA28012AACDFB8F7E`;
- 31 DLC slots;
- the convar list: nucleus ids, the auth token trio, and the local host proof from `Local\R5F_HostProof_<port>`, sent
  to loopback only.

**Result (`tests\E5_bridge_handshake\`):** `bridge_connect 127.0.0.1:37015` → challenge `0x0A501EEB` → `C2S_CONNECT`
→ **`CONNACCEPT mp_rr_canyonlands_staging_mu1 / survival`**. The dedi logs
`Processing connectionless challenge for '[::ffff:127.0.0.1]:63185' ('9990000')`, and its signon pump moves client[0]
from signon 2 to 3 and streams netchannel packets (341 bytes) that the bridge does not consume yet. **Milestone D1.**

## Next (stage 2: signon)
Hand the dedi's netchannel to the S30 engine: put `CClientState` into the connected state for the accepted map; parse
S3 netchannel packets (sequence/ack, subchannels, split packets); and translate S3 messages to S30's message set.
That needs the S30 net message table (B1) and the S30 `CNetChan`/`CClientState` layouts, re-derived the way the S21
bridge's symbol table was.
