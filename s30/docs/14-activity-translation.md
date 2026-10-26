# 14. Activity and weapon-state translation, S3 dedi → S30.1 client (2026-10-26)

Section C of doc 13. This doc covers research and data only; no hooks are implemented. Generated output:
`src/core_s30/activity_s3_to_s30_table.h`. Tools: `s30/tools/activity_list.py`, `activity_tables.py`,
`script_consts.py`, `script_enum_slots.py`. Raw lists: `analysis/activity/*.tsv`.

## What the S21 SDK does (reference, `tools/s21-r5sdk/src/game/shared`)

Almost all of it runs **on the dedi**, at the wire boundary. The client has a single hook.

| S21 file | what it translates | where | why |
|---|---|---|---|
| `activity_s3_to_s21.cpp` | activity id S3 → S21 (and S21 → S3) by **name** | dedi: map built at host-state init (`host_state.cpp:1647`) from the live dedi table (`ActivityList_GetActivityName_Server`) against the embedded `activity_s21_enum.h` | ids are positions in each engine's registration order, and S21 inserted and reshuffled entries |
| `dt_extend.cpp` `WeapIdealActivity_XlatProxy` | `DT_WeaponX.m_idealActivity`, `m_customActivity`, `m_weaponActivity` S3 → S21 | dedi SendProp proxies | e.g. inspect: dedi 557, S21 564 |
| `dt_extend.cpp` `DTExtend_ApplyWeapStateXlatProxy` + `weapstate_s3_to_s21.h` | `m_weapState`: S3 22 states → S21 25 (S21 inserts ENERGIZE at 8 and COOLDOWN_OVERHEAT at 22); WCAF flag bits remapped | dedi SendProp proxy | "every client-side attempt degenerated into a compare mask on a predicted field" |
| `dt_extend.cpp` collision-group proxy | `m_CollisionGroup` S3 → S21 | dedi SendProp proxy (8 tables) | S21 inserted groups |
| `weapon_customact_c2s_xlat.cpp` | usercmd `weaponCustomActivity` S21 → S3 | dedi: hooks both clones of `Weapon_ExecuteCustomActivityCmd` | the dedi's weapon sim uses S3 ids |
| `weapon_dualwield_slot_patch.cpp` | dual-wield partner slot main+5 → main+7 | dedi: byte patch at two sites (partner `r5apex_ds+0xB52270`, give `+0xDC83AD`) | S21 pairs main+7 |
| `offhand_slots_ext.*` | offhand slots 6 and 7 as a side-band map plus a SendProp proxy, with script natives | dedi + client | the engine stores only 0..5 |
| `activity_s3_to_s21_client.cpp` + `activity_s3_to_s21_table.h` | the player's **queued activity** (S21 `player+8118`) S3 → S21 | **client**: hook on the anim-state activity selector `0x5B55F0` (pattern `40 57 48 83 EC ?? 48 8B 81 ?? ?? ?? ?? 48 8B F9 48 85 C0 74`); translate-and-restore around the original | the jump T-pose: the dedi sent S3 693 (JUMP_START), which reads as an unbound S21 activity. Skips the 6 values the client writes itself (713, 740, 744, 771..773) |

How S21's table was built: the embedded enum is the S21 client's `RegisterSharedActivities` call list in order.
`activity_list.py` reproduces it **exactly** from the S21 exe (`0x9DA060` → `RegisterActivity 0x9D9D70`): 975 of 975
names have the same ids, plus `ACT_RESET` at 0, which the SDK leaves out.

The workspace dedi runs this code today: `sdk_port` has the same files, and the dedi log shows
`[ACT-XLAT] S3->S21 activity map built: 975 mapped, 20 unmapped (S3 max=994)`, the three `[WEAP-ACT-XLAT]`
proxies, `[WEAPSTATE-XLAT]`, 8 `[COLGROUP-XLAT]`, `[WEAP-ACT-C2S]` on both clones and both `[WEAP-DUALWIELD]`
patches. **So the S30 client receives S21-numbered activities and weapon states, and the dedi reads the S30
client's usercmd custom activity as an S21 id.**

## The three activity tables

| image | function (RVA) | RegisterActivity | entries |
|---|---|---|---|
| S3 dedi `r5apex_ds.exe` | `0xB38FC0` | `0xB38DA0` (name, id, flag) and its no-flag twin `0xB38E60` (used from id 663 on) | 0..882 |
| S3 dedi customs | `scripts/activity_types.txt`, appended in file order (SDK `LoadCustomActivitiesFromFile`) | | 883..994 (112). Matches the dedi log: `Loaded 112 custom activities`, `S3 max=994` |
| S21 client | `0x9DA060` | `0x9D9D70` | 0..975 |
| S30.1 client | `0xAA11C0` (called once, from client init `0x8A9748`) | `0xAA0F00` (name, id): all 1029 callers are in `0xAA11C0`, so S30 loads no customs | 0..1028 |

All three are dense and duplicate-free. Matched by name:
- **S3 → S30**: 965 of 995 mapped. 30 S3 activities have no S30 counterpart: knife melee (`ACT_VM_MELEE_KNIFE*`,
  `ACT_MP_MELEE_KNIFE*`), `ACT_VM_MELEE_CHARGE`, 24 `*_RELOAD*_ALT1`, `ACT_MP_VCD`.
- **S21 → S30**: 951 of 976 (the same 24 `_ALT1` reloads plus `ACT_MP_VCD`).
- 64 S30 activities have no S3 source.
- Examples: JUMP_START S3 693 / S21 713 / S30 719; WEAPON_INSPECT S3 557 / S21 564 / S30 562.

`activity_s3_to_s30_table.h` contains:
- `kS30Activities` (name, id): S30's counterpart of `activity_s21_enum.h`, for building a map by name at runtime as
  the dedi does today.
- `kActS3ToS30[995]`, `kActS21ToS30[976]` and `kActS30ToS3[1029]`, with -1 where there is no counterpart.
- `kS30LocalQueuedActivities`.

A static assert check under MSVC passes (`kActS3ToS30[693] == 719`, `kActS30ToS3[719] == 693`, 1028 entries).

The S3 part of `kActS3ToS30` depends on the dedi's `activity_types.txt`; regenerate the table when that file
changes. The runtime-by-name path through `kS30Activities` has no such dependency.

## Weapon flags and collision groups (script constants)

`script_consts.py` reads `RegisterConstant(vm, name, value)` sites (S30 `0xF5ACE0`, S3 `0x10572A0`, S21 `0xE28310`).
`script_enum_slots.py` reads the native enum tables (push string → push int → newslot).

- **WCAF_*** (custom activity flags): S30 matches S21 exactly (ISINTERRUPTIBLE 1, DISABLEWEAPON 2,
  ALLOW_WHILE_SPRINTING 4, ALLOW_START_WHILE_SPRINTING 8, ALLOW_JUMP_LAND 16, TOGGLE 32, KEEPMELEESTATE 64,
  PLAYRAISEONCOMPLETE 128, PLAYMELEERAISEONCOMPLETE 256). S3 matches the SDK's `WeapCustomActFlags_S3ToS21` comment.
  So the dedi's S3 → S21 flag remap is already correct for S30.
- **TRACE_COLLISION_GROUP_***: S30 matches S21 for all 15 script-visible groups (S3 has 10). So the dedi's collision
  proxy is already correct for S30, as far as those groups go. Groups that scripts cannot see are not proven.
- **Weapon state (`m_weapState`)**: **unproven for S30.** Neither client has state names. A search for a function
  using both ACT_VM_ENERGIZE and ACT_VM_COOLDOWN_OVERHEAT immediates finds only the registration code, in S21 and in S30.

## S30 hook points

| S21 | S30.1 | evidence |
|---|---|---|
| activity selector `0x5B55F0` | **`0x619E30`**, pattern `40 55 57 48 8B EC 48 83 EC ?? 48 8B 81 E8 01 00 00 48 8B F9 48 85 C0 74 ?? F6 80 ?? ?? ?? ?? 03` (unique) | Ranking functions by how many of the selector's activity constants (translated to S30 ids by name) they use as immediates: 29 distinct in `0x619E30`. The only other hits are the registration and enum tables, plus two helpers split out of it: `0x618F80` (drag-revive / downed) and `0x619620` (crouch-slide ends). The decompile has the same flow as S21, with the first check `(player+0x4610)&3` matching S21's `+17392` |
| anim state → player `+464` | `+488` (`0x1E8`) | the selector's first load |
| queued-activity flag `player+8108`, value `+8118` (int16) | **`+8336`, `+8346` (`0x209A`)** | same block: flag, `+8344` skip, 0.2 s timer at `+8340`, `sub_140618A30(a1, act, act)`; every related field moves +228 |
| values the client writes itself (S21 `0x5B3310`: 713, 740, 744, 771, 772, 773) | **`0x6176D0`** writes 718 JUMP_CHARGE, 719 JUMP_START, 746 DOUBLEJUMP, 754 DODGE, 781/782/783 WALLJUMP L/R/UP, and when `player+0x30C4` is set the hoverboard variants 829, 843, 844, 845 | every `mov [reg+0x209A]` in the S30 IDB (IDA operand scan) is in `0x6176D0` |

The other S21 entries are on the dedi and need no S30 RVA: the SendProp proxies, `Weapon_ExecuteCustomActivityCmd`
(both clones) and the dual-wield patches. What changes is **what they translate to**. Two places the S30 port can do
it (not decided, not implemented):
1. **Dedi (S21's design):** proxies emit S30 ids (`kActS3ToS30`, or by name against `kS30Activities`), and the C2S
   hook reads S30 ids (`kActS30ToS3`).
2. **Client:** translate S21 → S30 on receipt (`kActS21ToS30`) and S30 → S21 in the usercmd writer. The S21 SDK warns
   that client-side rewrites of predicted fields turn into prediction mismatches.

## Open (not proven)
- **S30 `m_weapState` ordinals**: whether S21's 25-state layout (which the dedi sends today) is S30's. Needs an
  in-game read of the S30 client's `m_weapState` during known states (energize, overheat, sprint, reload), or a
  state-name source.
- **S30 dual-wield partner slot**: neither client contains the dedi's pairing pattern
  (`80 F9 04 73 ?? 8D 41 ?? 83 F8 ?? 73 ?? 48 63 C1 8B 54 87 ??`). The dedi's client-half twin is `r5apex_ds+0x7CE760`;
  its S30 counterpart has to be matched by callers, not bytes.
- **S30 offhand slot count** (`offhandWeapons` array length) for the slots 6/7 extension: not checked.
- **S21 static queued-activity table**: against this dedi exe matched by name, 507 of its 737 rows are off by one; the
  JUMP_START block (692..694) agrees. Either it was generated from another S3 build or it has a generator error. The S30
  port should use `kActS3ToS30` / `kActS21ToS30` (name-matched) rather than port that table.
- Whether S30's `player+0x209A` is networked, and through which RecvProp: S21 treats the value as wire data except for
  the local writes. Not traced on S30.
