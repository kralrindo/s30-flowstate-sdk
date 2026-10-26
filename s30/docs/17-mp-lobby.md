# 17. S30 mp_lobby on the S3 dedi (2026-10-26)

The dedi's `mp_lobby` was S21's. It is rebuilt from S30's own map, the same way as the staging map (docs/10).

## Where it lives

| side | files |
|---|---|
| dedi | `vpk/englishserver_mp_common.bsp.pak000_dir.vpk` + `server_mp_common.bsp.pak000_000.vpk` (35 files: `maps/mp_lobby.bsp`, 19 `.bsp_lump`, 5 `.ent`, plus `scripts/entitlements.rson` (S30, docs/10), `playlists_r5.txt`, `gameinfo_r5.txt`, `cfg/aimassist`, 5 `chatfilters`, `particles/particles_manifest.txt`); `paks/Win64_server/mp_lobby.rpak` (already S30-built, 17 models); loose `platform/scripts/levels/mp_lobby.rson` (`IsTestMap: false`) |
| S30 depot | `paks/Win64/mp_lobby_client_perm.rpak` + `_client_temp.rpak` (wrap assets = the BSP and its lumps), `mp_lobby.rpak`, `mp_lobby_loadscreen.rpak`, `ui_lobby.rpak`; no VPK |
| dedi scripts | `dev_default` playlist (`cmdlineMapLoad 1`) runs survival on `mp_lobby`; `mp/_changemap.nut` returns to it |

## Build (`s30/tools/dedi_map.py mp_lobby export stage convert closure gate common`)

- `export`…`closure`: unchanged pipeline. RSX wrap export (65 files), `bspconv -dedi` v52 → v47 (sprp game-lump
  version 0x34 → 0x33), no heightfield lumps, 30 files. Closure: the BSP names 19 models, all in the dedi paks.
- `gate` (new, `dedi_bsp_gate.py`): the converted BSP against 4 shipped dedi maps (`mp_rr_aqueduct`,
  `arena_composite`, `arena_skygarden`, `party_crasher`, unpacked from the dedi's own server VPKs) and the staging
  conversion that runs on this dedi. Checks header (rBSP v47 rev 30), header length = lump file length for every lump,
  every shipped lump also shipped by a reference, lump versions, record size (lump length a multiple of the gcd of the
  references' lengths), static props (one `sprp`, version 0x33 like every reference; the 64-byte records, size from
  bspconv's source, fit the lump and each model index (u16 at +28, holds in every reference) is in the dictionary:
  254 props, 15 models), `.ent` headers (`ENTITIES02`). Result: ok. The same gate on the S21 lobby fails 42 checks
  (sprp v52, header lumps with no file, ...), so it does detect a mismatch. With only two references a gcd can be a
  multiple of the record size (lump 03: 36 vs 12), so it always runs with five.
- `common` (new): unpacks the **installed** `server_mp_common` (read only), replaces the 25 S21 `maps/mp_lobby*`
  files with the 30 converted ones (the S30 map also ships lumps 0002, 0036, 0050, 0052, 0055, as the staging
  conversion does), writes the manifest (map entries as `dedi_map.py`'s own server VPKs, the rest as unpacked), packs
  with `revpk`, then unpacks the result and requires every file to be byte-identical to the workspace, and every
  non-lobby file byte-identical to the installed VPK (10 files, including the S30 entitlements). Output:
  `asset_pipeline/mp_lobby_s30/vpk/`.

Since `common` starts from the installed VPK, rebuild it after anything else repacks `server_mp_common`
(e.g. `entitlements_dedi.py`).

## Install (not run yet)

```
pwsh -File F:\apex_builds\m2299605425033557595\asset_pipeline\mp_lobby_s30\install.ps1
```
Refuses while the workspace dedi runs. The S21 originals stay as `.s21`; the replaced pair is kept as `.pre_mp_lobby`.

## Differences that matter on the dedi

- Entities: S21 lobby 755, S30 272. Classes only in S30: `script_mover_lightweight` (the dedi has it),
  `exposure_volume` (also in the staging map; the dedi reaches ready with it and logs nothing about it) and
  `prop_uvanim` (not in the dedi binary; expected to be skipped like `exposure_volume`, **unverified** until a dedi
  run on mp_lobby).
- Script names: every `GetEnt(Array)ByScriptName` literal in the dedi's scripts that the S21 lobby has and the S30
  one lacks (7: `event_1_prop`, `event_portal_*`) is used only by `lobby/cl_lobby.gnut`, a CLIENT script (the S30
  client runs its own). No server script loses an entity.
- Spawn points: neither lobby has `info_player_start` (one `info_target` each), unchanged from the S21 lobby the dedi
  already runs.

## Open
- Not run on the dedi yet (this build did not touch `build/`): install, then `+map mp_lobby` (or the `dev_default`
  playlist) and check the dedi reaches ready and the `prop_uvanim` entity is skipped silently.
- `levels/mp_lobby.rson` stays the dedi's (`IsTestMap: false`); S30's copy is in its script rpak and was not compared.
