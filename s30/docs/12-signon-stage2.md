# 12. Server connection, stage 2: the dedi hands the S30 engine its connection (2026-10-26)

Stage 1 (docs/09) left the dedi with the client connected and the S30 engine idle. Stage 2 moves the S30 engine into
the connection the way the S21 bridge does on S21: the engine runs its own native connect and builds its own
netchannel; the bridge only feeds it the replies it expects.

## How S21 does it (reference)

The S21 bridge rewrites the dedi's replies on the engine socket into the S21 engine's native connectionless type
`0x04` (`ffffffff 04 <map>\0 ...`). The S21 dispatcher (`0x3BE9F0`, case 4) runs `FullConnect` and starts the level
load; the bridge then drives `SetSignonState(2)` and replaces `CNetChan::ProcessPacket`.

## S30 facts (verified in the S30 IDB)

| | S30.1 | S21 |
|---|---|---|
| connectionless dispatcher | `0x3EF530` (cases 10, 11, 12, 13, 14, 22, 78) | `0x3BE9F0` (2, 4, 5, 6, 9, 23, 78) |
| "server map" reply | **case 12**: three 256-byte strings + 1 bit; `FullConnect`, `maps/%s%s.bsp`, level load | case 4 |
| `FullConnect` | `0x2FEDF0`: `NET_CreateNetChannel(cl+96 socket, adr, "ClientToServer", cl)` (`0x2A5430`), netchannel at `cl+104` | `cl+0x60` |
| client state fields | signon `+196`, server adr `+136` type / `+140` address / `+156` port / `+159` flag, level file `+412`, base `+476` | 32 bytes earlier |
| client state array | `0x1E1BD60`, stride `0x69378`, vtable `0x17F1828` | |
| `NET_ReceiveDatagram` | `0x2A5A90`: `recvfrom`, sender from `sockaddr_in6` into the netpacket (`+0` type 3, `+4` address, `+20` port) | |
| `NET_DecodeDatagram` | `0x2A5870(sock, packet, bEncrypted, buf, len, pDrop)`: decrypts a 28-byte header when `bEncrypted` | |
| `NET_ProcessSocket` | `0x2A5F80`: `bEncrypted = 1` on the client sockets unless a Valve relay runs with `valve_relay_encrypt_traffic 0`; a `-1` packet goes to `handler->vtable[1]` | |
| connectionless wrapper | `0x3F2FB0`: types other than 11, 13, 22, 78 reach the dispatcher directly only if `cl_strict_transport_checks`, `net_encrypt_dtls` or a flag is off; otherwise an undecrypted packet from a non-loopback sender is dropped (`dtls...violation`) | |

Case 12 needs signon 1 (the native connect sets it) and the sender equal to the server address. After a native
`connect 127.0.0.1:37015` the client state holds type 3, `::ffff:127.0.0.1`, port 37015, flag 0.

## The bridge's part (`src/core_s30/bridge_signon_s30.cpp`)

`bridge_connect host:port` now does both:

1. **The engine connects natively** to the same address: `net_usesocketsforloopback 1` (without it a `127.0.0.1`
   connect never touches a socket), `cl_strict_transport_checks 0` (read only by the wrapper above), then `connect`.
   Both ConVars are DEVELOPMENTONLY in S30.1; the bridge clears that flag on these two only, and restores the
   values on `bridge_disconnect`.
2. **Its datagrams to the dedi are dropped** in the `sendto` hook (encrypted S30 challenges, later netchannel
   traffic); the hook learns the engine's socket from them. The bridge's own sends are left alone.
3. **The S3 handshake** runs on the bridge socket as in stage 1.
4. **On CONNACCEPT** the bridge queues `ffffffff 0C <map>\0 \0 \0 00`; the `recvfrom` hook returns it on the engine's
   socket with the dedi as sender, and the `NET_DecodeDatagram` detour treats datagrams from the dedi as plaintext.

`sdk_clstate` prints client state 0 (signon, server address, netchannel, level) and the bridge's routing counters.

## Result (`tests/E6_signon`)

One `bridge_connect 127.0.0.1:37015` against the workspace dedi:

```
[SIGNON] engine socket to the dedi: 1600
[SIGNON] engine datagram to the dedi dropped (78 bytes, #1)        <- native challenge
[BRIDGE] CONNACCEPT: map 'mp_rr_canyonlands_staging_mu1' mode 'survival'
[SIGNON] delivered a 38-byte datagram to the engine socket
[SIGNON] datagram from the dedi taken as plaintext (sock 0, 38 bytes, #1)
client state 0: signon 2 netchan 000002B96E179FA0
  server adr type 3, ::ffff:127.0.0.1 port 37015, level 'maps/mp_rr_canyonlands_staging_mu1.bsp'
```

The client shows the Firing Range loading screen (`tests/E6_signon/connected.png`). The engine's netchannel now
sends to the dedi (1,228- and 256-byte datagrams, about 120 a second), dropped for now.

## Notes

- Without `net_usesocketsforloopback 1` the native connect stays on the in-process loopback path: no `sendto`, no
  socket for the bridge to answer on.
- Without `cl_strict_transport_checks 0` the type-12 reply is decoded but dropped by the wrapper; signon stays 1
  and the engine keeps resending its challenge.
- The workspace dedi crashes (call to address 0) seconds after `ready` when started with `+launchplaylist
  survival_firingrange`; started like the user's `launch_server.bat` with `+map mp_rr_canyonlands_staging_mu1` it
  stays up. Not yet investigated.

## Step 2: the netchannel (`src/core_s30/bridge_netchan_s30.cpp`)

S30 facts used (S30 IDB):

| | S30.1 |
|---|---|
| `CNetChan::ProcessPacket(chan, packet)` | `0x29FF80`: `Seek(packet+56, 0)`, header `0x29EB10` (in seq → `chan+8`, out seq ack → `chan+12`), last received `chan+8432` = net time (`0x2801CD8`, loaded at `+0x96`), handler `chan+8568` slot 5 `PacketStart(handler, chan+8, chan+12)`, subchannel, `ProcessMessages`, slot 6 `PacketEnd` |
| netpacket | `+0` sender, `+48` data, `+56` bf_read, `+120` size; `NET_GetPacket` `0x2A5CB0` fills the bf_read (`+0x10` bits, `+0x18` bytes, `+0x28` in, `+0x30` end, `+0x38` data) then `Seek(0)` (`0x5E2C90`) |
| message array | pointer at `chan+0x2180`, count `+0x2198`; `RegisterMessage` stores `array[GetType()]` |
| `CClientState::SetSignonState(cl, state, spawn, msg)` | `0x2FF0C0` (called by the handler's ProcessSignonState `0x3F3F70`); states 0..10 named as S21 (9 MAYRECONNECT, 10 CHANGELEVEL) |
| svc_Print reader | `0x4741C0`: a leading bit stored at `msg+48`; ProcessPrint `0x3F4590` prints only when it is set |
| PersistenceDefFile reader | `0x47EEF0`: 64-bit version, 16-bit length, data (the dedi's format) |
| `Host_Error` | `0x4165E0`: formats into a stack buffer, `Host_Disconnect` (`0x271EE0`), then `longjmp`; now logged by `engine_log_s30` |
| ConVar flags | `sv_cheats` `0x82000`, `host_timescale` `0x6000`: REPLICATED `1<<13` and CHEAT `1<<14` as tier1 |

The detours take only packets whose sender is the dedi; everything else runs natively. S2C: S3 header, nonce, the
subchannel (reassembly, LZSS), then each S3 message through the docs/11 map: pass-through for StringCmd, Playlists,
PersistenceDefFile and the persistence messages; the S21 format bridges for SignonState (NEW deferred until
SendTables, C2S NEW sent at once), ServerInfo, ServerTick, SetConVar (replicated, non-cheat, S21's name filter) and
Print (leading bit 1); MTXUserMsg, SetPause, PlaylistChange and DLCNotifyOwnership (S21's auto rule: only when the
rest is `64 + N x 16` bits) consumed. The DataBlock is dispatched on the engine thread at the next packet. C2S is
the S21 bridge's packet: nonce ack, SignonState rungs (CONNECTED on CONNACCEPT, then each engine `SetSignonState`
2..8, echoed every 250 ms), UserInfo 3 times; flushed when the engine sends to the dedi (at most every 10 ms) and
every 250 ms from the bridge thread. `sdk_netchan` shows the counters.

Not done yet: the dedi's ack is not translated into `chan+12` (the engine sees out-ack 0), flow statistics are not
fed, UserInfo sends the S21 fallback values (not live ConVars), PlaylistChange/Overrides are not applied.

### Result (`20261026_092738.log`, workspace dedi restarted with `+map`)

```
[NETCHAN] server nonce 0x00011B91
[NETCHAN] DLCNotifyOwnership consumed (N = 194 balances)
[NETCHAN] S3 23 (PersistenceDefFile) -> S30 26: read 1 process 1, 126608 bits
[NETCHAN] S3 11 (Playlists) -> S30 15: read 1 process 1, 2469569 bits
[NETCHAN] S3 7 (ServerInfo) -> S30 12: read 1 process 1
[NETCHAN] S3 22 (ServerTick) -> S30 25: read 1 process 1
[NETCHAN] S3 4 (SetConVar) -> S30 5: read 1 process 1
[NETCHAN] S2C SignonState(3) deferred until SendTables; C2S NEW queued
[NETCHAN] DataBlock 0: not dispatched yet: 34(47) 12(883680) 12(102338) ... (17 CreateStringTable)
[NETCHAN] DataBlock 0: S3 8 at bit 0 not bridged yet          <- second transfer: SendTables + ClassInfo
[NETCHAN] S2C SignonState 4 -> 4 spawn 1 ...
[NETCHAN] replaying deferred SignonState(3) before 4
[ENGINE] Host_Error: CL_RegisterResources:  host_state.worldmodel/GetBaseLocalClient().GetModel( 1 )==NULL
```

The dedi acks the bridge's C2S and walks CONNECTED → NEW → PRESPAWN on it. The engine disconnects in NEW because the
model precache table never arrived: CreateStringTable is not bridged yet.

## Step 3: string tables, SendTables, ClassInfo

| | S30.1 |
|---|---|
| svc_CreateStringTable reader `0x488220` | S21's widths (16, log2, 24, 12, 4, 2; `netmsg_bodies.py`) |
| ProcessCreateStringTable `0x3F5D30` | compressed: `[uncomp:32][blob:32]`, blob `0xFFFFFFFD` + size + Oodle (`OodleLZ_Decompress` `0xFE5800`); uncompressed: parsed directly; Process true only if it consumed exactly the length |
| `OodleLZ_Compress` | `0xFE4E60` (linked; `oo2::OodleLZ_Compress`), Kraken 8 / SuperFast 1 as the S21 bridge |
| ProcessSendTable `0x3F57D0` | parse `0x256880`, then `RecvTable_RecvClassInfos` `0x251450`: with the needs-decoder bit, "No matching RecvTable for SendTable" fails it; skipped once handler+1096 is set (after CreateDecoders) |
| client RecvTable list | 16-byte nodes at `0x1B59968` (head `0x1B59980`), RecvTable name `+0x4C8`, compare ignoring case (`0x1416920`) |
| `CL_ParseClassInfo_EndClasses` `0x2FDB60` | server classes `+0x460`/`+0x468`; client list = client interface `0x28019C8` `+0x60` = `g_pClientClassHead` `0x2801718` (ClientClass: name `+0x10`, RecvTable `+0x18`, next `+0x20`, id `+0x28`); a missing class is a fatal `Error` (S21: a warning) |
| ProcessClassInfo `0x3F5840` | EndClasses, then CreateDecoders `0x257E30` ("CreateDecoders failed" → Host_EndGame) |
| CompareRecvPropToSendProp builder `0x256440` | every SendProp needs a RecvProp of the same name, type, inside-array flag and element count, else the decoder fails; RecvProp offsets as S21's (type `+0`, child table `+0x20`, name `+0x28`, array element `+0x38`, count `+0x5C`) |
| engine messages retail drops | `Host_Error` `0x4165E0`, `Host_EndGame` `0x4164D0`, the datatable warning `0x251220`: all logged by `engine_log_s30` now |

Done in the bridge:
- **CreateStringTable**: the S21 rewrite (LZSS → Oodle, 22 → 24 bits). All 17 tables of the first transfer process.
- **SendTable**: passed through; the 21 whose name has no S30 RecvTable go without a decoder (the needs-decoder bit
  cleared, S21's fallback): `DT_Beam`, `DT_BoneFollower`, `DT_PhysBox`, `DT_PlayerResource`, `DT_SpotlightEnd`,
  `DT_Sprite`, `DT_SpriteOriented`, `DT_BaseBeam`, the nine `DT_TEBeam*`, `DT_TriggerNoGrapple`,
  `DT_TriggerNoZipline`, `DT_TriggerSlip`, `DT_DoorMover`, `DT_Prop_Portal`.
- **ClassInfo**: passed through; EndClasses is wrapped so the 21 server classes S30 lacks (`CBeam` ... `CProp_Portal`,
  `DoorMover`) find a stub ClientClass (name + stub RecvTable named after the server's datatable) at the front of the
  client list for the engine's own matching; the list is restored afterwards (also on a longjmp). EndClasses passes.

### Result (`20261026_095754.log`)

CreateDecoders fails: 609 SendProps of the dedi do not match S30's RecvProps (logged as `[PROPS]` by a read-only
wrapper of `0x256440`): 528 with no RecvProp of that name (`m_shieldHealth`, `m_squadID`, `m_teamMemberIndex`,
`m_parentAttachmentType`, `m_smartAmmo*`, ...), 69 with another type (`m_modBitfield` int → datatable,
`m_classModsActive` int64 → array, ...), 12 with another array size (`m_bools` 32 vs 36, `m_int32s` 6 vs 8,
`m_entities` 22 vs 24). `DT_Player` alone has 81.

## Next
The decoders have to tolerate unmatched props, as the S21 bridge's do: there the decoder builder skips them, the
decoder gets a dummy RecvProp (`?_unmatched`, no-op proxy) instead of NULL, the WriteBlock paths skip writes for it,
and unmatched arrays are consumed element by element with the wire decoders. S30's decode/WriteBlock path has to be
mapped before porting that. Then PlaylistOverrides, UpdateStringTable, the snapshot.
