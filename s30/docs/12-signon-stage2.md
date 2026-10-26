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

## Step 4: tolerant decoders, precache sync (signon reaches SPAWN)

| | S30.1 |
|---|---|
| decoder builder mismatch exit | `0x256440+0xA4` (`xor al,al; jmp end`), next prop at `+0x217`: patched to `jmp next` (S21's DecodeAbort patch) |
| per-decoder finisher | `0x255F30`, called by CreateDecoders after the props are set; CRecvDecoder: root RecvTable `+0`, SendProp* `+0x18`, RecvProp* `+0x4080` (count `+0x4098`), owner `+0x40A0`; unmatched = NULL prop, owner unset |
| prop type functions | table `0x1B49350`, 6 per type (decode `+0x10`, decode-zero `+0x20`); Int decode `0x252A30` writes through the RecvProp proxy (`+0x48`) when there is one; array decode `0x253F70` steps by the RecvProp's element (`+0x38`) and stride (`+0x58`) |
| RecvTable list insert | `0x2639D0` (CUtlLinkedList InsertBefore of `0x1B59968`; 5 template copies, picked by the list it loads) |
| `RecvTable_Decode` / BuildEntityDecode `0x308770` | a class whose RecvTable has no decoder is fatal |
| PrecacheWeapon `0x8B63C0`, PrecacheImpactEffectTable `0x8A9310` | a name missing from the server's WeaponNames crashes on removal (`0x75B20B`, NULL settings); a full ImpactEffectTableNames is a fatal Error |

- Unmatched SendProps get dummy RecvProps (the SendProp's type, no-op proxy; arrays an element dummy) and the
  decoder's root as owner; the dedi's 21 tables S30 lacks get stub RecvTables registered through the engine's own
  list insert, so they have decoders, and the stub ClientClasses use them.
- Client precaches of weapons / impact tables the dedi lacks are skipped (logged). `s30/tools/dedi_precache_sync.py`
  writes the weapons into a SERVER block of the dedi's `sh_weapons.gnut` (8: S30 abilities whose server script the
  S21-era dedi does not have; WeaponNames 252/256). The 41 impact tables would not fit (222 + 41 > 256): skipped.
- Result: the engine walks CONNECTED → NEW → PRESPAWN → SPAWN. Next is the snapshot (svc_Snapshot, S3 40).

## Crashes fixed (2026-10-26)

- **Client from `launch_client_dx12.bat`**: exited 9 s in, `Failed to load playlists` (fatal) -- the `.bat` passes
  `-playlistFile playlists_r5_patch.txt`, which the S30 depot does not have. `deploy_s30_client.ps1` now copies the
  dedi's `platform/playlists_r5_patch.txt`.
- **Dedi ~1 s after `ready`** (every time with `+launchplaylist survival_firingrange`, about 1 in 6 with `+map`): a
  return to address 0 on the main thread with a zeroed stack. Bisected through the staging area scripts to the firing
  range vending machine: `prop_loot_grabber` with the death box model (`mdl/props/death_box/death_box_01.rmdl`,
  `SOLID_VPHYSICS`) set to `MOVETYPE_FLY`; the loot bin model crashed the same way, a raised (out of contact) death
  box did not. The death box `.rmdl` collision and `.phy` are sound (`.phy` byte-identical to S21's except the
  checksum) and so is the world BVH (lumps 0x12/0x13 byte-identical to the S30 source, same child types as the S21
  and S3 maps; no Apex map stores lump 0x14). The FLY sweep traces the static props around it: 184 converted models
  -- the firing range loot mats under the vending machine among them, 44 props on this map -- had **no BVH nodes**.
  rmdlconv sized the last collision part's node array from the smallest studiohdr offset past the blob; an empty
  region keeps a stale offset (`projectile_rocket`'s `uiPanelOffset` lands 8 bytes into the collision model), so the
  array became 0 bytes and the header pointed past the model. The S30 sources do have the nodes (all 2959 with
  collision store exactly the nodes reachable from node 0, up to EOF). rmdlconv (`tools_src`, `rmdl_160.cpp`
  `V160LastPartNodeBytes`) now sizes the array by walking the tree, which also drops the tail data that used to ride
  along (the death box's ref animation, 14 bytes). `s30/tools/dedi_coll_gate.py` walks every converted model the way
  the engine does and gates `dedi_paks.py models` (S21's 1981 shipped models pass; the old build failed 1349 of
  3300). With the paks rebuilt and FLY restored: 3/3 firing range and 4/4 `+map` starts stay up.

## Step 5: after SPAWN (2026-10-26)

- **Mod policy kick.** Right after SPAWN the dedi's mod policy gate (`mod_policy_gate.cpp`, `sv_modPolicy 1`) read
  the client's `sdk_mods` userinfo, failed to parse the bridge's bare `-` and disconnected the client
  (`#SDK_MODS_POLICY`), so no snapshot ever came. The value is `<ids>#<FNV-1a 64 of ids>` (`ModSystem_ParseAttestation`);
  the S30 client loads no SDK mods: `-#af63a04c86018698`. The dedi now logs `[MOD-POLICY] accept slot=0`.
- **The first snapshot is a DataBlock.** The dedi paces bridge clients (`[FULL-PACE]`): one NoDelta FULL, then
  keepalive until the client acks that tick (30 s safety release). The FULL arrives as a DataBlock transfer
  (ServerTick 22, S3 13, svc_Snapshot 40), not as split netchan packets (none arrive; S30's own split reassembly,
  `NET_ReceiveSplit` 0x2A5650, takes S3's layout with split size 564..1188). The bridge dispatches a DataBlock only
  with the next netchan packet, and the dedi sends none while it waits for the ack: the snapshot is never dispatched.
- **pdef.** S30's client scripts register stats for S30's seasons, ranked periods and S30-only stat structs. The
  dedi's pdef got S30's enum members (see docs/10: per-mode stats trimmed to the playlists' modes to stay under 26,000
  entries). The S30-only structs do not fit the dedi parser's 40 struct types; the dedi's scripts never write them, so
  `bridge_persistence_s30.cpp` gives `PersistenceGetVarHandle` (0xC58BF0) a dummy handle for names the pdef lacks
  (the handle is only stored, `sh_stats_internals.gnut` 1430) and logs each as `[PDEF-MISSING]` (console: one line per
  path). First run: 121 names in `stats.arena_seasons`, `characterClasses`, `rankedEventPeriods`,
  `narrativechallenge`. To revisit with the dedi's struct cap.
- Result: the client loads into the Firing Range loading screen with no script error and waits for the snapshot.

## Step 6: the first snapshot (2026-10-26)

| | S30.1 |
|---|---|
| snapshot message | type 50 (216 bytes, vtable 0x17F24D8): reader 0x48AD70, Process 0x2F9280. S21's is type 47 (reader 0x44BD80, the SDK's `SvcSnapshotReadFromBuffer`); the 552-byte class registered next to it (S30 53, S21 50) reads a 64-bit value and a string (the S21 bridge's `SvcSnapshotWrongReader` matches S30 0x474410) |
| header | the S21 reader's: ServerTickInfo helper (S21 0x446700, S30 0x4858E0: same field stores, the last 32-bit read only laid out differently), then the same 26 reads (15, 14, 24-bit widths) and a seek past the entity payload |
| UpdateStringTable | S30 17 = S21 16 ([5 id][1 many][16 count][20 length]), passed through |
| `CopyNewEntity` create call | 0x2EF0B4: `call [ClientClass +0](entnum, serial)` through the server-class table |

- A completed DataBlock queues a 16-byte wake datagram for the engine socket; the ProcessPacket detour dispatches the
  DataBlock for it on the engine thread (the dedi sends no netchan packet while it holds the FULL).
- S3 40 is re-encoded as the S21 bridge does (ServerTickInfo 137 → 152 bits, length 22 → 24) and run as S30 50;
  every C2S packet then carries S3 clc_ClientTick (type 60, [32 tick][32 raw tick]) for the last processed tick.
- Stub ClientClasses had no create function: the first snapshot called NULL (0x2EF0B4, entnum 883). They now borrow
  `CInfoTarget`'s (+0 create, +8 create-event).
- Result: `Snapshot #1 tick 562 full, 148700 payload bits, ok 1`; the dedi logs `[FULL-PACE] released slot=0:
  client acked FULL tick=562 -- deltas resume`; the engine walks to signon 8 (FULL).
- Then the **dedi** stops: on the player's match start its S21-era scripts index S30 data they do not know
  (`sh_battlepass.gnut` 2190 `battlePassMap[pass]` for S30's active pass; `sh_items.gnut` 872
  `guidItemFlavorMap[1600822881]` = `xp_boost/battlepass_season30_01`), "There was a problem processing game logic",
  `CHostState::FrameUpdate: Shutdown host game`.
- Client side, `GetPersistentVar` reads of S30-only vars still raise (UI `sh_inbox.gnut` 621 `inboxMessages[0].id`,
  client `mastery_weapons[...].trial_list_v2[0].stat`): only `PersistenceGetVarHandle` gets a default so far.

## Step 7: in game (2026-10-26)

- Client: `GetPersistentVar` (0xC34940) / `GetPersistentVarAsInt` (0xC349E0) helpers check the name first (string
  arg 2 via 0xF64F10, the interface lookup); a name the dedi's pdef lacks is logged `[PDEF-MISSING] ... read as 0`,
  int 0 is pushed (0x3A8A40) and they return 1. First run: `inboxMessages[0..]` (UI), `mastery_weapons[...]
  .trial_list_v2[...]`.
- Dedi (S21-era scripts against S30 data; originals `.s21`):
  - `platform/playlists_r5_patch.txt` Gamemodes defaults: `enable_battlepass 0` (S30's active pass is not in
    `sh_battlepass.gnut`'s `battlePassMap`; the client gets the same file).
  - `sh_perks.gnut`: role perks without a `PerkInfo` on the dedi (S30 perks 12, 18, 20) are left out and printed.
  - `sh_passives.gnut`: the dedi's `ePassives` has 214 entries (S30 names added), the S3 engine 128 passive bits
    (`CPlayer` `m_passives`, 2 x 64; "Passive N out of max range [0-128)"): `GivePassive` / `TakePassive` /
    `PlayerHasPassive` ignore and print passives >= 128 (S30 character setup grants 195, 197), the `TakeAllPassives`
    and DEV loops stop at 128.
- Result: snapshots stream (586 S2C packets in 30 s, deltas acked), no script error on either side, and the client
  is in the Firing Range spawn room with the HUD (`tests` screenshot in the session).
- Open: C2S carries no `clc_Move` yet (the engine's own packets are dropped), so the player cannot move; S3 68 (an SDK
  message) in reliable transfers has no handler and drops the rest of its transfer; TempEntities (S3 41) and
  UserMessage (S3 37) are not bridged.

## Step 8: input (2026-10-26)

| | S30.1 |
|---|---|
| `ClWriteMoveCmds` | 0x2F5320 (S21 SDK signature): clc_Move on the stack (vtable `lea` at +0x5D: 0x17F0430), backup +0x20, new +0x24, bf_write +0x70 (current bit +0x80); first command against a reset command; sent with `CNetChan::SendNetMsg` 0x2A03D0 |
| usercmd delta writer | client vtable 0x1891008 slot 31 → 0x8AB720 (thunk) → input vtable 0x18DC508 slot 6 → 0x9B5BA0 → `WriteUsercmd` 0xB9E820, reset 0xB9E560 (read from the running client: no RTTI names in the IDB) |
| `WriteUsercmd` vs S21 | S21's order except: no spring roll, no impulse, weaponCustomActivity 12 bits (S21 11), the vector group after attack focus only written when focus is, a third vector group, a bit before frametime; normalized float (0xB9DD00), signed delta (0xB9DC80, N-bit two's complement) and reset values are S21's |
| tick interval | ServerInfo `f11` = 0.05 |

`bridge_usercmd_s30.cpp` hooks `SendNetMsg`; for the clc_Move vtable it parses each command in S30's format and emits
the dedi's (the S21 bridge's `S21Bridge_EmitS3Cmd` with its default levers: real tick count, command time from the
tick interval, ping remap, frametime restamp, energize/akimbo in impulse's top bits), and queues S3 clc_Move (46,
[4 new][3 backup][16 bits]) for the next C2S packet (flushed at once). Every batch parses to exactly its bit count.
The dedi drains the command queue (`[CMDQ] FIRST FIRE`); `+forward` for 2.5 s walked the player down the spawn
corridor.

S2C added: S3 68 (`net_ScriptMessage`, the dedi SDK's ScriptRemote lane) is consumed and its call names logged
(`ServerToClient_OnPersistentDataReady`, `ServerCallback_YouRespawned`, `ServerCallback_UpdateHudWeaponData`, loadout
and challenge updates, ...) -- run on the S30 VMs in step 9; S3 18 (`svc_FixAngle`) passed to S30 22 (same reader).

## Step 9: server-to-client script calls (2026-10-26)
`src/core_s30/bridge_scriptremote_s30.cpp` runs the S3 68 call frames on the S30 VMs, as S21's
`S21Bridge_InjectScriptRemote` does:

| S30.1 | |
|---|---|
| `0xF594F0` FindFunction(vm, name, signature, scope) | a null signature skips the type check (`0xF68A73`) |
| `0xF597E0` ExecuteFunction(vm, hFunc, args, n, ret, scope) | engine callers pass a zeroed return variant, null scope |
| ScriptVariant_t | 16 bytes, type int16 at +14: float 1, vector 3, int 5, bool 6, cstring 0x24, hscript 0x25 |
| CLIENT / UI CSquirrelVM* | `0x4073D28` (loaded before `ClientCodeCallback_MapSpawn`), `0x400A490` (`UICodeCallback_RunHeadless`) |
| `0xAD32B0` | stores a `Remote_RegisterClientFunction` / `Remote_RegisterUIFunction` registration: (this, hFunc, name, flags 2*bUI+1, schema, schema bytes, params); only caller `0xAD4864` |
| schema (`0xAD3550`) | per param a type byte: float 1 / vector 3 + min, max, bits; int 5 + min, max; bool 6; entity 0x2A; typed_entity 0x2B + class; itemflavor 0x2C + kind + 1-2 ints. No string type |
| entities | `GetEntityFromEncodedEHandle` `0xC0C5D0`: table `0x58D4A18`, 32-byte slots (entity, serial +8); `GetScriptInstance` `0xAC5150` |

A script error on the CLIENT VM is fatal ("There was a problem processing game logic"), so a call only runs when it
matches the S30 registration: name, param count, types (dedi entity/typed_entity 0x28/0x29 = S30 0x2A/0x2B) and the
registered ranges. A mismatch is dropped and logged once per name and reason (`[SCRIPT-REMOTE] <name>: ... --
dropped`). Calls wait for the snapshot tick they were stamped with (at most 64 snapshots), in order.

Dedi script differences this found, fixed on the dedi (originals `.s21`):
- Heal items: the S30 client reads them as its `eHealthPickupType` (ULTIMATE 7 after CLOCKUP_SYRINGE 5,
  MED_WHEEL_CONSUMABLE_END 6; SHIELD_LARGE 2, SHIELD_SMALL 3), the dedi sent `eConsumableType.ULTIMATE` 5 and its
  `eHealthPickupType` swapped the shields. `s30/tools/dedi_heal_enums.py` gives both dedi enums the S30 layout. The
  dedi's note that autoplayers (bots, native `HealingItem`) depend on the first five keeps holding for
  `eConsumableType`; the dedi `eHealthPickupType` shield order now follows S30.
- `ServerToClient_AssignedChallengesDidChange`: S30 takes `bool didEventChallengesRefresh` and has no
  `_FromEventsRefresh` twin. `sh_challenges.gnut` registers the bool and passes `false`, or `true` where it called the twin.

Result: 33 calls run on connect (persistent data ready, loadout batches, respawn, HUD weapon data, charge times,
ultimate ready, ...), no script errors, the client stays in game. Dropped: `FS_ByRef_ConsistencyCheck` (a Flowstate
client script S30's client scripts do not have).

## Step 10: the rest of the protocol (2026-10-26)
Inventory and status in docs/13; specs in docs/14–16.
- S2C: user messages (`bridge_usermessage`), TempEntities (S30 51), native passthrough of 14, 15, 17, 19, 20,
  29–31, 33, chat lanes 67/70 and PlaylistChange 32 (`bridge_hud`). Stub ClientClasses keep a null create-event
  function so dedi-only temp entities are skipped.
- C2S (`bridge_c2s`): each message the engine sends is serialized with its own WriteToBuffer (body only; S30's
  bf_write ORs bits into dwords, so the buffer starts zeroed) and queued in the bridge's C2S stream with the
  dedi's dedupe sequences. Client script calls go by name; the dedi's server.dll got the matching receive path.
- Activities: the dedi translates to S30 ids (`+bridge_client_set s30`), the client translates the
  queued player activity; weapon RNG seed matched (`bridge_cmdseed`).
- Test helper: `sdk_script_call <client function>` runs a no-argument CLIENT VM function on the engine thread.

## Next
Still consumed without effect: 34 PlaylistOverrides, 69 DebugOverlay. Earlier list, kept for history: 37
UserMessage (S21 maps ids), 41 TempEntities (S21 24-bit length bridge; stub TE classes have no create-event function),
34 PlaylistOverrides, 44 DatatableChecksum. C2S: StringCmd (console commands), ClientSayText, SetConVar relays.
