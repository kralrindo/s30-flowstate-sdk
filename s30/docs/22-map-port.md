# 22. The rest of the S30 maps on the S3 dedi (2026-10-26)

S30.1 ships 17 maps (`paks/Win64/<map>_client_perm.rpak`). `mp_lobby` (docs/17) and
`mp_rr_canyonlands_staging_mu1` (docs/10) were already on the workspace dedi; the other 15 are rebuilt
the same way: the map's server pak from S30's `<map>.rpak`, the server BSP VPK from S30's own BSP.

All 15 reach `[R5F-HOST] ready <map>` on the map-port test dedi. **None is installed in the workspace
dedi yet** (`build\s3_server` is untouched); `s30/tools/install_maps.ps1` copies them over.

## Status

Test: `s30/tools/dedi_map_test.ps1 -Map <map>` on `build\s3_server_mapport`, port 37016,
`+bridge_client_set s30 +map <map>`, no client connected. "ready" = the map reached ready, no modal
dialog, the dedi still up 8 s later (final run of all 15: `asset_pipeline/s30_dedi_maps/final_tests.log`).

| map | dedi before | map pak (assets) | static props / BSP models | result |
|---|---|---|---|---|
| mp_rr_thunderdome | S21 pak + BSP | 594 (589 mdl, 4 aseq, 1 arig) | 12,489 / 595 | ready; loot-drone script error |
| mp_rr_freedm_skulltown | S21 pak + BSP | 507 mdl | 24,949 / 512 | ready; loot-drone script error |
| mp_quicklighting_test | none | 13 mdl | 73 / 14 | ready |
| mp_rr_district_mu1 | S21 pak + BSP | 2,559 | 437,747 / 2,547 | ready |
| mp_rr_district_mu1_uh | none | 2,704 | 446,546 / 2,690 | ready |
| mp_rr_divided_moon_mu1 | S21 pak + BSP | 2,108 | 378,518 / 2,096 | ready; loot-drone script error |
| mp_rr_divided_moon_mu1_uh | none | 2,234 | 384,237 / 2,218 | ready; loot-drone script error |
| mp_rr_olympus_mu3 | none | 2,142 | 466,429 / 2,114 | ready |
| mp_rr_olympus_mu3_uh | none | 2,283 | 475,439 / 2,262 | ready |
| mp_rr_olympus_mu3_uh_snapback | none | 2,358 (+ the `(01)` patch pak) | 475,634 / 2,344 | ready |
| mp_rr_desertlands_mu5 | none | 2,060 | 371,069 / 2,039 | ready; one hatch sequence not converted |
| mp_rr_tropic_island_mu2_landscape | none | 2,110 | 460,466 / 2,132 | ready |
| mp_rr_canyonlands_hu_avt | none | 2,098 (incl. 10 stgs) | 372,348 / 2,043 | ready; loot-drone script error; needs the crypto script fix |
| mp_rr_canyonlands_hu_avt_uh | none | 2,304 | 377,230 / 2,236 | ready; same |
| mp_rr_canyonlands_hu_avt_uh_night | none | 2,342 | 380,978 / 2,275 | ready; same |

Every BSP passes `dedi_bsp_gate.py` and the closure check (every model a static prop or entity names is in
the dedi paks). Every run also logs the same 12 missing models from scripts (P2020 legendaries, crystal
shards, an elevator door, a rock); they existed before this port and now fall back to `mdl/error.rmdl`
through the SDK fallback (`[MDL-FALLBACK]`) instead of the fatal dialog of docs/10.

Maps with no `levels/<map>.rson` on the dedi (`freedm_skulltown`, `district_mu1(_uh)`, `quicklighting_test`,
the `_uh` variants, `olympus_mu3*`, `desertlands_mu5`, `tropic_island_mu2_landscape`, `canyonlands_hu_avt*`)
run without level scripts, as `freedm_skulltown` and `district_mu1` already did; no dedi playlist names the new
maps (they load with `+map`).

## Pipeline (per map)

```
py s30/tools/dedi_paks.py --map <map> --server <dedi dir> list export models anims buildlist repak install
py s30/tools/dedi_map.py <map> --server <dedi dir> export stage convert closure gate vpk install
pwsh s30/tools/dedi_map_test.ps1 -Map <map>
bash s30/tools/port_maps.sh <map> ...        # all three, map by map, results in asset_pipeline/s30_dedi_maps/port_maps.log
```

- **`dedi_paks.py --map`** (new): builds only `<map>.rpak`, in `asset_pipeline/s30_dedi_maps/<map>`. The shared
  paks are taken as built: their RSX listings name assets and give dependencies, their build lists
  (`asset_pipeline/s30_dedi/build/<pak>.json`) are what the dedi holds when the map pak loads (unregistered
  refs). Converted models are cached across maps (`asset_pipeline/s30_dedi_maps/mdl_v54`, hash-checked):
  12,023 models, 0 conversion failures, 10,859 with collision, collision gate clean.
- **Settings in a map pak**: RSX exports a setting only with its layout, and the layouts are in the shared
  paks. Map mode exports the map's `stgs` in a second RSX pass with `common_early` + `common` loaded
  (`--exportguids`), and copies the layout files RePak reads into the map's export dir (they stay in the shared
  paks). Only the `canyonlands_hu_avt` maps have any: 10 `settings/scriptAnimWindow/fx_crypto_sat_*`.
- **`--server <dir>`** on both tools installs into another dedi copy; both delete the target before writing, so a
  hard-linked test dedi never writes through into the workspace one.
- **`dedi_bsp_gate.py`**: the record-size check took the gcd of the references' lump lengths, which can be a
  multiple of the record size. Lump 0x14 (`LUMP_PACKED_VERTICES`, int16 x/y/z) is now checked against 6:
  `mp_rr_olympus_mu3` ships 3,390 bytes there (565 vertices), the references happen to be multiples of 48.
- **`dedi_map.py closure`** also reads the map's own build list (`asset_pipeline/s30_dedi_maps/<map>/build`).
- Map paks with no sequences (`freedm_skulltown`, `quicklighting_test`) skip AnimConv.

## Test dedi

`build\s3_server_mapport`: `paks\` and `vpk\` hard-linked to `build\s3_server` (3.4 GB, the dedi never writes
them), everything else a real copy, `platform\logs` empty. Installs there replace the link (`.s21` = the shared
original). `dedi_map_test.ps1` only stops dedis whose path is under that folder.

## Findings

- **`mp_rr_canyonlands_hu_avt*` crashed twice before ready.**
  1. `sh_crypto_tt_common.nut` `GetMapPieces()` picks the hu holo map pieces only for the exact name
     `mp_rr_canyonlands_hu`; for the `hu_avt` maps it fell back to `crypto_holo_map_01..05`, which their paks do not
     have, and the `error.rmdl` fallback (no sequences) crashed the engine (`r5apex_ds+0x1E6566`, read 0x8). The
     check is now `GetMapName().find( "mp_rr_canyonlands_hu" ) == 0` (test dedi copy; original `.s21`).
  2. Then `r5apex_ds+0xD58DF7` (read 0x10) on the first frame after ready: the engine's script-anim-window list
     (static vector at `r5apex_ds+0x172E148`, entries carrying the `settings/scriptAnimWindow/...` name at +0x10)
     had 2,639 entries with the last 10 NULL -- the 10 crypto satellite settings the map's sequences name but the
     first build left out (no layout at export, above). Found with cdb on the test dedi.
- **Loot drones** (`thunderdome`, `freedm_skulltown`, `divided_moon_mu1(_uh)`, `canyonlands_hu_avt*`): the S30 BSPs have drone path
  nodes the old ones lacked, and `worldsystems/_drones.nut` / `_loot_ticks.nut` spawn `npc_frag_drone`
  (`mdl/drone_frag_loot`), whose attachments the shared `common` conversion loses (docs/10: 330 struct-relative
  strings rmdlconv cannot encode). Server script errors "Missing REF attachment" / "Invalid attachmentIndex"; the
  dedi stays up. The S21-era `mp_rr_canyonlands_hu` on this dedi shows the same errors. A fix belongs in the
  shared paks (rmdlconv), not in the map port.
- `mp_rr_desertlands_mu5`: R5-AnimConv did not convert `animseq/props/prowler_hatch_tt/close_idle.rseq` (also not on
  the retries); its rig drops that sequence (`build/missing.txt`).

## Install into the workspace dedi

```
pwsh -File F:\apex_builds\m2299605425033557595\sdk_port\r5sdk\s30\tools\install_maps.ps1 [-Maps <map>,...]
```
Refuses while a dedi runs from `build\s3_server`. Per map it copies from `build\s3_server_mapport`:
`paks\Win64_server\<map>.rpak`, `vpk\englishserver_<map>.bsp.pak000_dir.vpk`, `vpk\server_<map>.bsp.pak000_000.vpk`;
for the `canyonlands_hu_avt` maps also `platform\scripts\vscripts\mp\levels\sh_crypto_tt_common.nut`. Replaced files
are kept as `.s21`.
