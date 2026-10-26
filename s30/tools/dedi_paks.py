"""S30 paks -> S3 dedi paks of the same name (paks/Win64_server/<pak>.rpak).

The S3 dedi reads models, rigs, sequences, datatables, settings, layouts and texture
lists from its paks; it never touches materials, shaders or textures. Every dedi pak in
TARGETS is rebuilt 1:1 from its S30 source pak(s), converting the types whose version the
S3 dedi does not read:

    mdl_ v20 -> v10   rmdlconv -v19s30 (S30 studiohdr -> v54)
    arig v7  -> v4    R5-AnimConv -i 30 -o 3
    aseq v14 -> v7    (asqd v1 payloads are folded into the v7 sequences)
    stgs v2  -> v1    RePak's v1 writer from RSX's JSON export
    dtbl, stlt, txls  same version, repacked from RSX's export

Steps (default: all, in order):
    list      RSX --list + dependency lists of every source pak   -> inv/lists/
    export    RSX raw export of the types above                   -> export/
    models    rmdlconv -v19s30, one process per model             -> work/mdl_v54/
    anims     R5-AnimConv -i 30 -o 3                               -> work/anim_s3/
    buildlist one RePak dedi build list per target pak            -> build/
    repak     RePak (zstd)                                         -> build/paks/
    install   copy into the workspace dedi (originals kept as <pak>.rpak.s21)

    py dedi_paks.py [step ...]
"""
import csv
import glob
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtech_guid import string_to_guid  # noqa: E402
import dedi_coll_gate  # noqa: E402

WS = "F:/apex_builds/m2299605425033557595"
S30_PAKS = WS + "/depot/paks/Win64"
OUT = WS + "/asset_pipeline/s30_dedi"
SERVER_PAKS = WS + "/build/s3_server/paks/Win64_server"
DEDI_S21_LISTS = WS + "/asset_pipeline/inv/dedi_s21"   # RSX listings of the S21-built dedi paks
TOOLS = "C:/Users/Lorry/Documents/apex/tools"
RSX = WS + "/tools_src/rsx/bin/Release/rsx.exe"   # workspace build: mdl_ v20 raw export, layout-sized asqd, -noanimparse
RMDLCONV = WS + "/tools_src/rmdlconv/bin/Release/rmdlconv.exe"  # workspace build: S30 RUI table + scaled u16 offsets
ANIMCONV = TOOLS + "/R5-AnimConv/bin/Release_x64/R5-AnimConv.exe"
REPAK = WS + "/tools_src/RePak/bin/Release/repak.exe"   # workspace build: sub-layout order, vector mods, unregisteredRefsFile
OODLE = WS + "/tools_src/oodle_shim/oo2core_9_win64.dll"  # rmdlconv unpacks the S30 vertex groups with it


def root_skin_paks():
    # S30 splits the legend/weapon skin models into on-demand root_lgnd_skins_* paks; the S3
    # dedi loads them from one root_lgnd_skins.rpak. The _shdrs paks carry shaders only.
    return sorted(os.path.basename(p)[:-5] for p in glob.glob(S30_PAKS + "/root_lgnd_skins_*.rpak")
                  if not p.endswith("_shdrs.rpak") and "(" not in os.path.basename(p))


# (dedi pak, S30 source paks, asset filter) in the dedi's load order. A filter is an RSX
# listing whose guids the target keeps (common_roots: S21 packs the six common_early
# layouts/tables there again; the dedi never loads it, but it is rebuilt from S30 all the same).
TARGETS = [
    ("common_early", ["common_early"], None),
    ("common_roots", ["common_early"], DEDI_S21_LISTS + "/common_roots.csv"),
    ("common", ["common"], None),
    ("common_mp", ["common_mp"], None),
    ("mp_lobby", ["mp_lobby"], None),
    ("root_lgnd_skins", root_skin_paks(), None),
    ("mp_rr_canyonlands_staging_mu1", ["mp_rr_canyonlands_staging_mu1"], None),
]
# Assets a dedi pak carries on top of its S30 source (none: each pak is its S30 source).
EXTRA_ASSETS = {}
# Dedi paks left as they are, at their place in the load order (asset listing).
KEPT = {"sdk_s30_dedi": DEDI_S21_LISTS + "/sdk_s30_dedi.csv"}
LOAD_ORDER = ["common_early", "common_roots", "common", "common_mp", "mp_lobby", "root_lgnd_skins", "sdk_s30_dedi",
              "mp_rr_canyonlands_staging_mu1"]
# RSX runs: sources that reference each other load together; the root paks go in batches
# (one command line cannot name 800 paks).
GROUPS = [("commons", ["common", "common_mp", "common_early", "mp_lobby"])] + \
         [("roots_%02d" % (i // 80), root_skin_paks()[i:i + 80]) for i in range(0, len(root_skin_paks()), 80)] + \
         [("staging", ["mp_rr_canyonlands_staging_mu1"])]
# Sequences in a common pak whose owning model ships in a map pak: (map pak, model).
RIG_DONORS = [("mp_rr_canyonlands_hu_avt", "mdl/props/explosivehold_panel_animated/explosivehold_panel_animated.rmdl")]
TYPES = ["stlt", "stgs", "txls", "dtbl", "aseq", "arig", "mdl_"]  # tie-break order after dependencies
LISTS = OUT + "/inv/lists"
EXPORT = OUT + "/export"
WORK = OUT + "/work"
MODEL_SRC = OUT + "/work/mdl_src"
MODELS = OUT + "/work/mdl_v54"
ANIM_SRC = OUT + "/work/anim_src"
ANIMS = OUT + "/work/anim_s3"
BUILD = OUT + "/build"
LOGS = OUT + "/logs"

# Per-map mode (docs/22): `py dedi_paks.py --map <map> [--server <dedi dir>] [step ...]` builds
# only <map>.rpak, in asset_pipeline/s30_dedi_maps/<map>. The shared paks are taken as built:
# their RSX listings name assets and give dependencies, their build lists say what the dedi
# holds when the map pak loads, and their exported rigs drive the map's sequences on common rigs.
MAP = None
BASE_LISTS, BASE_BUILD, BASE_EXPORT = LISTS, BUILD, EXPORT
SHARED_PAKS = ["common_early", "common_roots", "common", "common_mp", "mp_lobby", "root_lgnd_skins"]
LIST_GROUPS = GROUPS   # the groups `list` and `export` run on


def map_mode(name, server_dir):
    global MAP, OUT, LISTS, EXPORT, WORK, MODEL_SRC, MODELS, ANIM_SRC, ANIMS, BUILD, LOGS
    global TARGETS, LOAD_ORDER, GROUPS, LIST_GROUPS, RIG_DONORS, SERVER_PAKS
    if not os.path.isfile(S30_PAKS + "/%s.rpak" % name):
        sys.exit("no S30 pak %s.rpak" % name)
    MAP = name
    OUT = WS + "/asset_pipeline/s30_dedi_maps/" + name
    LISTS, EXPORT, WORK = OUT + "/inv/lists", OUT + "/export", OUT + "/work"
    MODEL_SRC = WORK + "/mdl_src"
    MODELS = WS + "/asset_pipeline/s30_dedi_maps/mdl_v54"   # shared by every map: the hash cache keeps reuse exact
    ANIM_SRC, ANIMS = WORK + "/anim_src", WORK + "/anim_s3"
    BUILD, LOGS = OUT + "/build", OUT + "/logs"
    TARGETS = [(name, [name], None)]
    LOAD_ORDER = [p for p in LOAD_ORDER if p != "mp_rr_canyonlands_staging_mu1"] + [name]
    LIST_GROUPS = [(name, [name])]
    GROUPS = [g for g in GROUPS if g[0] != "staging"] + LIST_GROUPS
    RIG_DONORS = []
    if server_dir:
        SERVER_PAKS = server_dir.replace("\\", "/").rstrip("/") + "/paks/Win64_server"


def list_dir(group):
    return LISTS if MAP and group == MAP else BASE_LISTS


def fwd(name):
    return name.replace("\\", "/")


def run(cmd, log, env=None):
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env,
                           creationflags=subprocess.HIGH_PRIORITY_CLASS)
    if r.returncode:
        sys.exit("failed (%d): %s -- see %s" % (r.returncode, " ".join(cmd), log))


def read_csv(path):
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        return [r for r in csv.DictReader(f)]


def rows():
    """Every S30 listing row, tagged with the RSX group it came from."""
    out = []
    for name, _ in GROUPS:
        for r in read_csv("%s/%s.csv" % (list_dir(name), name)):
            r["group"] = name
            out.append(r)
    return out


def unnamed(asset_name):
    return asset_name.split("\\")[-1].startswith("0x")


def name_map(listed):
    """asset name (lowercase, forward slashes) -> guid, for named assets."""
    return {fwd(r["asset_name"]).lower(): int(r["guid"], 16) for r in listed if not unnamed(r["asset_name"])}


def guid_of(name, names):
    """Guid of an RSX asset name: '<type>\\0x<guid>' or a named path."""
    tail = name.replace("\\", "/").split("/")[-1]
    if tail.lower().startswith("0x"):
        return int(tail, 16)
    return names.get(fwd(name).lower())


def read_seq_list(path, key):
    """The names under `key:` in an RSX model/rig .rson sidecar."""
    if not os.path.isfile(path):
        return []
    m = re.search(r"%s:\s*\[(.*?)\]" % key, open(path, encoding="utf-8", errors="replace").read(), re.S)
    return [l.strip() for l in m.group(1).splitlines() if l.strip()] if m else []


def pak_paths(stems):
    return [S30_PAKS + "/%s.rpak" % p for p in stems]   # RSX applies the (01) patches itself


def step_list():
    os.makedirs(LISTS, exist_ok=True)
    for name, stems in LIST_GROUPS:
        run([RSX, "-nogui", "-nocachedb", "-noanimparse", "--list", "%s/%s.csv" % (LISTS, name), "--listformat", "csv",
             "--depfilepath", "%s/%s.deps.txt" % (LISTS, name), "--depfileformat", "adjlist"] + pak_paths(stems),
            "%s/rsx_list_%s.log" % (LOGS, name))


def step_export():
    shutil.rmtree(EXPORT, ignore_errors=True)
    for name, stems in LIST_GROUPS:
        run([RSX, "-nogui", "-export", "-exportfullpaths", "-nocachedb", "-noanimparse",
             "--exporttypes", "mdl_,arig,aseq,asqd,dtbl,stgs,stlt,txls", "--exportdir", EXPORT] + pak_paths(stems),
            "%s/rsx_export_%s.log" % (LOGS, name))
    # Map mode: a map pak's settings (mp_rr_canyonlands_hu_avt: 10 settings/scriptAnimWindow
    # assets its sequences name) export only with their layouts, which are in the shared paks.
    # Without them RSX writes nothing, the assets fall out of the build, and the dedi's anim
    # window list keeps 10 NULL entries it dereferences on the first frame (r5apex_ds+0xD58DF7).
    if MAP:
        stgs = [r["guid"] for r in read_csv("%s/%s.csv" % (LISTS, MAP)) if r["type"] == "stgs"]
        if stgs:
            guid_file = LISTS + "/stgs_guids.txt"
            open(guid_file, "w").write("".join("%s\n" % g for g in stgs))
            run([RSX, "-nogui", "-export", "-exportfullpaths", "-nocachedb", "-noanimparse", "--exporttypes", "stgs",
                 "--exportguids", guid_file, "--exportdir", EXPORT] + pak_paths(["common_early", "common", MAP]),
                "%s/rsx_export_stgs_%s.log" % (LOGS, MAP))
            print("export: %d settings with the shared layouts" % len(stgs))
            # RePak reads each setting's layout (json/csv + sub-layout folder) from the assets
            # dir; the layouts stay in the shared paks, the map pak only references them.
            layouts = set()
            for root, _, files in os.walk(EXPORT + "/settings"):
                for f in files:
                    if f.endswith(".json"):
                        m = re.search(r'"layoutAsset"\s*:\s*"([^"]+)"', open(os.path.join(root, f), encoding="utf-8").read())
                        if m:
                            layouts.add(fwd(m.group(1))[:-len(".rpak")] if m.group(1).endswith(".rpak") else fwd(m.group(1)))
            for stem in sorted(layouts):
                for ext in (".json", ".csv"):
                    if os.path.isfile(BASE_EXPORT + "/" + stem + ext):
                        os.makedirs(os.path.dirname(EXPORT + "/" + stem), exist_ok=True)
                        shutil.copyfile(BASE_EXPORT + "/" + stem + ext, EXPORT + "/" + stem + ext)
                if os.path.isdir(BASE_EXPORT + "/" + stem):
                    shutil.copytree(BASE_EXPORT + "/" + stem, EXPORT + "/" + stem, dirs_exist_ok=True)
            print("export: %d settings layouts copied from the shared export" % len(layouts))
    # A pak exported without the paks it references names those assets by guid in the
    # model/rig .rson sidecars (root skin models -> common rigs); name them.
    names = {g: n for n, g in name_map(rows()).items()}
    fixed = 0
    for root, _, files in os.walk(EXPORT):
        for f in files:
            if not f.endswith(".rson"):
                continue
            p = os.path.join(root, f)
            text = open(p, encoding="utf-8").read()
            new = re.sub(r"(?m)^(\s*)(?:0x)?([0-9A-Fa-f]{6,16})\s*$",
                         lambda m: m.group(1) + names.get(int(m.group(2), 16), m.group(2)).replace("/", "\\"), text)
            if new != text:
                open(p, "w", encoding="utf-8").write(new)
                fixed += 1
    print("export: %d .rson sidecars renamed guid references" % fixed)


def link_tree(src, dst, skip_ext=()):
    for root, _, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            if not f.endswith(skip_ext):
                os.link(os.path.join(root, f), os.path.join(dst, rel, f))


def file_hash(paths):
    h = hashlib.sha1()
    for p in paths:
        if os.path.isfile(p):
            h.update(open(p, "rb").read())
    return h.hexdigest()


def step_models():
    """The dedi pak carries no vertex data (RePak's dedi model writer never reads the .vg),
    so rmdlconv gets the models without it: the .rmdl/.phy it writes are byte-identical
    either way, and its v17 -> v10 vertex pass faults on some S30 models. A model whose
    source is unchanged since its last conversion is kept."""
    shutil.rmtree(MODEL_SRC, ignore_errors=True)
    env = dict(os.environ, OODLE_DLL=OODLE.replace("/", "\\"))
    cache_path = MODELS + "/.source_hashes.json"
    cache = json.load(open(cache_path)) if os.path.isfile(cache_path) else {}
    jobs = []
    for top in ("mdl", "mdl_derived"):
        for root, _, files in os.walk(EXPORT + "/" + top):
            for f in files:
                if f.endswith(".rmdl"):
                    rel = fwd(os.path.relpath(os.path.join(root, f), EXPORT))
                    h = file_hash([os.path.join(EXPORT, rel)[:-5] + e for e in (".rmdl", ".phy", ".rson")])
                    if cache.get(rel) == h and os.path.isfile(os.path.join(MODELS, rel)):
                        continue
                    jobs.append((rel, h))

    # One rmdlconv process per model: a model that faults the converter is recorded
    # instead of ending the batch for every model after it.
    def convert(n_job):
        n, (rel, h) = n_job
        src, out = "%s/%d" % (MODEL_SRC, n), "%s/%d.out" % (MODEL_SRC, n)
        stem = os.path.join(EXPORT, rel)[:-5]
        os.makedirs(os.path.dirname(os.path.join(src, rel)), exist_ok=True)
        for ext in (".rmdl", ".phy", ".rson"):
            if os.path.isfile(stem + ext):
                os.link(stem + ext, os.path.join(src, rel)[:-5] + ext)
        with open(src + ".log", "w", encoding="utf-8", errors="replace") as log:
            rc = subprocess.run([RMDLCONV, "-nopause", "-v19s30", src, out], stdout=log, stderr=subprocess.STDOUT,
                                env=env, creationflags=subprocess.HIGH_PRIORITY_CLASS).returncode
        ok = rc == 0 and os.path.isfile(os.path.join(out, rel))
        warnings = [l.strip() for l in open(src + ".log", encoding="utf-8", errors="replace") if "WARNING" in l]
        if ok:
            for ext in (".rmdl", ".phy"):
                dst = os.path.join(MODELS, rel)[:-5] + ext
                p = os.path.join(out, rel)[:-5] + ext
                if os.path.isfile(p):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    os.replace(p, dst)
                elif os.path.isfile(dst):
                    os.remove(dst)
            shutil.rmtree(src, ignore_errors=True)
            shutil.rmtree(out, ignore_errors=True)
            os.remove(src + ".log")
        return rel, h, rc, ok, warnings

    from concurrent.futures import ThreadPoolExecutor
    failed = []
    warn_log = open(LOGS + "/rmdlconv_warnings.txt", "a", encoding="utf-8")
    with ThreadPoolExecutor(8) as pool:
        for i, (rel, h, rc, ok, warnings) in enumerate(pool.map(convert, enumerate(jobs))):
            for w in warnings:
                warn_log.write("%s\t%s\n" % (rel, w))
            if ok:
                cache[rel] = h
            else:
                failed.append("%s rc=0x%X" % (rel, rc & 0xFFFFFFFF))
            if (i + 1) % 500 == 0:
                print("  %d/%d models, %d failed" % (i + 1, len(jobs), len(failed)))
    os.makedirs(MODELS, exist_ok=True)
    json.dump(cache, open(cache_path, "w"), indent=0)
    open(LOGS + "/rmdlconv_failed.txt", "w").write("\n".join(failed) + "\n")
    print("models: %d converted, %d failed, %d unchanged (logs/rmdlconv_failed.txt; per-model logs in work/mdl_src)"
          % (len(jobs) - len(failed), len(failed), len(cache) - (len(jobs) - len(failed))))
    if dedi_coll_gate.main(MODELS):
        sys.exit("models: collision gate failed")


def step_anims():
    """R5-AnimConv reads rigs (.rrig, plus self-rigged .rmdl) with the sequence list in their
    .rson. Models that drive sequences no rig lists ride along with their S30 .rmdl."""
    shutil.rmtree(ANIM_SRC, ignore_errors=True)
    shutil.rmtree(ANIMS, ignore_errors=True)
    names = {g: n for n, g in name_map(rows()).items()}
    for top in ("animrig", "animseq", "animseq_derived", "animseq_data"):
        if os.path.isdir(EXPORT + "/" + top):
            link_tree(EXPORT + "/" + top, ANIM_SRC + "/" + top)
    rig_seqs = set()
    for root, _, files in os.walk(EXPORT + "/animrig"):
        for f in files:
            if f.endswith(".rson"):
                rig_seqs.update(fwd(s).lower() for s in read_seq_list(os.path.join(root, f), "seqs"))
    n = 0
    for top in ("mdl", "mdl_derived"):
        for root, _, files in os.walk(EXPORT + "/" + top):
            for f in files:
                if not f.endswith(".rson"):
                    continue
                seqs = [fwd(s).lower() for s in read_seq_list(os.path.join(root, f), "seqs")]
                if not any(s.startswith("anim") and s not in rig_seqs for s in seqs):
                    continue
                rel = os.path.relpath(os.path.join(root, f), EXPORT)
                dst = os.path.join(ANIM_SRC, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                os.link(os.path.join(root, f), dst)
                os.link(os.path.join(root, f[:-5] + ".rmdl"), dst[:-5] + ".rmdl")
                n += 1
    # Sequences whose owner model ships in a map pak: the model rides along as their rig only.
    donors = WORK + "/rig_donors"
    shutil.rmtree(donors, ignore_errors=True)
    for pak, model in RIG_DONORS:
        guid_file = donors + "/%s.txt" % pak
        os.makedirs(donors, exist_ok=True)
        open(guid_file, "w").write("%016x\n" % string_to_guid(model))
        run([RSX, "-nogui", "-export", "-exportfullpaths", "-nocachedb", "-noanimparse", "--exporttypes", "mdl_",
             "--exportguids", guid_file, "--exportdir", donors, S30_PAKS + "/%s.rpak" % pak], LOGS + "/rsx_donor_%s.log" % pak)
        dst = os.path.join(ANIM_SRC, model)[:-5]
        if os.path.isfile(dst + ".rmdl"):
            continue   # the model is in a converted pak now
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.link(os.path.join(donors, model)[:-5] + ".rmdl", dst + ".rmdl")
        # RSX names the common-pak sequences by guid here (only the map pak was loaded).
        text = open(os.path.join(donors, model)[:-5] + ".rson", encoding="utf-8").read()
        text = re.sub(r"(?m)^(\s*)(?:0x)?([0-9A-Fa-f]{6,16})\s*$",
                      lambda m: m.group(1) + names.get(int(m.group(2), 16), m.group(2)).replace("/", "\\"), text)
        open(dst + ".rson", "w", encoding="utf-8").write(text)
        n += 1
    print("anim source: %d self-rigged models" % n)
    if not os.path.isdir(ANIM_SRC):
        print("anims: none in these paks")   # map paks with models only (mp_rr_freedm_skulltown)
        return
    run([ANIMCONV, ANIM_SRC, "-i", "30", "-o", "3", "-outpath", ANIMS, "-nopause", "-ne"], LOGS + "/animconv.log")

    # R5-AnimConv's parallel sequence parse faults on a few clips in a full run that convert
    # cleanly on their own; reconvert the owners of whatever is missing.
    owners = {}
    for root, _, files in os.walk(ANIM_SRC):
        for f in files:
            if f.endswith(".rson"):
                for s in read_seq_list(os.path.join(root, f), "seqs"):
                    owners.setdefault(fwd(s).lower(), set()).add(os.path.join(root, f)[:-5])
    for attempt in range(3):
        missing = [s for s in owners if not os.path.isfile(os.path.join(ANIMS, s))
                   and os.path.isfile(os.path.join(ANIM_SRC, s))]
        if not missing:
            break
        retry = WORK + "/anim_retry"
        shutil.rmtree(retry, ignore_errors=True)
        for top in ("animseq", "animseq_derived", "animseq_data"):
            if os.path.isdir(ANIM_SRC + "/" + top):
                link_tree(ANIM_SRC + "/" + top, retry + "/src/" + top)
        for stem in {o for s in missing for o in owners[s]}:
            rel = os.path.relpath(stem, ANIM_SRC)
            ext = ".rrig" if os.path.isfile(stem + ".rrig") else ".rmdl"
            os.makedirs(os.path.dirname(os.path.join(retry, "src", rel)), exist_ok=True)
            for e in (ext, ".rson"):
                os.link(stem + e, os.path.join(retry, "src", rel) + e)
        run([ANIMCONV, retry + "/src", "-i", "30", "-o", "3", "-outpath", retry + "/out", "-nopause", "-ne"],
            LOGS + "/animconv_retry%d.log" % attempt)
        got = 0
        for s in missing:
            p = os.path.join(retry, "out", s)
            if os.path.isfile(p):
                os.makedirs(os.path.dirname(os.path.join(ANIMS, s)), exist_ok=True)
                shutil.copyfile(p, os.path.join(ANIMS, s))
                got += 1
        print("anim retry %d: %d of %d missing sequences converted" % (attempt + 1, got, len(missing)))


def asset_file(t, path):
    """The file RePak reads for an entry, and the tree it lives in."""
    if t == "mdl_":
        return MODELS, path
    if t in ("aseq", "arig"):
        return ANIMS, path
    if t == "dtbl":
        return EXPORT, path[:-5] + ".csv"
    if t in ("stgs", "stlt", "txls"):
        return EXPORT, path[:-5] + ".json"
    raise KeyError(t)


def asset_path(t, name, g):
    name = fwd(name)
    if t == "dtbl":
        return "datatable/0x%X.rpak" % g if name.split("/")[-1].startswith("0x") else name + ".rpak"
    if t in ("stgs", "stlt", "txls") and not name.endswith(".rpak"):
        return name + ".rpak"
    return name


def read_deps(names):
    """guid -> set of guids it depends on, from the RSX dependency lists of every group."""
    deps = {}
    for name, _ in GROUPS:
        for line in open("%s/%s.deps.txt" % (list_dir(name), name), encoding="utf-8", errors="replace"):
            parts = line.rstrip("\r\n").split(",")
            g = guid_of(parts[0], names) if parts and parts[0] else None
            if g is None:
                continue
            ds = deps.setdefault(g, set())
            for d in parts[1:]:
                if not d:
                    continue
                dg = guid_of(d, names)
                if dg is None and re.fullmatch(r"[0-9A-Fa-f]{1,16}", d):
                    dg = int(d, 16)   # RSX writes a dependency it did not load as bare hex
                if dg is not None:
                    ds.add(dg)
    return deps


def dependency_order(entries, deps):
    """Dependencies before dependents within a pak: the dedi resolves a dependency when the
    asset is fixed up, and one not loaded yet stays a NULL head (datatables naming settings)."""
    by_guid = {int(e["$guid"], 16): e for e in entries}
    rank = {id(e): (TYPES.index(e["_type"]), e["_path"]) for e in entries}
    done, out = set(), []
    for e in sorted(entries, key=lambda e: rank[id(e)]):
        stack = [(int(e["$guid"], 16), iter(sorted(deps.get(int(e["$guid"], 16), ()))))]
        visiting = {stack[0][0]}
        if stack[0][0] in done:
            continue
        while stack:
            g, it = stack[-1]
            nxt = next((d for d in it if d in by_guid and d not in done and d not in visiting), None)
            if nxt is None:
                stack.pop()
                visiting.discard(g)
                done.add(g)
                out.append(by_guid[g])
            else:
                visiting.add(nxt)
                stack.append((nxt, iter(sorted(deps.get(nxt, ())))))
    return out


def step_buildlist():
    listed = rows()
    names = name_map(listed)
    deps = read_deps(names)

    def guids(rson, key):
        out = []
        for n in read_seq_list(rson, key):
            g = names.get(fwd(n).lower())
            if g is None and re.fullmatch(r"[0-9A-Fa-f]{1,16}", n):
                g = int(n, 16)
            if g is None:
                g = string_to_guid(fwd(n).lower())
            if g >> 32:   # 0xDF5-style sentinel slots crash the dedi's rig extension
                out.append(g)
        return out

    os.makedirs(BUILD, exist_ok=True)
    problems, per_pak = [], {}
    for pak, sources, flt in TARGETS:
        keep = {int(r["guid"], 16) for r in read_csv(flt)} if flt else None
        src_files = {s + ".rpak" for s in sources}
        extra = {n.lower() for n in EXTRA_ASSETS.get(pak, [])}
        entries, seen = [], set()
        per_pak[pak] = entries
        for r in listed:
            t, g = r["type"], int(r["guid"], 16)
            wanted = (r["file_name"] in src_files and (keep is None or g in keep)) or fwd(r["asset_name"]).lower() in extra
            if not wanted or t not in TYPES or g in seen:
                continue
            seen.add(g)
            path = asset_path(t, r["asset_name"], g)
            root, rel = asset_file(t, path)
            if not os.path.isfile(os.path.join(root, rel)):
                problems.append("%s %s: no %s" % (pak, t, os.path.join(root, rel)))
                continue
            e = {"_type": t, "_path": path, "$guid": "0x%016X" % g, "$assetsDir": root.rstrip("/") + "/"}
            if t == "mdl_":
                rson = os.path.join(EXPORT, path[:-5] + ".rson")
                rigs, seqs = guids(rson, "rigs"), guids(rson, "seqs")
                if rigs:
                    e["$animrigs"] = rigs
                if seqs:
                    e["$sequences"] = seqs
            elif t == "arig":
                e["$sequences"] = guids(os.path.join(EXPORT, path[:-5] + ".rson"), "seqs")
            entries.append(e)

    # What the dedi holds when each pak loads: everything earlier in LOAD_ORDER plus the pak
    # itself. A reference to anything else (a later pak, or a type the S3 dedi has no loader
    # for: odla/odlp/wepn...) can only be an unresolved dependency at load.
    contents = {p: {int(e["$guid"], 16) for e in es} for p, es in per_pak.items()}
    for p, lst in KEPT.items():
        contents[p] = {int(r["guid"], 16) for r in read_csv(lst)}
    if MAP:
        for p in SHARED_PAKS:
            contents[p] = {int(e["$guid"], 16) for e in json.load(open("%s/%s.json" % (BASE_BUILD, p)))["files"]}
    manifests = []
    for pak, _, _ in TARGETS:
        upto = LOAD_ORDER[:LOAD_ORDER.index(pak) + 1]
        available = set().union(*(contents[p] for p in upto if p in contents))
        entries = per_pak[pak]
        for e in entries:
            for key in ("$animrigs", "$sequences"):
                if key not in e:
                    continue
                gone = [g for g in e[key] if g not in available]
                if gone:
                    problems.append("%s %s %s: dropped unresolved %s" % (pak, e["_type"], e["_path"],
                                                                        ", ".join("0x%016X" % g for g in gone)))
                    e[key] = [g for g in e[key] if g in available]
                    if not e[key]:
                        del e[key]
        unregistered = sorted({d for e in entries for d in deps.get(int(e["$guid"], 16), ())} - available)
        refs = os.path.join(BUILD, pak + ".unregistered_refs.txt")
        open(refs, "w").write("".join("%016x\n" % g for g in unregistered))
        entries = dependency_order(entries, deps)
        mp = os.path.join(BUILD, pak + ".json")
        # compressLevel is read per pak map: the S3 dedi ignores a pak that is not zstd.
        json.dump({"version": 8, "name": pak, "assetsDir": EXPORT + "/", "outputDir": BUILD + "/paks/",
                   "compressLevel": 6, "compressWorkers": 16, "files": entries}, open(mp, "w"), indent=1)
        json.dump({"version": 8, "dedi": True, "keepDevOnly": False, "keepClientOnly": False,
                   "unregisteredRefsFile": refs, "outputDir": BUILD + "/paks/", "paks": [mp]},
                  open(os.path.join(BUILD, "build_list_%s.json" % pak), "w"), indent=1)
        counts = {}
        for e in entries:
            counts[e["_type"]] = counts.get(e["_type"], 0) + 1
        print("%s: %d assets %s, %d refs left unregistered" % (pak, len(entries), counts, len(unregistered)))
    open(os.path.join(BUILD, "missing.txt"), "w").write("\n".join(problems) + "\n")
    if problems:
        print("%d problems (build/missing.txt), e.g.:" % len(problems))
        for p in problems[:10]:
            print("  " + p)


def step_repak():
    out = BUILD + "/paks"
    shutil.rmtree(out, ignore_errors=True)   # a failed RePak leaves a stub pak behind
    os.makedirs(out)
    for pak, _, _ in TARGETS:
        run([REPAK, BUILD + "/build_list_%s.json" % pak], LOGS + "/repak_%s.log" % pak)
        p = os.path.join(out, pak + ".rpak")
        if not os.path.isfile(p):
            sys.exit("RePak wrote no %s" % p)
        with open(p, "rb") as f:
            head = f.read(0x84)
        if head[0x80:0x84] != b"\x28\xb5\x2f\xfd":   # the S3 dedi ignores a pak that is not zstd
            sys.exit("%s is not zstd-compressed" % p)
        print("%s %d bytes" % (p, os.path.getsize(p)))


def step_install():
    for pak, _, _ in TARGETS:
        dst = os.path.join(SERVER_PAKS, pak + ".rpak")
        bak = dst + ".s21"
        if os.path.isfile(dst) and not os.path.isfile(bak):
            os.replace(dst, bak)
        if os.path.isfile(dst):
            os.remove(dst)   # never write through a hard link (a test dedi linked to the workspace one)
        shutil.copyfile(os.path.join(BUILD, "paks", pak + ".rpak"), dst)
        print("installed", dst)


STEPS = {"list": step_list, "export": step_export, "models": step_models, "anims": step_anims,
         "buildlist": step_buildlist, "repak": step_repak, "install": step_install}

if __name__ == "__main__":
    args = sys.argv[1:]
    server = None
    if "--server" in args:
        i = args.index("--server")
        server = args[i + 1]
        del args[i:i + 2]
    if "--map" in args:
        i = args.index("--map")
        map_mode(args[i + 1], server)
        del args[i:i + 2]
    elif server:
        SERVER_PAKS = server.replace("\\", "/").rstrip("/") + "/paks/Win64_server"
    for s in args or list(STEPS):
        if s not in STEPS:
            sys.exit("unknown step %s (%s)" % (s, ", ".join(STEPS)))
        print("== %s" % s)
        STEPS[s]()
