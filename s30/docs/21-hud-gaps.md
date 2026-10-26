# 21. HUD gaps on the S30 client: mute-squad hint, squad panel, legend frame colours (2026-10-26)

Symptoms seen in game (staging map, playlist `dev_default`):
1. "F Mute Squad" stays on screen.
2. The squad HUD piece (teammate slots next to "NEXT LVL") looks broken.
3. The legend frame in the bottom left has the wrong colour palette.

Sources:
- S30 client/UI scripts: `analysis/scripts_s30`
- S30 IDB copy: `analysis/idb/fork_hud`
- dedi scripts: `build/s3_server/platform/scripts/vscripts`
- client log: `20261026_165123.log`, the prop-adapter run with the full `[PROPS]` diff
- S21 reference SDK: `tools/s21-r5sdk`

Everything below is proven unless it says **unproven**.

## Cause A (all three symptoms): no script netvar reaches the S30 client correctly

Script netvars (`RegisterNetworkedVariable`, read with `GetGlobalNetInt` / `GetPlayerNet*` and the RUI tracks
`RUI_TRACK_SCRIPT_NETWORK_VAR_*`) travel in the `CScriptNetData_SNDC_*` entities. Each entity carries one array per
internal type, and each netvar is a **slot index** in one of them.

**1. The arrays do not bind.** The client log lists every ScriptNetData table as mismatched, so the lenient
matcher puts those arrays on dummies and nothing is written:

| table | array | dedi count | S30 count |
|---|---|---:|---:|
| SNDC_GLOBAL / ScriptNetDataGlobal | `m_ranges` | 34 | 38 |
| ScriptNetDataGlobalNonRewind | `m_int32s` | 10 | 12 |
| SNDC_PLAYER_GLOBAL | `m_int32s` | 34 | 38 |
| SNDC_PLAYER_GLOBAL | `m_ranges` | 30 | 38 |
| SNDC_PLAYER_EXCLUSIVE (+ `_EXPANDED`) | `m_bools` | 32 | 36 |
| SNDC_PLAYER_EXCLUSIVE (+ `_EXPANDED`) | `m_int32s` | 6 | 8 |
| SNDC_PLAYER_EXCLUSIVE (+ `_EXPANDED`) | `m_entities` | 22 | 24 |
| every SNDC table | `m_times` | sent | **no S30 RecvProp** |

`_EXPANDED` also lacks the base-table link. S30's arrays are `m_bools`, `m_ranges`, `m_int32s`, **`m_float32s`** and
`m_entities` (strings `0x1934C40..CC0`); S30 has no `m_times`. Its RecvTables are built in `0xB7BE30..0xB7F6D0`, one
function per table. SNDC_GLOBAL, for example:

| array | elements | offset |
|---|---|---:|
| `m_bools` | 18 × 1 B | 3200 |
| `m_ranges` | 38 × 2 B | 3218 |
| `m_int32s` | 18 × 4 B | 3296 |
| `m_float32s` | 26 × 4 B | 3368 |
| `m_entities` | 10 × 4 B | 3472 |

**2. Even where an array binds, the slots differ.**
- S30 `RegisterNetworkedVariable` (native `0xBB7710` → `0xB80210`) assigns each var the next free slot of its
  (category, internal type), in **registration order** (`0xB80160`: `slot = counter[type]++`).
- S30 records `{hash, category +8, type +12, slot +16, second slot +20, min/max +24/+28}` in a 350-entry hash table
  at **`0x85CCE40`**.
- The dedi assigns its own slots in its own script order. Its table is the one `scriptnetdata_ext.cpp` reads:
  250 buckets, 32-byte client-chain entries, `{hash, category +8, type +0xC, slot +0x10}`.
- The two script sets register different lists in a different order. Example, `SNDC_GLOBAL_NON_REWIND` int-range
  registrations in `_remote_functions_mp`:

  | | slot 0 | 1 | 2 | 3 | 4 |
  |---|---|---|---|---|---|
  | S30 | switchedSides | roundsPlayed | winningTeam | winReason | **gameState** |
  | dedi | switchedSides | roundsPlayed | winningTeam | **gameState** | |

  So the client reads another var's value as `gameState`. These positions are counted within that file only;
  scripts that register before it are **unproven**.

S21 never had this problem: its client ran the same Flowstate scripts as the dedi, so slots and counts matched
(`game/client/scriptnetdata_client.cpp` only adds UI-VM stubs).

### Fix A: name-keyed slot remap (recommended: client adapter, dedi publishes its table)
1. **Dedi** (server.dll, `+bridge_client_set s30`): after script init, enumerate its netvars and send them once per
   map to each client on the S2C lane, as a new untyped S3 68 frame.
   - Fields per netvar: name, category, internal type, slot.
   - Names come from a hook on its `RegisterNetworkedVariable` chain.
   - Name rather than hash, because S30's hash `0x5DB350` equalling the dedi's Squirrel string hash is **unproven**.
2. **Client** (`bridge_props`-style adapters, owned by the main session):
   - **Slot map:** read its own table at `0x85CCE40`, hash the dedi's names with S30's own function `0x5DB350`, and
     build per (category, type) `dediSlot → s30Slot`.
   - **Adapters:** replace the dummied arrays with adapter Array RecvProps of the dedi's count and element type. Set the
     adapter's offset to 0 and stride to 1 so that `pOut − pStruct` is the element index. The element proxy writes
     the value to S30's array: base `+4` and stride `+0x58` of S30's own RecvProp, found by name at runtime, at
     `map[i]`.
   - **Times:** the dedi's `m_times` go to S30's `m_float32s` (time vars).
   - **Type changes:** where a var is a range on one side and an int32 on the other (SNVT_INT vs BIG_INT), convert.
     S30 stores int ranges as 10 bits, −512..511 or 0..1023, per `0xB80210`.
   - Vars S30 does not register are dropped; S30-only vars keep their defaults.
3. Alternative: a dedi-side layout that copies S30's slots (hook the dedi's slot allocation).
   - Needs S30's (name → slot) table at the dedi's registration time: a static dump from a client run, per playlist,
     because some registrations are mode-dependent. That makes it brittle.
   - The dedi would also have to grow its arrays to S30's counts (`scriptnetdata_limits.h` already grows them, to
     S21's) and rename `m_times` → `m_float32s` with Float elements.

## Cause B (symptom 1): `eGameState` numbering differs
- S30 (`sh_consts.gnut`): WaitingForCustomStart 0, WaitingForPlayers 1, **PreGamePreview 2**, PickLoadout 3,
  Prematch 4, Playing 5, SuddenDeath 6, SwitchingSides 7, WinnerDetermined 8, Epilogue 9, Resolution 10,
  Postmatch 11.
- Dedi (`_settings.nut`): WaitingForCustomStart 0, WaitingForPlayers 1, PickLoadout 2, Prematch 3, Playing 4, …,
  Postmatch 10, MapVoting 11.

`gameState` is a global non-rewind int carrying the raw number. Even once Cause A is fixed, the dedi's Playing (4)
reads as S30 Prematch.

The "F Mute Squad" text is the `squadMuteHint` of the waiting-for-players overlay:
- `cl_gamemode_survival.nut` `WaitingForPlayersOverlay_Setup` / `UpdateWaitingForPlayersMuteHint`.
- It is shown because `dev_default` (inherits `survival_dev`) leaves `squad_mute_intro_enable` at its default
  (true) and `survival_staging_area_enabled` unset.
- The overlay is destroyed only in `OnGamestatePlaying` (eGameState.Playing enter callback). With Cause A and Cause B
  the client never enters S30 Playing, so the overlay and its hint stay.

**Fix B:** renumber the dedi's `eGameState` to S30's order: insert `PreGamePreview` after `WaitingForPlayers`, keep
`MapVoting` last. Verified safe on the dedi:
- no script compares `GetGameState()` or `"gameState"` with a numeric literal;
- `_count_` only sizes arrays;
- no SDK C++ code uses the values (grep of `src/`).

Keep the original as `.s21`. Note: MapVoting becomes 12, which S30 does not know; the client asserts
`newVal < _count_` (`cl_gamestate.gnut`). Only Flowstate map voting uses it.

**Interim** (playlist only, no code): `squad_mute_intro_enable 0` in the dedi's `dev_default` / `survival_dev` vars
hides the hint text. The overlay itself stays until Cause A and Cause B are fixed.

## Cause C (symptoms 2 and 3): props S30 moved to another table never bind
For each dedi SendProp the client logged as "no RecvProp", the S30 exe was checked for the same name. 9 of the 47
on DT_Player / DT_BaseEntity / DT_BaseCombatCharacter / DT_WeaponX exist in S30 under another table.
Offsets come from the decompiled RecvTable builders, records parsed with `recvrec.py`:

| prop | dedi sends in | S30 RecvTable | S30 offset, type |
|---|---|---|---|
| `m_teamMemberIndex` | DT_BaseEntity (every entity) | DT_BaseCombatCharacter (`0x8E1BB0`) | **828 (0x33C)**, Int |
| `m_squadID` | DT_BaseEntity | DT_BaseCombatCharacter | **832 (0x340)**, Int |
| `m_shieldHealth` | DT_BaseEntity | DT_BaseCombatCharacter, also DT_TitanSoul (`0x8D71F0`) | **400**, Int |
| `m_shieldHealthMax` | DT_BaseEntity | DT_BaseCombatCharacter, DT_TitanSoul | **404**, Int |
| `m_glideMeter` | DT_Player.localdata | DT_Player (`0x835240`) | 0x30B0, Float |
| `m_lastMoveInputTime` | DT_Player.localdata | DT_Player | 0x3D2C, Time |
| `m_grappleDetachTime` | DT_Player.m_grapple | record in the DT_Player builder | 0x2E1C, Time — **unproven** whether relative to a `m_grapple` sub-struct |
| `m_networkedFlags` | DT_BaseCombatCharacter, DT_AI_BaseNPC, … | 11 S30 sites (DT_TitanSoul at 720, …) | per table, not needed for the HUD |
| `m_mantleBoostState` | DT_Player | native S30 field, other meaning | **leave unbound** (docs/19) |

The offsets 400/404/828/832 are inside `C_BaseEntity`'s own layout. They are identical in two unrelated S30 tables,
so they are base-entity fields that S30 networks only through the combat-character table.

- **Squad panel:** `cl_squad_display.gnut` `GetTeammateAtIndex` and the unit frames find teammates by team +
  `GetTeamMemberIndex()`, and `cl_unitframes.gnut` tracks `RUI_TRACK_PLAYER_TEAM_MEMBER_INDEX`. With
  `m_teamMemberIndex` / `m_squadID` never written, every player has index 0 and squad 0.
- **Legend frame colours:** the frame's team-member colour comes from the same index. The shield colour tier comes
  from `m_shieldHealthMax`, which is also never written. Which of the two drives the exact palette the user sees is
  **unproven**; both are wrong today.
- The squad slots next to "NEXT LVL" also read script netvars (Cause A); expected squad size comes from the
  playlist's team size.

**Fix C (client, `bridge_props`):** adapters keyed by prop name with any root table, owner = root entity, same
mechanism as `m_classModsActive`:
- `m_teamMemberIndex` → `*(int*)(ent+828)`
- `m_squadID` → `+832`
- `m_shieldHealth` → `+400`
- `m_shieldHealthMax` → `+404`
- `m_glideMeter` → `*(float*)(ent+0x30B0)`
- `m_lastMoveInputTime` → `*(float*)(ent+0x3D2C)`

Restrict to send type 0 (Int) for the first four, 1 / 9 for the last two. Leave `m_grappleDetachTime` until its base
is proven.

## What S21 handled that the S30 port does not (yet)
- **The dedi-side datatable reshaping targets S21's RecvTables.**
  - `dt_extend.cpp` rebuilds the dedi's SendTables from `s21_dt_schema.h`: 5,434 lines of S21 tables, props, types,
    element counts. On top of that come the S21 rename table `DTExtend_RenameProps`, `[DT-BRACKET]` element names and
    `[WIRE-NAME]` uniquify.
  - That is why S21 sees matching shapes. The renames only work within a table and warn on cross-table moves
    (`DTExtend_WarnRenameTargetNotInSchema`), so even the S21 machinery cannot express S30's moved props.
  - The S30 port has no S30 schema or rename table. Today it relies on client adapters for each S30 change.
  - A generated `s30_dt_schema.h`, dumped from the S30 client's RecvTables, plus an S30 rename list under
    `+bridge_client_set s30` would let the dedi emit S30's shapes for every prop at once. Moves still need adapters
    or a new cross-table mechanism.
- **Script netvars:** S21 needed nothing, because it ran the dedi's scripts. S30 needs Fix A.
- **Script enums carried as raw numbers:** S21 needed none for the same reason. S30 so far:
  - fixed: `eHealthPickupType` / `eConsumableType`, activities;
  - found here: `eGameState`.

  Other enums sent as numbers (netvars, remote-call args) need the same name comparison. **Unproven** which others
  differ; method: diff `global enum` blocks of the S30 scripts against the dedi's for every enum used in
  `RegisterNetworkedVariable` defaults, `SetGlobalNetInt` / `SetPlayerNetInt` calls and remote-call int args.
- **UI-VM `GetPlayerNet*` stubs and NonRewind lifecycle** (`scriptnetdata_client.cpp`, `scriptnetdata_ext.cpp`
  client half): S21-client natives the Flowstate UI scripts needed. S30 has its own natives. Not needed.
- **RUI tracks / wall time** (`ruitracks.cpp`, `rui_walltime.cpp`): no-ops / retired in S21. Not needed.

## Order
1. Fix B: dedi `eGameState` renumber. One script edit.
2. Fix C: six client adapters.
3. Fix A: netvar remap, dedi table frame + client array adapters. This is the large one, and the one most HUD
   pieces depend on: game state, squad, evo/upgrade points, ring timers.
4. An S30 schema for the dedi rebuild (longer term).
