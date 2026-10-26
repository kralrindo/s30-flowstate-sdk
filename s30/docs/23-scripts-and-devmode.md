# 23 — CLIENT/UI scripts from disk, and dev mode

## Scripts from disk (`src/core_s30/scripts_disk_s30.cpp`)

S21-style: one folder, `platform/scripts/vscripts`, with each CLIENT/UI script once and one
`scripts.rson`. The client reads it instead of `script_client.rpak` / `script_ui.rpak` (both
still load, the engine needs them; a script the folder lacks still comes from the pak).

- Scripts: S30.1 compiles `scripts/vscripts/<name><.client|.ui>` (0xF591E0) through the
  filesystem ReadFile (0x4AA9D0), which serves the paks first and refuses `.nut`/`.gnut`
  past them. The ReadFile hook hands it the folder file's absolute forward-slash path (no pak
  has that) and the refusal's two `jne` are NOPed.
- Lists: `scripts/vscripts/scripts.rson.client|.ui` go through the rson loader 0x4B4770,
  which looks up the full path's GUID in the paks and otherwise opens the path minus its VM
  suffix from disk and parses it as text. The hook passes the folder's `scripts.rson`. The
  list conditions resolve `CLIENT`, `UI`, `SERVER` and `DEV` per VM (0xC09D10).
- Off with `-s30_rpakscripts`, or when the folder has no `scripts.rson`.

Log: `[SCRIPTS] scripts/vscripts/scripts.rson.ui from F:/.../scripts.rson` and
`the filesystem reads disk scripts by absolute, forward slashes path`.

### Building the folder

1. RSX export of the packs (`-exporttypes wrap` and `rson`) for `script_client(01).rpak` and
   `script_ui(01).rpak`.
2. `py s30/tools/merge_s30_scripts.py <client wrap> <ui wrap> <client rson> <ui rson> <out>`:
   shared files get client-only runs in `#if CLIENT` and UI-only runs in `#if UI` (Respawn's
   build blanks the other VM's lines, so the two copies line up); one-VM files are copied;
   the lists merge as `When: "CLIENT"` / `"UI"` (+ the block's own condition). It also copies
   `s30/scripts/*` (loaded first under DEV) and comments out the `COMMENT_CALLS`.
3. `py s30/tools/verify_merge.py <export root> <out>`: preprocesses the merged files as
   CLIENT/UI × DEV on/off and compares with the exports (3330 checks, 0 mismatches).

Re-running the merge overwrites the folder: edits made there are lost.

## Dev mode (`src/core_s30/devmode_s30.cpp`, `-dev` / `-developer`)

- `#if DEV` in scripts: the compiler folds the DEV node to a hardcoded false (0xF86B5E);
  patched to true.
- DEV list blocks: the `developer` convar; the command line gains `+developer 1`.
- Dev-only natives retail lacks: `s30/scripts/sh_dev_native_stubs_s30.gnut` defines empty
  stubs (AddNetPanelText, DevMenu_Alias_DEV / DevMenu_Rm_DEV, WarpMouseCursorDEV, the
  art viewer's OV* functions and ARTVIEWER_PROPERTIES_* ids). `UpdateOVAssetUI` takes arrays
  of two element types, which no script signature accepts, so its calls are commented (S21
  did the same).
- Dev-only convars retail lacks are registered (`S30ConCommand_RegisterConVar`, a ConVar
  built like the engine's statics) with the value of the script's non-DEV path.
  `s30/tools/dev_missing.py` lists DEV-only calls naming nothing the exe or scripts define.

Verified: UI and CLIENT compile with `-dev`, 12 dev convars registered, connected to the
dedi for 5+ minutes with no script errors.
