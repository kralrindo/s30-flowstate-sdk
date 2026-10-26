# 11. S30 net message map (milestone B1, 2026-10-26)

S30 keeps no `svc_`/`clc_` name strings, so messages are matched through their registration, not their names.

## Registration

| | S21 client | S30.1 client |
|---|---|---|
| `CClientState::ConnectionStart` | `0x2D1B10` | `0x2FA120` (slot 1 of the client-state vtable `0x17F1828`) |
| `CNetChan::RegisterMessage` | `0x27A580` | `0x2A1580` |
| message array / count in `CNetChan` | `+0x2190` / `+0x21A8` | `+0x2180` / `+0x2198` (array grown to 0x60 slots) |
| messages registered | 52 (types 3..54) | 59 (types 3..61) |

`RegisterMessage` stores each message at `m_NetMessages[msg->GetType()]`, so the array index is the wire type.
The `INetMessage` vtable layout is the same in both: slot 3 `Process`, 4 `ReadFromBuffer`, 5 `WriteToBuffer`,
7 `GetType` (`mov eax, imm`), 11 returns `sizeof`. Each `Process` calls one method of the client's message handler
(`+0x18` of the message); that method's vtable offset identifies the message across builds.

`ConnectionStart` registers what the client receives (`net_*` and `svc_*`). Client-to-server messages (`clc_Move`
and others) are not in it and are mapped separately (open).

## S21 → S30 (server-to-client and net messages)

Two independent signals give the same pairing: the order and object size of the registered messages
(`netmsg_match.py`), and the handler method each `Process` calls (`analysis/netmsg_process_handler_calls.json`;
both handler interfaces keep their order, S30 inserts slots).

| S21 type | S30 type |
|---|---|
| 3..9 | same |
| 10..45 | +1 |
| 46..54 | +3 |

S30-only: **10** (another generic message sharing reader `0x480AD0` with 8, 9 and 48), **47**, **48**, **58..61**.
S21 45 → S30 46 changes size (368 → 288 bytes); S21 37 → S30 38 (160 → 560), S21 42 → S30 43 (3,488 → 41,648),
S21 14 → S30 15 (Playlists buffer 512,048 → 768,048), S21 27 → S30 28 (PersistenceBaseline 131,120 → 163,888) and
S21 19 → S30 20 (2,096 → 2,104) change size too.

### Bodies (S21 IDB built for this, `analysis/idb/r5apex_dx12_s21.exe.i64`)

Each `ReadFromBuffer` was decompiled in both IDBs and compared on the bit widths it reads in order (inlined
`ReadUBitLong`) and on the constant arguments of its out-of-line calls (strings, byte blocks). 101 of 104
reader/writer pairs read the same widths. The out-of-line calls differ in a pattern that is not wire format:
`sub_5B2400(table, 22 | 29, value)` is a statistics histogram update on the reader's overflow path (bucket 22 in
S21, 29 in S30). Real format changes:

| S21 → S30 | message | change | dedi sends it? |
|---|---|---|---|
| 19 → 20 | **svc_Print** | S30 reads a **1-bit flag first**, then the same 4-bit prefix and string (S30 `ReadString` max 2048) | **yes**, via the bridge's Print format bridge (dedi 16 → S21 19): the S30 bridge must write the extra bit |
| 37 → 38 | svc_AntiCheat | 1024-byte string → 1 bit + 8 bits + 512-byte string | no (consumed and dropped) |
| 42 → 43 | (S21 42) | new fields, string sizes | no |
| 45 → 46 | (S21 45) | first string 128 → 48 bytes | no |
| 5 → 5 | net_SetConVar | value buffer 2048 → 4096 (same wire format) | yes, unchanged |

Everything else the dedi sends reads the same in S30 as in S21, so the S30 bridge keeps the S21 bridge's
translation and renumbers.

## Dedi (S3) → S30, server-to-client

Composed from the bridge's `s_S3ToS21` (`net_bridge_install.cpp`) and the S21 → S30 rule above.

| dedi | message | S21 | S30 | note |
|---:|---|---:|---:|---|
| 0..3 | NOP, Disconnect, reserved, StringCmd | 0..3 | 0..3 | |
| 4 | net_SetConVar | 5 | 5 | |
| 5 | net_SignonState | 6 | 6 | S21 format bridge (22-bit extra fields) |
| 6 | net_MTXUserMsg | 7 | 7 | |
| 7 | svc_ServerInfo | 11 | 12 | S21 format bridge; S30 reader = S21's |
| 8 | svc_SendTable | 12 | 13 | |
| 9 | svc_ClassInfo | 13 | 14 | |
| 11 | svc_Playlists | 14 | 15 | |
| 12 | svc_CreateStringTable | 15 | 16 | S21 format bridge (24-bit length) |
| 13 | svc_UpdateStringTable | 16 | 17 | |
| 14 | svc_VoiceData | 17 | 18 | |
| 15 | svc_DurangoVoiceData | 18 | 19 | |
| 16 | svc_Print | 19 | **20** | format bridge **plus S30's leading bit** |
| 17 | svc_Sounds | 20 | 21 | S21 format bridge |
| 18 | svc_FixAngle | 21 | 22 | |
| 19 | svc_CrosshairAngle | 22 | 23 | |
| 20 | svc_GrantClientSidePickup | 23 | 24 | |
| 22 | svc_ServerTick | 24 | 25 | S21 format bridge |
| 23 | svc_PersistenceDefFile | 25 | 26 | |
| 24 | svc_UseCachedPersistenceDefFile | 26 | 27 | |
| 25 | svc_PersistenceBaseline | 27 | 28 | |
| 26 | svc_PersistenceUpdateVar | 28 | 29 | |
| 27 | svc_PersistenceNotifySaved | 29 | 30 | |
| 29 | svc_MatchmakingETAs | 31 | 32 | |
| 30 | svc_MatchmakingStatus | 32 | 33 | |
| 31 | svc_MTXUserInfo | 33 | 34 | |
| 33 | svc_SetTeam | 35 | 36 | |
| 40 | svc_Snapshot | 50 | 53 | re-encoded by the bridge |
| 41 | svc_TempEntities | 48 | 51 | S21 format bridge (24-bit length) |

Consumed or dropped by the bridge as on S21: 10, 21, 28 (DLCNotifyOwnership body-skip), 32 (PlaylistChange),
34..36, 38, 39, 42 (Menu), 43, 44 (DatatableChecksum); 37 (UserMessage) and 66..68 (SDK messages) go through the
bridge's own paths.

## Client-to-server (`clc_*`)

Found by scanning `.rdata` for message vtables (`tools/pe_netmsg_vtables.py`: all share base slots 1, 2, 6, 8, 9).
S21 has 32 not registered by `ConnectionStart` (types 55..86), S30 has 32 (62..95). Aligned by order and size
and checked on the decompiled `WriteToBuffer`:

| S21 | S30 | rule |
|---|---|---|
| 57..63 | 64..70 | +7 (includes **clc_Move 57 → 64**, writer identical; **ClaimClientSidePickup 63 → 70**) |
| 65, 66, 68 | 72, 73, 75 | +7 (**clc_ClientTick 65 → 72**, **clc_ClientSayText 66 → 73**) |
| 70, 71 | 76, 77 | +6 (S21 69 has no S30 counterpart) |
| 72..86 | 80..94 | +8 (S30 inserts 78, 79; S30 95 is new) |

Format changes in messages the bridge sends:
- **clc_ClientTick** (S30 72): three signed 32-bit values (31 bits + sign each) instead of two; the object grew
  from 40 to 48 bytes. The S30 bridge's C2S path has to read three and re-encode the S3 message.
- S21 62 → S30 69 adds a 256-byte string, a 32-bit count and N 16-bit values (not relayed by the bridge today).

`clc_Move`'s writer is identical, but the user commands it carries are delta-encoded elsewhere
(`CL_SendMove`); that encoding is still to be compared.

## Tools
- `tools/pe_netmsg.py <exe> <count offset>`: finds `RegisterMessage` by its count check and `ConnectionStart` as its
  main caller, and lists every registered message (vtable, `GetType`, `sizeof`, slots). No IDA needed.
- `tools/ida_netmsg.py`: the same from an IDB, given the two functions.
- `tools/netmsg_match.py`: aligns two dumps by order and size.
- `tools/ida_decomp_dump.py`, `tools/netmsg_bodies.py`: normalized Hex-Rays output of each reader/writer, compared
  on the sequence of bit widths read.

Outputs: `analysis/s21_connectionstart_msgs.txt`, `analysis/s30_connectionstart_msgs.txt`,
`analysis/b1_netmsg_match.md`, `analysis/netmsg_decomp/{s21,s30}/`.
