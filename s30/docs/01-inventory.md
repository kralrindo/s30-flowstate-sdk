# 1. Inventory (2026-10-25)

Read-only survey. `C:\R5Flowstate` is the reference install and is never written to; anything
needed from it is copied into the workspace `F:\apex_builds\m2299605425033557595`.

## Game builds

| build | path | exe | PE TimeDateStamp / SizeOfImage | notes |
|---|---|---|---|---|
| S30.1 client (target) | `F:\apex_builds\m2299605425033557595\depot` | `r5apex_dx12.exe` (42.9 MB) | `0x6AAB34CE` / `0xCD84000` | `R5pc_r5-301_J28_CL11570498_FSv30_1_2026_09_16_17_18`, v3.0.1.28. DX12 only, no DX11 exe. Not packed (no `.bind` section, normal `.text` entropy), so it can be analysed directly. No RTTI. 4,563 RPak v8 files, 66 GB of paks. No `vpk\` folder. |
| S21 client (reference) | `C:\R5Flowstate` | `r5apex.exe` (DX11), `r5apex_dx12.exe` | `0x666CB316` / `0x11A76000`, `0x666CB530` / `0x0E08B000` | `R5pc_r5-211_J12_CL6933961_2024_06_14`. `loader.dll` added to the import table through an extra `.l2` section. 4,385 RPak v8. |
| S3 dedicated server | `C:\R5Flowstate\r5apex_ds.exe` | `r5apex_ds.exe` | `0x5DBA2A2F` / `0x2A4B7000` | `R5pc_r5launch_N1094_CL456479_2019_10_30`. Also imports `loader.dll`. Carries both the server and client halves of every class. |
| live client | `A:\SteamLibrary\steamapps\common\Apex Legends` | `r5apex_dx12.exe` (68.7 MB, `0x6ABFE359`) | | Newer than S30.1 and packed (`.bind`). **Not used.** S30.1 is superseded and is not the live build. |

The S30 maps that ship in the depot (17 `_client_perm` sets): mp_lobby, mp_quicklighting_test, mp_rr_canyonlands_hu_avt(+_uh, _uh_night),
mp_rr_canyonlands_staging_mu1, mp_rr_desertlands_mu5, mp_rr_district_mu1(+_uh), mp_rr_divided_moon_mu1(+_uh),
mp_rr_freedm_skulltown, mp_rr_olympus_mu3(+_uh, _uh_snapback), mp_rr_thunderdome, mp_rr_tropic_island_mu2_landscape.

These five have server data in the reference dedi under the same name, built from **S21** data:
`mp_rr_canyonlands_staging_mu1`, `mp_rr_district_mu1`, `mp_rr_divided_moon_mu1`, `mp_rr_freedm_skulltown`, `mp_rr_thunderdome`.

## Reference install (`C:\R5Flowstate`, SDK `R5FlowstateSDK005`)

| item | what it is |
|---|---|
| `loader.dll` | Imported by both exes. Picks `client.dll` or `server.dll` from the host exe name. |
| `client.dll` (5.3 MB) + pdb | SDK client product: detour layer for S21 plus the S3→S21 net bridge. |
| `server.dll` (7.3 MB) + pdb | SDK server product, injected into the S3 dedi. |
| `platform\scripts\vscripts` | Flowstate Squirrel scripts: 950 `.nut` and 445 `.gnut`, shared by the dedi and the S21 client/UI VMs. `scripts.rson` has 3,029 lines. |
| `paks\Win64` | S21 client paks plus Flowstate client paks (`common_flowstate`, `effects_sdk`, map ports). |
| `paks\Win64_server` | 42 server rpaks: common, common_early, common_mp, common_flowstate, mp_lobby, root_lgnd_skins, per-map, and `sdk_s30_dedi`. |
| `vpk\` | 67 files: `englishserver_<map>.bsp.pak000_dir.vpk` dedi BSPs for 33 maps. |
| `platform\playlists_r5_patch.txt` | Playlists used by both ends (`-playlistFile`). |
| launch | `launch_server.bat`: `r5apex_ds.exe -dedicated -port 37015 +sv_allowSendTableTransmitToClients 1 ...`. `launch_client(_dx12).bat`: `-forceSendTable -offline -noorigin -sdk_safe_mode -devsdk -sdkstage2 ...`. |

## SDK source (`tools\s21-r5sdk` → `sdk_port\r5sdk`)

- Upstream `R5Flowstate/s21-r5sdk`, branch `s21-unify` @ `10077bb` ("1.3", 2026-10-08). The 1.4/1.5 tags are 2021 upstream tags and are not newer.
- 1,384 source files and about 462k lines, excluding thirdparty and protoc. Builds with CMake and MSBuild (VS 2026), Release only. Products: `client`, `server`, `loader`.
- 1,818 byte-signature literals: 1,472 `Module_FindPattern`, 132 `FindPatternSelf`, plus the bridge's own `NETOBS_*` table.
- The client net bridge (`src/engine/client/net_bridge_*`, `bridge_*`) is about 31k lines.

## Tools (all under `C:\Users\Lorry\Documents\apex\tools`)

| tool | commit | use here |
|---|---|---|
| RePak | `d5bf376` 2026-09-27 | build dedi/client rpaks |
| rsx | `c5813ed` 2026-09-29 | list/export S30 paks (aseq v14, arig v7, asqd v1 supported) |
| rmdlconv | `eae1e1f` 2026-10-06 | mdl_ conversion, down to dedi v8/v54 |
| R5-AnimConv | `a7567e6` 2026-10-13 | `-i 30 -o 3` S30 anims to the dedi |
| bspconv | `b8cc35b` 2026-09-27 | `-dedi` BSP for the S3 server |
| r5ed | `056aad5` 2026-10-14 | map compiler (not needed here) |
| s30-port-kit | `1e59657` 2026-10-24 | proven S30→S21 **map** pipeline incl. dedi rpak (docs/05) |
| revpk | built from the SDK | dedi VPKs |
| IDA Pro 9.1 | `C:\Program Files\IDA Professional 9.1` | existing S21 DX12 and S3 dedi IDBs in `Documents\apex\RE\idb`. S30 IDB built in batch mode (`idat -B`): `analysis\idb\r5apex_dx12_s30.exe.i64`. |
| Python | capstone 5.0.8, pefile, lief 0.17.6, minidump, frida 17 | analysis scripts in `s30/tools` |
| cdb | Windows Kits 10 Debuggers x64 | crash triage |

## Workspace layout

```
F:\apex_builds\m2299605425033557595\
  depot\            S30.1 client (pristine, never modified)
  sdk_port\r5sdk\   this repo (kralrindo/s30-flowstate-sdk); s30\ = port docs, tools, status
  analysis\         generated data: pattern_census.json, strings\ (S21/S30 string + RTTI dumps), idb\
  server_compat\  asset_pipeline\  assets_converted\  build\  logs\  tests\  docs\  backups\
```
