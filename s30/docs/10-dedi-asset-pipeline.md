# 10. S30 assets for the S3 dedi (2026-10-26)

The S30 client and the S3 dedi must agree on models, rigs, sequences and settings: the
dedi precaches models by name, plays sequences by index, and reads hitboxes, attachments
and collision from its own copies. So the dedi's paks are rebuilt from S30's, under the
same names (user rule: replace the pak, no overlay or renamed paks).

## Pipeline (`s30/tools/dedi_paks.py`)

| dedi pak | S30 source | note |
|---|---|---|
| `common_early` | `common_early` | |
| `common_roots` | `common_early` (the 6 assets S21 packs here) | the dedi never loads it |
| `common` | `common` + `common(01)` | |
| `common_mp` | `common_mp` | |
| `mp_lobby` | `mp_lobby` | |
| `root_lgnd_skins` | all 804 `root_lgnd_skins_*` paks | S30 splits skins into on-demand paks; `_shdrs` paks hold shaders only |
| `mp_rr_canyonlands_staging_mu1` | same | the test map |

Kept as they are: `patch_master` (patch index; the rebuilt paks have no patches) and
`effects_sdk` (an empty 128-byte pak). Taken out of the workspace dedi (`preload.rson` no
longer lists them; files kept as `.rpak.s21`):
- `common_flowstate`, at the user's request; `start_s3_server.ps1` also passes `+sdk_pak_load_flowstate 0`.
- `sdk_s30_dedi` (the port kit's pak, built against the S21 commons). 1,649 of its 1,746 assets are
  S30 assets the rebuilt paks now carry under their own names; the other 97 are the kit's renamed
  copies (`aseq_30\...`, `arig_30\...`, Lifeline drone). Loaded on top of the S30 paks it crashed the
  dedi: its settings name S21's `settings_layout/settings_rpak_root_layout.rpak`, which no rebuilt pak
  has, and the engine's settings loader dereferenced the NULL layout (r5apex_ds RVA 0xFB3277).

Only the types the S3 dedi reads are carried; materials, shaders and textures are not.

| type | S30 | dedi | how |
|---|---|---|---|
| `mdl_` | v20 | v10 | RSX raw export -> rmdlconv `-v19s30` (S30 -> v17 -> v54) |
| `arig` | v7 | v4 | R5-AnimConv `-i 30 -o 3` |
| `aseq` + `asqd` | v14 + v1 | v7 | R5-AnimConv (asqd folded into the sequence) |
| `stgs` | v2 | v1 | RSX JSON -> RePak v1 writer (v2 only adds 8 header bytes) |
| `stlt`, `dtbl`, `txls` | v0, v1, v1 | same | RSX export -> RePak |

Steps: `list` (RSX listing + dependency lists), `export`, `models`, `anims`, `buildlist`,
`repak`, `install`; gates: `dedi_model_gate.py` (converted v54 headers vs the S21 dedi's
copies of the same models: no lost bones/hitbox sets/bodyparts/physics) and a re-list of every
built pak (same GUID set and counts as its build list, every type at the dedi version).

## Tool fixes (workspace clones under `tools_src/`, the user's checkouts are untouched)

**RSX** (`tools_src/rsx`, on upstream `5ea6071` + the local edits):
- mdl_ v20 raw export (`.rmdl` = the S30 studiohdr as stored, `.phy`, `.vg` raw starpak entry,
  `.vg_static`, `.rson` rigs/seqs). RSX could not export v20 at all.
- asqd sized by its extent in the page (`Pak_RegionSizeAt`): the bone walk truncated S30 clips
  and never ran for clips owned by unparsed models.
- `-noanimparse`: skip the post-load frame decode (raw export does not use it; it faulted in
  `CalcBonePosition` on S30 `common`).
- settings layout export wrote `subLayouts` in field order instead of layoutIndex order
  (S30 `settings_player_layout`: `damageMilestones` is #0 after #1-#4).
- settings mods on one component of a float2/float3 field are exported as `<field>.x/.y/.z`
  (S30 modifies `poseSettings[n].hull_max` per axis; 29 assets had "FIELD NOT FOUND").

**rmdlconv** (`tools_src/rmdlconv`, local HEAD + local edits):
- S30 front end read the RUI panel table as 16-byte records; `mstudiorruiheader_t` is 8 bytes.
  The phantom second header was mesh 0's `numparents/numvertices`, relocated like an offset
  (`01 00 0e 00` -> `f8 ff 0d 00`); the dedi pass then copied ~64K parents and faulted
  (71 weapon models).
- `FIX_OFFSET` was the identity for v16/v17 u16 offsets; the engine decodes
  `(o & 0xFFFE) << (4 * (o & 1))`. Every v17 intermediate past 64 KB was misread: legend
  models lost all collision ("Invalid collision headerCount"), two derived models faulted in
  the BVH walk.
- raw gaps past 64 KB keep their source position mod 32, so header offsets into them stay
  encodable (drone_frag_loot's linear bone table pointed at a stale offset and faulted at random).
- Known left: `drone_frag_loot` keeps 330 struct-relative string refs the front end cannot
  encode (attachment/skin names); its bones, collision and linear bones convert.
- The dedi models are converted without the `.vg` (RePak's dedi writer never reads it; output
  byte-identical, and the v17 -> v10 vertex pass faults on some S30 models). One process per
  model, so a converter fault is recorded instead of ending the batch.

**RePak** (`tools_src/RePak`, local HEAD + local edits):
- settings layouts may map array fields to sub-layouts in any order.
- settings mods accept `<field>.x/.y/.z` on float2/float3 fields.
- `unregisteredRefsFile` (build list): guids written but not registered as pak dependencies.
- `compressLevel` is read per pak map (it was set on the build list, so the paks came out
  uncompressed, which the S3 dedi ignores).

**Oodle**: rmdlconv needs `OodleLZ_Decompress` for S30 vertex groups; `tools_src/oodle_shim`
builds a local `oo2core_9_win64.dll` from RSX's static `oo2core_x64.lib` (passed via `OODLE_DLL`).

## Dedi load-time findings

- **Dependencies first.** The dedi resolves a dependency when the asset is fixed up; one later in
  the same pak is a NULL head (datatables naming settings). Each pak's entries are ordered from
  RSX's dependency lists, dependencies first.
- **Unresolvable references** (`[pak-fixup] unresolved dep`): S30 settings name S30-only types
  (`odla` on-demand-load assets, `wepn` weapon definitions) and assets in paks that load later
  (skin settings in `common` -> models in `root_lgnd_skins`). Each pak's build lists them in its
  `unregisteredRefsFile`: everything a dependency names that is not loaded by then
  (`LOAD_ORDER`).
- **Server BSP.** The dedi's `vpk/server_mp_rr_canyonlands_staging_mu1.bsp` was an older S21
  Flowstate build whose static props name models S30 does not ship (`mdl/vistas/eden_se.rmdl`, foliage,
  crypto holo maps; the S21-built map pak carried 42 such models). `dedi_map.py` rebuilds it from S30's
  own BSP: RSX wrap export of `_client_perm/_client_temp`, `bspconv -dedi` (v52 -> v47), `hf2tris` when
  the map has heightfield lumps (staging has none), a closure check (492 models named, all in the dedi
  paks; `editor_effect_node_*` excluded: editor placeholders the game never spawns, per the user), and
  `revpk` into the server VPK pair (S21 pair kept as `.s21`).
- **A missing model is fatal** with this dedi: the SDK sets its fallback only once `mdl/error.rmdl`
  itself has been looked up, so the first missing model ends in a modal `SDK Error` dialog that hangs
  the dedi. (An early theory that `error.rmdl` must sit in `common_early` was disproved by a test run
  and not kept.)
- **Weapon models renamed in S30** (`ptpov_rspn101` -> `techart/mshop/.../r301_base_v`).
  `weapon_model_paths.py` matches each server weapon script (`platform/scripts/weapons`) with the
  S30 wepn asset of the same name (RSX `--exportsetting wepn=1` writes the keyvalues txt) and takes
  S30's model path per block path/key: 140 values in 61 scripts (originals `.s21`). Placeholder
  differences (`mdl/dev/empty_model.rmdl` on either side: Flowstate's heirloom melee scripts) are
  left alone, and so is one S30 path that is a typo in S30's own data (`.../eapons/...`).
- **Weapon names follow S30** (user rule). `weapon_renames.py` lists server weapon scripts with no S30
  file of the same name and the S30-only scripts whose printname/shortprintname/weaponClass/weaponType/
  viewmodel/playermodel all match: none of the 55 server-only scripts (Flowstate `_og` variants,
  copycat kits, mapedit tool, instagib...) matches one. The space elevator was the one rename; the dedi
  already had both `mp_ability_space_elevator_tac.txt` and S30's `mp_ability_horizon_tac_space_elevator.txt`
  (same identifying keys), and `_weapon_utility.nut` (stickiness table columns) and the copycat kit's
  `disguise_ability` now use the S30 name (originals `.s21`).
- Skipped, with reasons in `build/missing.txt`: one gladcard clip whose asqd is outside the common
  paks (its single reference from the RE-45 model is dropped).

## State (2026-10-26)

The workspace dedi reaches `[R5F-HOST] ready mp_rr_canyonlands_staging_mu1` on the S30-built paks and
S30 BSP, with 0 unresolved pak dependencies. Open:
- `sh_loadouts.gnut` reads `statTrackerMinValue/MaxValue` from `datatable/bot_loadout/bot_loadout_stat_trackers`;
  S30 split them into Low/Med/High MMR pairs (script error at map spawn; which tier is the user's call).
- ~14,000 `[pak-fixup] NULL head` lines in `common`: settings that reference each other in cycles
  (`character/<legend>` <-> its intro quips); one side of each cycle loads second. S21-pak runs logged
  some too (the log is sampled), not this many.
- The old-name rows/files (`mp_ability_space_elevator_tac.txt`, its `arenas_items.csv` row next to the
  S30-name row) are still there.

## Engine limits hit by S30 content (2026-10-26)

| limit | where | fix |
|---|---|---|
| `SkinNames` table 1024 | SDK-created (`skinnames_table_inject.cpp`) | 4096 entries (S30: ~1300 character skin names) |
| `ParticleEffectNames` table 4096 (stock 2048) | engine `r5apex_ds+0xD587DD`, SDK grow | 8192; `m_effectIndex` width derived from it (14 bits, signed) |
| legendary weapon skin slots 32 (stock 8) | `weapon_legendary_ext.cpp` | 64 (S30 max: R-301 42) |
| 256 bones per model | `CStudioHdr` init `r5apex_ds+0x33D93A`: two inline 256-entry per-bone arrays (`+0x48`, `+0x448`), no bound check; 263 bones overwrite `+0x858` | not raised. The one S30 model over 256 (`ptpov_sparrow_tact.rmdl`, 263 bones) was taken out of `mp_weapon_sonar_dart.txt` by the user |
| missing model = fatal dialog | SDK fallback only cached via `FindMDL`; `error.rmdl` names itself `mdl/error` | `GetErrorModel` resolves `mdl/error.rmdl` by name (not yet exercised) |
