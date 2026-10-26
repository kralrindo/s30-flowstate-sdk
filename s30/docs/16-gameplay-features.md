# 16. Flowstate gameplay features and the SDK S2C lanes on S30 (research, 2026-10-26)

Scope: section D of docs/13 (the client halves of the dedi's server.dll gameplay features) and the S2C lanes
32, 34, 66, 67, 68 (non-call frames), 69, 70. Research only, no code changed. Sources: the S21 client SDK
(`tools/s21-r5sdk`, read-only), the dedi SDK (`sdk_port/r5sdk/src`, DEDICATED half), the dedi's scripts
(`build/s3_server/platform/scripts`), the S30.1 exe and IDB (copies under `analysis/idb/fork_gameplay`), and
read-only reads of the running S30 client's interface vtable. **Unproven** marks what was not shown from code.

## How to measure before porting a twin
The S21 twins exist because the dedi re-implements abilities S3 lacks, and the S21 client's prediction had to agree
with that re-implementation. S30 has most of these abilities **natively** (strings below), so the question per
feature is whether S30's native prediction agrees with the dedi. S30 ships the measuring tools already:
`cl_pred_error_verbose` (0x18FFEE0) and `cl_predict_error_icon_show` / `_threshold_dist` / `_threshold_angle`
(0x18E4A30..). Exercise one feature at a time with these on; port a twin only where it mispredicts.

## Ranked by player-visible impact

| # | feature | dedi state | S30 | verdict |
|---|---|---|---|---|
| 1 | **usercmd RNG seed parity** (`bridge_cmd_seed`) | `bridge_cmd_seed_parity 1`: seed = MD5_PseudoRandom(command_number) & 0x7FFFFFFF | native `C_UserCmd::ComputeRandomSeed` **0xBA5900** hashes command_number **plus (int)pitch%360, (int)yaw%360**, stores at cmd **+0x12C** (S21: +0x130) | **needed** -- every predicted spread/recoil roll diverges. Hook 0xBA5900 as S21 does (pattern below) |
| 2 | **mantle boost** (`mantle_boost*`, MBV1 lane) | `mantle_boost_enabled 1`, all players (default profile 1.0; Sparrow scales it). Publishes packed `m_mantleBoostState` (dt_extend appends it to DT_Player) and sends MBV1 frames | **native** mantle boost (`MayMantleBoost`, `canMantleBoost`, `mantleBoostJumpHeight`, `mantleBoostSprintSpeedMultiplier`, `mantleBoostSweetSpotAngleThreshold`, netvar `m_mantleBoostState`) | **needed, design decision**: see "Mantle boost" below -- the prop name collides and the meaning differs |
| 3 | **chat** (70 ChatBuilder, 67 SystemSayText) | `bridge_chat_bridge_render 1` (default): **every player chat line goes out as svc_ChatBuilder (70)**, not SayText | `CBaseHudChat` exists; PrintSystem **0x9A2000** | **needed** -- with the parent's current code chat is invisible. See "Chat" |
| 4 | **playlist name / overrides** (32, 34) | sent at signon and on change | `Playlists_SetCurrentByName` **0x3DD340**, current var getter **0x23E470** | needed for load-screen text and host settings; see lanes |
| 5 | skydive (`bridge_skydive_server_sim 1`) | dedi simulates freefall server-side | native skydive (99 strings, `CodeCallback_PlayerSkydive*`) | **measure** -- only matters in modes with a drop (the trimmed list mostly has none) |
| 6 | Sparrow: wall launch, double-jump power, mantle profile | natives `SetWallLaunchEnabled`, `SetDoubleJumpPowerEnabled`, `SetMantleBoostProfile` called from `pilot/sh_pilot_passive_sparrow.gnut` | double jump native (`CodeCallback_OnPlayerDoubleJump`, `disable_double_jump`); **no wall-launch string in S30** | **measure** with Sparrow; wall launch unproven on S30 (may be under another name) |
| 7 | glide (`bridge_glide 1`, drone rider passive), jetpack (`bridge_jetpack 1`) | dedi model | native glide (87 strings) and jetpack (68) | **measure** |
| 8 | armored leap, jet drive, skyward, slide gate, updraft, drag revive, shield-throw traversal | SDK natives used by the matching ability scripts | all native in S30 (`*ArmoredLeap*`, `BeginJetDrive`, `*Skyward*`, `m_slideGateDebounceExpireTime`, `Player_EnterUpdraft`, `Player_StartDragRevive`) | **measure** per legend; S21 had no client twin for these either |
| 9 | movement parity: dodge rules (`bridge_dodge_rules 1`), tap-strafe lurch cutoff (`bridge_lurch_cutoff_speed 1100`), zipline disconnect (`bridge_zip_disc 1`), zipline exit parity (`bridge_zip_exit_parity 1`), jump pads, gravity cannon, movement-ability input (IN_DODGE routing) | on | native dodge/tapstrafe/jumppad (`m_jumpPadDebounceExpireTime`)/gravity cannon (`m_gravityCannonLaunched`) | **measure**; S21 twins are the reference if one mispredicts |
| 10 | weapons: burst latch seed (`bridge_fire_tap`), mod bodygroups after RecalcMods (`weapon_mod_visual`), akimbo reload anims, dual-wield pairing (dedi patched to S21's N+7), offhand slots 6/7 | dedi patched to S21 layouts | native akimbo/energize/overheat | **verify**: S30 pairing offset and offhand slot count are **unproven** (S21 patterns do not match S30) |
| 11 | aim assist tuning (`aimassist`) | client-only | native | optional (controller feel, no parity issue) |

**Not needed** (proven off or unused in this workspace):
- autobunnyhop: `sv_autobunnyhopping 0`.
- slope launch: `sv_slope_launch 0`.
- half-duck zip parity: `bridge_halfduck_zip_parity 0`.
- zipline cooldown: `bridge_zip_cooldown_enable 0`.
- source push: no script calls `SourcePush_AddVolume`.
- SetClassVar (66): no dedi script calls `SetClassVar`, and S30 has no `_setClassVarClient` (only `_setClassVarServer`). Consume it.
- portal: `OPTION_WIP=ON` in this build, but it is a dev weapon.
- S21-only fixes:
  - `fov_limit`, `visual_clutter`, `sceneview`, `cubemap_capture`: preferences or work-in-progress features.
  - `grapple_rope_diag`, `titan_*`, `phase_door_depth`: patch S21 code bytes.
  - `body_skin`, `pose_param` clamps: crash guards, revisit only if S30 crashes there.
  - `demo_natives`, `classvar_natives`, `vscript_*`: natives for Flowstate client scripts, not used while S30 runs its own.
- server-only shared modules: `weapon_heat`, `deathfield_system`, `highlight_context`, `status_effects_sdk`, `weapon_script_vars`, `weapon_legendary_ext`, `dt_extend`. Each states "no client-side code; the client binds natively".

## Mantle boost (detail)
- Dedi encoding (`game/server/mantle_boost.cpp:207`): `state & 7 | (traversalSeq & 15) << 3 | edict << 7`. States: 3 = FAILED, 4 = BOOST.
- S30 native enum (registered at 0xC38640):

  | S30 value | state |
  |---:|---|
  | 0 | UNBUFFERED |
  | 1 | INVALID |
  | 2 | FAILED |
  | 3 | MANTLE_JUMP |
  | 4 | MANTLE_BOOST |
- The dedi appends `m_mantleBoostState` to DT_Player (`dt_extend.cpp:269`), and S30 has a native prop of the same name. If the matcher binds them, S30 reads the packed value into its enum field: garbage plus a different meaning for 2 and 3. **Check the matcher log for this prop** (out of this fork's scope).
- S2C lane: `net_ScriptMessage` (S3 68), untyped frame of 8 bytes: [u32 0x3156424D 'MBV1'][i32 packed].
  - Reliable, sent to the owner only, change-gated.
  - S21 handling: `MantleBoostClient_OnAuthoritativeState(state, seq, edict)` reconciles its own FSM twin (`mantle_boost.cpp:354`) and feeds the 3P animation (`MantleBoostAnim_OnWireState`).
- Options:
  - (a) Let S30 native predict, and measure. If it mispredicts, translate the dedi's verdict into S30's enum (keep S30's own field, drop the dedi prop from matching).
  - (b) Port the S21 twin. It disables the native boost, which needs S30's gate (`canMantleBoost` player setting / `MayMantleBoost`, **unproven** where it is read).

## Chat (detail)
- **70 svc_ChatBuilder** (dedi `game/server/chatbuilder.cpp:24`, `shared/chatbuilder.h`).
  - Layout: [16 payload bytes][8 streamFlags (1 admin, 2 player chat)][8 segCount ≤ 8][8 senderSlot], then per segment [8 op (0 text, 1 rainbow)][8 flags (1 newline)][8 r][8 g][8 b][16 sustainMs][16 fadeMs][8 textLen ≤ 64][text].
  - Player chat: `ChatBuilder_SendPlayerChat` from `gameinterface.cpp:645` when `bridge_chat_bridge_render 1`.
  - The S21 client renders it into the hud chat RichText (`hud_basechat.cpp` RenderChatBuilder).
- **67 svc_SystemSayText**: [string prefix][string message][1 admin]. S21: `CBaseHudChat::PrintSystemMsg`.
- S30 chat facts, matched against S21 0x8F2AE0 (same shape, offsets proven by the decompiles):
  - `PrintSystem(msg, prefix, channel)` **0x9A2000**. Pattern `48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 81 EC ?? ?? ?? ?? 48 8B 1D` is unique in S30 (also S21's pattern). It walks the hud-chat list whose head global is loaded by the `mov rbx,[rip+x]` at +0x15: S30 **0x834F678**, S21 0x96F3A68.
  - Hud chat fields, S30 vs S21 (+96 throughout):

    | field | S30 | S21 |
    |---|---|---|
    | ready byte | +675 | +579 |
    | text colour | +728 | +632 |
    | prefix colour | +732 | +636 |
    | channel | +748 | +652 |
    | history RichText* | +760 | +664 |
    | next | +768 | +672 |
  - RichText vtable offsets, S30 vs S21 (+40 bytes):

    | method | S30 | S21 |
    |---|---|---|
    | InsertChar | 2104 | 2064 |
    | InsertString(wide) | 2112 | 2072 |
    | InsertColorChange | 2288 | 2248 |
    | InsertFade(show, fade) | 2304 | 2264 |
- Simplest S30 path:
  - 67 → `PrintSystem(message, prefix, 0)`.
  - 70 → walk the segments with the four RichText calls, as S21 RenderChatBuilder does, with the S30 offsets above.
  - Alternative for player chat only: set the dedi's `bridge_chat_bridge_render 0`. The dedi then sends the SayText user message ([8 slot][string][1 team]). S30's SayText handler 0x9A16E0 reads [8 slot][string 256][1 bit][1 bit], and the extra bit falls in the payload's byte padding (0, meaning **unproven**). It also needs the sender's player entity on the client.

## Playlist lanes (detail)
- **32 svc_PlaylistChange**: dedi [NUL-terminated name ≤ 64].
  - S21 `S21Bridge_ApplyServerPlaylist`: validates `[A-Za-z0-9_]{1,63}`, calls `Playlists_SetCurrentByName(name)` once per new name (≤ 8 per session), and keeps the applied name. Result 0 means the name is not in the client's table yet.
  - S30: **0x3DD340** `char (const char*)`. Evidence: S30 `CClientState::SetSignonState` at 0x2FF445 does `lea rcx,[r14+0x90]; call 0x3DD340; test al,al; jne`, the same shape as S21 0x2D682B → 0x3AD7A0 (S21's resolved setter).
  - Anchor pattern, unique in both exes: `E8 ?? ?? ?? ?? 84 C0 75 ?? 4D 8D 86 ?? ?? ?? ?? B9` at S30 0x2FF44C; follow the E8.
- **34 svc_PlaylistOverrides**: dedi [32 lenBits][8 count ≤ 64], then per entry [8 nameLen < 128][name][8 valueLen < 64][value]. The reader must seek to start + lenBits.
  - S21: a table plus hooks on the current and by-name var getters.
  - The override applies when the name is on `bridge_playlist_override_allow` (default `name,map_name`), or when the current playlist declares it as a host setting and the value validates. Setting declarations are never overridden.
  - S30 current-playlist var getter: **0x23E470**, interface global **0x3FD1A60**, vtable **0x17E4A98** slots 90 and 92 (S21: same slots, 0x2163A0). Proven by `GetCurrentPlaylistVarString` native 0xB50290 → 0xB33430 → `vtable+720`. Unique pattern: `48 83 EC 38 48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0B 45 33 C9 41 8B C1 48 83 C4 38 C3 4C 8B 0D`.
  - By-name getter: **unproven**. S30 slots 75/76 → 0x23E330 → 0x3DC790, which does not decompile as a var getter.

## Other lanes
- **68, Portal frame**: 40 bytes, [u32 0x504F5254 'PORT'][i32 entity][i32 portal (S21 recv handles)][3 f origin][3 f angles][i32 tick], from `portal/portal_teleport.cpp:175`. The S21 client applies it only under `SDK_WIP`. Dev weapon: consume.
- **69 svc_DebugOverlay**: [16 payload bytes][8 count ≤ 64] then 58-byte items: [8 type (0 line, 1 box, 2 sphere, 3 capsule, 4 triangle)][12 floats p0..p3][8 r][8 g][8 b][8 a][8 flags][f duration]. Sent when dedi scripts or SDK draw debug geometry (scripts call `DebugDrawLine` 162× and `DebugDrawText` 172×, mostly dev paths). S30 has native `DebugDraw*`; the engine-side overlay entry is **unproven**. Low priority: consume.
- **66 svc_SetClassVar**: [string key][string value]. Consume (see "not needed").

## S30 hook points (summary)

| what | RVA | pattern / evidence |
|---|---|---|
| C_UserCmd::ComputeRandomSeed | 0xBA5900 | `48 89 5C 24 18 55 48 8B EC 48 81 EC 80 00 00 00 33 C0 C7 45 A0 01 23 45 67 48 8B D9 C7 45 A4 89` (unique); seed at cmd+0x12C, command_number at +0 |
| CBaseHudChat PrintSystem(msg, prefix, channel) | 0x9A2000 | pattern above; head 0x834F678 |
| Playlists_SetCurrentByName | 0x3DD340 | E8 at 0x2FF44C (anchor above) |
| current playlist var getter | 0x23E470 | pattern above; iface 0x3FD1A60 slot 90/92 |
| mantle boost enum | 0xC38640 | registration of MANTLE_BOOST_STATE_* (values above) |
| prediction error report | convars | `cl_pred_error_verbose`, `cl_predict_error_icon_show` |
