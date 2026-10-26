# 19. Mantle boost: the dedi follows the S30 client's native boost (2026-10-26)

User decision: keep S30's own mantle boost. The S30 client predicts it with its native movement code; the dedi's
authoritative boost (`src/game/server/mantle_boost.cpp`) has to land on the same result. Sources: S30 IDB copy
(`analysis/idb/fork_mantle`), S30 player settings (`asset_pipeline/s30_dedi/export/settings/player/mp`, the same
assets the dedi's common.rpak carries), the dedi source.

## S30.1 native algorithm

| what | S30.1 |
|---|---|
| traversal move (decision + finish) | `C_GameMovement::TraversalMove` **0xB12580** (`a1+8` player, `a1+16` CMoveData, `a2` just started) |
| jump | `C_GameMovement::Jump` **0xB07BA0** |
| state | `player+11372` (`m_mantleBoostState`, int, in the prediction desc at 0x8422CE, type 5, offset 0x2C6C); 0 UNBUFFERED, 1 INVALID, 2 FAILED, 3 MANTLE_JUMP, 4 MANTLE_BOOST |
| previous delta | `player+11376` (float) |
| MayMantleBoost | **0xBE5A50**: movetype byte `+938 == 12`, traversal type `+11208 == 0` (mantle), state `!= 1`; returns 1 or 2 (never blocks once those hold) |
| GetMantleBoostState | 0xBE5A90 (`+11372`) |

### Decision (0xB12580, decomp lines 414–464), every tick while traversal type is mantle and state is 0
1. `delta = traversalViewPitch - eyePitch` (0x6D5060 on the first-person proxy, minus `EyeAngles` 0xBC73F0).
2. Only when not the first tick and `settings.canMantleBoost`:
   - the activation button is pressed: `mantle_boost_input_setting` (ConVar `qword_1426720D8`, default "1",
     FCVAR_USERINFO) 1 → `mv+36 & 2` (IN_JUMP), 2 → `& 0x4000004` (duck), 3 → `& 0x10000000`; `mv+36` is the
     pressed set (Jump 0xB07BA0 reads the same field for its own jump press);
   - dangle clear: `player+11369 == 0`, `player+11364 == 0.0`; MayMantleBoost ≥ 1;
   - **FAILED (3)** if `delta < 0` or `prevDelta < delta` or `settings.mantleBoostSweetSpotAngleThreshold <=
     |delta|` or `player+8980 (traversal anim frac, last tick's) <= 0.5`; else **BOOST (4)**.
3. `prevDelta = delta` (also on the first tick; traversal start 0xB0E9F0 zeroes state and prevDelta).
4. Hang: the climb settling into the hang activity (11) sets state 1 (INVALID).

### Finish (0xB12580 lines 1101–1205), progress reaches 1.0
- state < 3: plain traversal exit.
- state ≥ 3: `speed = GetPoseSpeed_Sprint(player, 0)` (0xBE0AA0: `player+13244` move scale × pose[0].sprintspeed).
  Direction: `mv+172` (move dir in gravity space), else the traversal forward dir; reflected over the ledge when
  `dot(dir, fwd) < 0`.
  - state 4: `velocity = dir × speed × mantleBoostSprintSpeedMultiplier × (2·se + 1)`, `se` = status effect
    `boost_mantle_boost` (0xB97070, 0 when absent); `player+11840 = time` (`m_lastMantleBoostTime`);
    client callback `CodeCallback_OnPlayerMantleBoosted`.
  - state 3: `velocity = dir × speed`; with input setting 2 (crouch) it presses duck, runs Duck (0xB24AF0) and
    returns — no jump.
  - then (not crouch-failed): `player+7852 = time`, state restored, `mv+36 |= IN_JUMP`, **Jump**, restore buttons.
    State 4 then sets `player+11837..11839 = 1`.
- Jump height (0xB07BA0): state 4 → `mantleBoostJumpHeight × (2·se + 1)`; otherwise `jumpHeight`.
  `vz += sqrt(2 · sv_gravity · height)` along the up axis (`qword_1426A9878` is the ConVar created with default
  "750" at 0x1D34C0).

### Tap-strafe and reset
- 0xB16A30: with `mantle_boost_disables_tap_strafes` (ConVar `qword_1426A3708`) and state 4, the tap-strafe path
  is suppressed.
- State back to 0: landing (0xB05590, when `time - player+11396 >= 0.5` and `player+11390 == 0`), traversal start
  (0xB0E9F0), 0xBD41F0, 0xBDF9F0.

### Settings values (all 29 S30 legends, `pilot_survival_*.json`)
- Base: `canMantleBoost false`, `mantleBoostJumpHeight 120` (Sparrow 200), `mantleBoostSprintSpeedMultiplier 1.5`
  (Sparrow 1.0), `mantleBoostSweetSpotAngleThreshold 1.5`, `jumpHeight 56`.
- Class mod **`enable_mantle_boost`** (index 27 in every legend's `$modNames`): `canMantleBoost true`,
  `mantleBoostJumpHeight 56`, `mantleBoostSprintSpeedMultiplier 1.5` — the same for every legend.
- So on S30 **the boost exists only with that class mod**, and then always with 56 / 1.5 / 1.5°.
  The S30 client script names it (`sh_pilot_passive_mantle_boost`: `MANTLE_BOOST_MOD_NAME = "enable_mantle_boost"`);
  which server code adds it is not in the client scripts (**unproven** where S30's server applies it).

## Dedi vs S30

| item | dedi before | S30 | now |
|---|---|---|---|
| decision cascade | dec / angle / frac with `mantle_boost_require_decreasing_view_angle 1` | same conditions (`!(delta >= 0 && prev >= delta)`, `|delta| >= thr`, `frac <= 0.5`) | unchanged (equal) |
| sweet-spot threshold | 1.5 | 1.5 | unchanged |
| activation masks | 2 / 0x4000004 / 0x10000000, pressed set | same | unchanged |
| canMantleBoost | none: everyone could boost | only with class mod `enable_mantle_boost` | **gate added** (`mantle_boost_require_class_mod 1`) |
| state 4 sprint multiplier | 1.3 | 1.5 | **default 1.5** |
| state 4 jump height | 90 | 56 | **default 56** |
| state 3 jump height | 56 | jumpHeight 56 | unchanged |
| direction / reflection | moveDirGravity else fwd, reflect | same | unchanged |
| crouch-mode failure | duck, no jump | same | unchanged |
| prevDelta on the first tick | not sampled (0) | sampled | unchanged: the decision needs `frac > 0.5`, so the first ticks can never decide |
| `boost_mantle_boost` status effect factor | none (Flowstate `SetMantleBoostProfile` scales instead) | `(2·se+1)` | not ported; neither the dedi's nor S30's client scripts define/apply the effect, so the factor is 1 |
| landing reset | landed time > boost time | landing with a 0.5 s rule on `player+11396` | not changed (**unproven** what +11396 is) |

## Changes in `src/game/server/mantle_boost.cpp`
- `bridge_mantle_boost_sprint_mult` default 1.3 → **1.5**; `bridge_mantle_boost_jump_height` 90 → **56**.
- New `mantle_boost_require_class_mod` (FCVAR_RELEASE, default 1) and `bridge_mantle_boost_class_mod`
  (default `enable_mantle_boost`): the trigger is evaluated only when the player's `m_classModsActive`
  (`player+0x5EF8`) holds that mod's index in the player's settings (`player+0x5DF0`), resolved with
  `PlayerSettings_FindModIndex` (same pattern and offsets as `entity_script_ext.cpp`). A settings asset without the
  mod cannot boost; an unresolved FindModIndex warns once and leaves the gate off.
- Not built here (the main session builds server.dll).

## What else is needed for a boost to happen at all
1. **The dedi must give players the class mod**, as S30 does: today no dedi script adds `enable_mantle_boost`, so
   with the gate nobody boosts — which now matches the S30 client (it never predicts one without the mod).
   The dedi already has the native: `player.AddPlayerClassMod( "enable_mantle_boost" )`, e.g. on spawn when
   `GetCurrentPlaylistVarBool( "mantle_boost_enabled", true )` (the playlist var S30's
   `ShPassiveMantleBoost_Init` reads).
2. **The S30 client must receive `m_classModsActive`**: the client log reports `DT_Player.m_classModsActive: type
   (send type 7, recv type 5)`. Unless the matcher binds that prop, the client never sees the mod, never predicts
   the boost, and every dedi boost is a correction. Check/convert it in the snapshot matcher (client side, out of
   this change).
3. No usercmd change: the bridge copies the 32 button bits verbatim (`bridge_usercmd_s30.cpp`), and IN_JUMP 2,
   duck 0x4000004 and 0x10000000 have the same meaning in S30's mantle code and the dedi's masks.

## Unproven
- That the dedi's camera-delta sample (`MantleBoost_SampleCameraDelta`, ported against S21) equals S30's
  0x6D5060 value at the same tick.
- The S30 server side of `enable_mantle_boost` (who adds it, when).
- `player+11396` in S30's landing reset (0.5 s rule).
- S30 `player+11837..11839` set after a boost vs the dedi's `MantleBoost_GrantSlide`.

## Test in game
1. Build server.dll, deploy (the dedi keeps `server.dll.pre_c2s` / `.s21` backups).
2. Dedi: `+bridge_mantle_boost_trig_log 1 +bridge_mantle_boost_apply_log 1`. Give the player the mod (script or
   console `script GetPlayerArray()[0].AddPlayerClassMod( "enable_mantle_boost" )`).
3. Client: `cl_pred_error_verbose 1`, `cl_predict_error_icon_show 1`; mantle a crate, press jump late in the climb.
4. Expect `[MB-TRIG] ... -> 4` / `[MB-APPLY] state=4 ... speed=<sprint×1.5>` on the dedi and no prediction
   error on the client. Without the mod: no `[MB-TRIG]` line and no client boost.
