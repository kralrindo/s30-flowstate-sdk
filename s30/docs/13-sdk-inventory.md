# 13. What the S21 client SDK does for the S3 dedi, and where the S30 port stands (2026-10-26)

Source: the user's S21 SDK (`tools/s21-r5sdk`, read-only reference): `src/engine/client/net_bridge_*.cpp`
(29k lines), `src/game/client`, `src/game/shared`, `src/vscript`. Each row is one S21 behaviour the S3 dedi relies on,
with the S30 status. Diagnostics-only levers (`*_diag`, `*_probe`, `*_trace`, `*_dump`, `*_log`) are not listed.

Status: **done** (in `src/core_s30`, verified in game), **todo**, **n/a** (S30 does not need it; reason given).

## A. Protocol: server to client (S3 message → S30)

| S3 | message | S21 handling | S30 |
|---:|---|---|---|
| 1 | net_Disconnect | reason, disconnect | done |
| 3 | net_StringCmd | native | done |
| 4 | net_SetConVar | replicated non-cheat only | done |
| 5 | net_SignonState | format bridge | done |
| 6 | net_MTXUserMsg | consumed | done |
| 7 | svc_ServerInfo | format bridge | done |
| 8, 9 | SendTable, ClassInfo | stub RecvTables, table renames | done (renames: todo if a table needs one) |
| 10 | svc_SetPause | consumed | done |
| 11 | svc_Playlists | native | done |
| 12, 13 | Create/UpdateStringTable | format bridge, in-order ids | done |
| 14, 15 | VoiceData, DurangoVoiceData | native (renumbered) | done (S30 18, 19; not seen yet) |
| 16 | svc_Print | format bridge (+1 bit on S30) | done |
| 17 | svc_Sounds | format bridge (bounded copy) | done (S30 21, same reader; not seen yet) |
| 18 | svc_FixAngle | native | done |
| 19 | svc_CrosshairAngle | native | done (S30 23) |
| 20 | svc_GrantClientSidePickup | native | done (S30 24) |
| 22 | svc_ServerTick | format bridge | done |
| 23..27 | persistence | pdef load, gate | done |
| 28 | DLCNotifyOwnership | body skip | done |
| 29..31 | Matchmaking ETAs/Status, MTXUserInfo | native | done (S30 32..34) |
| 32 | svc_PlaylistChange | apply by name (load screen) | done (`bridge_hud`: Playlists_SetCurrentByName 0x3DD340; verified) |
| 33 | svc_SetTeam | native | done (S30 36) |
| 34 | svc_PlaylistOverrides | decoded by the bridge, allow-listed vars | todo (consumed; S30 by-name var getter unproven, docs/16) |
| 35, 36, 63 | AntiCheat | never processed | done (skipped) |
| 37 | svc_UserMessage | 5 direct handlers: SayText, WeapProjFireCB, PlayerNotifyDidDamage, RemoteBulletFired, RemoteWeaponReload | done (`bridge_usermessage`, handlers by HookMessage name; others consumed + logged) |
| 40 | svc_Snapshot | re-encode | done |
| 41 | svc_TempEntities | 22 → 24-bit length | done (S30 51; stub classes have no create-event, so unknown events are skipped) |
| 42, 43 | Menu, CmdKeyValues | body skip | done (skipped) |
| 44 | svc_DatatableChecksum | consumed (bridge changes tables) | done (skipped) |
| 66 | svc_SetClassVar (SDK) | two strings, consumed | done (consumed) |
| 67 | svc_SystemSayText (SDK) | chat system message | done (`bridge_hud`: PrintSystem 0x9A2000; display not verified) |
| 68 | net_ScriptMessage (SDK) | S2C remote calls, MantleBoost and Portal frames | calls done; MantleBoost/Portal todo (section D) |
| 69 | svc_DebugOverlay (SDK) | debug draw | done (consumed; dev draws) |
| 70 | svc_ChatBuilder (SDK) | rich chat | done as plain text through PrintSystem (colours todo; display not verified) |

## B. Protocol: client to server

| message | S21 handling | S30 |
|---|---|---|
| net_SignonState, userinfo, clc_ClientTick, clc_Move | bridge-built | done |
| net_StringCmd (S30 3 → S3 3) | sequenced + redundant sends, dedi dedupes | done (`bridge_c2s`, "brq"; verified: say, noclip) |
| net_SetConVar runtime (S30 5 → S3 4) | userinfo mirror, resend | done (split to fit, values ≥ 260 bytes dropped) |
| client ScriptRemote (`Remote_ServerCallFunction`, S30 4) | S21: verbatim by function index → S3 68 | done by name: "C2RS" frame, dedi `ScriptRemoteServer_ProcessNamedMessage` (docs/15); verified |
| clc_ClaimClientSidePickup (S30 70 → S3 57) | loot pickups | done (not exercised yet) |
| clc_ClientSayText (S30 73 → S3 61) | chat, sequenced | done ("brc"; not exercised yet) |
| clc_GamepadMsg (S30 82 → S3 65) | gamepad state | done (S30's extra 32 bits dropped) |

## C. Entity and animation compatibility (S3 data on a newer client)

| S21 | what | S30 |
|---|---|---|
| `activity_s3_to_s21*` | activity ids S3 → client | done: dedi `+bridge_client_activity_set s30` (964 mapped), client queued-activity hook 0x619E30 (docs/14) |
| `weapstate_s3_to_s21.h`, `weapon_customact_c2s_xlat` | weapon state / custom activity ids | custom activity: done with the activity set; weapon state: S30 order unproven (docs/14) |
| `offhand_slots_ext`, `weapon_dualwield_slot_patch`, `offhand_*` | offhand slot layout | todo |
| `scriptnetdata_ext`, `dt_extend_*` | script netvar / datatable extensions | todo |
| `bridge_anim_*`, ActivitySelector, SetSequence, StudioFrameAdvance hooks | animation fixes | todo |
| PostNetworkDataReceived, TransitionEntity, DestroyBaselineEntity | entity lifecycle guards | todo |
| ClockDrift, ClSendTick, ProcessSnapshotDispatch, HostFrame | clock and tick fixes | todo |

## D. Flowstate gameplay (client half of server.dll features)

Each needs the client prediction to match what the dedi's server.dll does: mantle boost, portals, ziplines
(`c_zipline`, `zipline_disconnect`, ziprail), wall launch, slope launch, double jump, glide, jump pads, trigger
cannon, dodge, wallclimb, titan gate, source push, melee (lunge, attack state), weapon charge / heat / mods /
legendary, akimbo, aim assist, autobunnyhop, noclip prediction, deathfield, highlight, status effects. Status:
todo, one at a time.

## E. Script side

| S21 | what | S30 |
|---|---|---|
| S2C remote calls | | done (`bridge_scriptremote_s30`) |
| script error log, print | | done |
| `classvar_natives`, `vscript_client`, `vscript_player`, `demo_natives`, `fs_1v1_convars` | natives the Flowstate client scripts call | n/a while S30 runs its own client scripts from rpaks; todo when Flowstate client scripts are ported |
| `hud_basechat`, chat mute | chat | todo with UserMessage SayText |

## Done after the inventory (2026-10-26)
- Weapon RNG seed: `bridge_cmdseed` hooks S30 `ComputeRandomSeed` 0xBA5900 to the dedi's command-number seed.
- Chat, playlist lanes: `bridge_hud`.
- Research: docs/14 (activities), 15 (C2S relays), 16 (gameplay features), 17 (mp_lobby).

## Order of work
A and B first (everything else rides on them), then C (what makes S3 entities look and move right), then D one
feature at a time, checked in game against the dedi.
