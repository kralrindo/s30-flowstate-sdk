"""S30 map BSP -> S3 dedi server VPK (vpk/server_<map>.bsp.pak000_*.vpk).

The dedi reads the map from its server VPK, and the static props / entities it names must
be in the dedi paks; an older BSP names models S30 no longer ships (staging_mu1's S21 BSP:
vistas, foliage, crypto holo maps), so the BSP is rebuilt from S30's own.

Steps (default: all):
    export   RSX wrap export of <map>_client_perm + _client_temp     -> <work>/export
    stage    maps/ lumps, .client stripped, lump ids lower-case     -> <work>/bsp_in
    convert  bspconv -dedi (v52 -> v47), then hf2tris (S3 has no
             heightfield BVH leaves)                                 -> <work>/bsp_dedi
    closure  every static-prop / entity model is in a dedi pak (the rebuilt paks' lists)
    vpk      revpk -pack english server                              -> <work>/vpk
    install  copy the VPK pair into the workspace dedi (originals kept as .s21)

A map that ships inside vpk/server_mp_common (mp_lobby) instead of its own server VPK:
    gate     dedi_bsp_gate.py against shipped dedi maps + the staging conversion
    common   the installed server_mp_common with this map's files replaced, repacked and
             round-trip checked                         -> asset_pipeline/<map>_s30/
             (install with asset_pipeline/<map>_s30/install.ps1)

    py dedi_map.py <map> [step ...]
"""
import csv
import glob
import json
import os
import re
import shutil
import subprocess
import sys

WS = "F:/apex_builds/m2299605425033557595"
S30_PAKS = WS + "/depot/paks/Win64"
TOOLS = "C:/Users/Lorry/Documents/apex/tools"
RSX = WS + "/tools_src/rsx/bin/Release/rsx.exe"
BSPCONV = TOOLS + "/bspconv/x64/Release/bspconv.exe"
HF2TRIS = TOOLS + "/s30-port-kit/tools/dedi/hf2tris.py"
REVPK_DIR = TOOLS + "/s21-r5sdk/game/bin"
MAP_MODELS = "C:/Users/Lorry/Documents/apex/olympus_night"   # map_models.py: static prop / entity model names
SERVER = WS + "/build/s3_server"
DEDI_BUILD = WS + "/asset_pipeline/s30_dedi/build"           # dedi_paks.py build lists = what the dedi loads


def run(cmd, log, cwd=None):
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=cwd, creationflags=subprocess.HIGH_PRIORITY_CLASS)
    if r.returncode:
        sys.exit("failed (%d): %s -- see %s" % (r.returncode, " ".join(cmd), log))


class Map:
    def __init__(self, name):
        self.m = name
        self.w = WS + "/asset_pipeline/s30_maps/" + name
        self.logs = self.w + "/logs"
        os.makedirs(self.logs, exist_ok=True)

    def export(self):
        shutil.rmtree(self.w + "/export", ignore_errors=True)
        run([RSX, "-nogui", "-export", "-exportfullpaths", "-nocachedb", "-noanimparse", "--exporttypes", "wrap",
             "--exportdir", self.w + "/export"] +
            ["%s/%s_client_%s.rpak" % (S30_PAKS, self.m, s) for s in ("perm", "temp")], self.logs + "/rsx_wrap.log")

    def stage(self):
        src, dst = self.w + "/export/wrap/maps", self.w + "/bsp_in"
        shutil.rmtree(dst, ignore_errors=True)
        os.makedirs(dst)
        n = 0
        for f in os.listdir(src):
            if not os.path.isfile(os.path.join(src, f)):
                continue   # maps/spl/<map>/ (splines) is not part of the BSP
            out = f[:-len(".client")] if f.endswith(".client") else f
            out = re.sub(r"\.([0-9A-Fa-f]{4})\.bsp_lump$", lambda m: "." + m.group(1).lower() + ".bsp_lump", out)
            shutil.copy2(os.path.join(src, f), os.path.join(dst, out))
            n += 1
        if not os.path.isfile("%s/%s.bsp" % (dst, self.m)):
            sys.exit("no %s.bsp in the wrap export" % self.m)
        print("stage: %d files" % n)

    def convert(self):
        out = self.w + "/bsp_dedi"
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        run([BSPCONV, "%s/bsp_in/%s.bsp" % (self.w, self.m), "-dedi", "-out", out + "/"], self.logs + "/bspconv.log")
        if os.path.isfile("%s/%s.bsp.0015.bsp_lump" % (self.w + "/bsp_in", self.m)):
            run([sys.executable, HF2TRIS, out, self.m, self.w + "/bsp_in"], self.logs + "/hf2tris.log")
            for f in glob.glob(out + "/*.prehf"):
                os.makedirs(self.w + "/prehf", exist_ok=True)
                shutil.move(f, self.w + "/prehf/" + os.path.basename(f))
        print("convert: %d files in bsp_dedi" % len(os.listdir(out)))

    def closure(self):
        sys.path.insert(0, MAP_MODELS)
        import map_models
        bsp = self.w + "/bsp_dedi"
        names = set(map_models.static_prop_models(map_models.lump(bsp, self.m, map_models.GAME_LUMP)))
        names |= set(map_models.entity_models(map_models.lump(bsp, self.m, map_models.ENTITIES).decode("latin1", "replace")))
        for ent in glob.glob(bsp + "/*.ent"):
            names |= set(map_models.entity_models(open(ent, encoding="latin1").read()))
        packed = set()
        # the shared paks, plus this map's own pak when dedi_paks.py --map built it (docs/22)
        for f in glob.glob(DEDI_BUILD + "/*.json") + glob.glob(WS + "/asset_pipeline/s30_dedi_maps/%s/build/*.json" % self.m):
            if "build_list" not in os.path.basename(f):
                packed |= {e["_path"].lower() for e in json.load(open(f))["files"] if e["_type"] == "mdl_"}
        # editor_effect_node_* are editor placeholders the game never spawns (user, 2026-10-26)
        names = {n.replace("\\", "/").lower() for n in names if n.lower().endswith(".rmdl")
                 and not n.replace("\\", "/").lower().startswith("mdl/dev/editor_effect_node")}
        missing = sorted(n for n in names if n not in packed)
        open(self.w + "/closure_missing.txt", "w").write("\n".join(missing) + "\n")
        print("closure: %d models named by the BSP, %d not in the dedi paks%s" %
              (len(names), len(missing), (": " + ", ".join(missing[:12])) if missing else ""))

    def vpk(self):
        ws = self.w + "/vpk_ws"
        shutil.rmtree(ws, ignore_errors=True)
        os.makedirs(ws + "/manifest")
        shutil.copytree(self.w + "/bsp_dedi", ws + "/maps")
        files = sorted(os.listdir(ws + "/maps"), key=lambda f: (not f.endswith(".bsp"), f.endswith(".ent"), f))
        out = ['"BuildManifest"', "{"]
        for f in files:
            out += ['\t"maps\\%s"' % f, "\t{", '\t\t"preloadSize"\t\t"0"', '\t\t"loadFlags"\t\t"257"',
                    '\t\t"textureFlags"\t\t"0"', '\t\t"useCompression"\t\t"1"', '\t\t"deDuplicate"\t\t"1"', "\t}"]
        out.append("}")
        open(ws + "/manifest/englishserver_%s.txt" % self.m, "w", newline="\r\n").write("\n".join(out) + "\n")
        shutil.rmtree(self.w + "/vpk", ignore_errors=True)
        os.makedirs(self.w + "/vpk")
        run([REVPK_DIR + "/revpk.exe", "-pack", "english", "server", self.m, ws + "/", self.w + "/vpk/", "-1", "default"],
            self.logs + "/revpk.log", cwd=REVPK_DIR)
        print("vpk:", sorted(os.listdir(self.w + "/vpk")))

    def install(self):
        for f in ("englishserver_%s.bsp.pak000_dir.vpk" % self.m, "server_%s.bsp.pak000_000.vpk" % self.m):
            src, dst = self.w + "/vpk/" + f, SERVER + "/vpk/" + f
            if not os.path.isfile(src):
                sys.exit("revpk wrote no " + src)
            if os.path.isfile(dst) and not os.path.isfile(dst + ".s21"):
                os.replace(dst, dst + ".s21")
            if os.path.isfile(dst):
                os.remove(dst)   # never write through a hard link (a test dedi linked to the workspace one)
            shutil.copy2(src, dst)
            print("installed", dst)


    # Shipped dedi server VPKs (S3 maps the dedi loads today) whose BSPs the gate compares against,
    # plus the staging conversion that runs on this dedi.
    GATE_REFS = ["mp_rr_aqueduct", "mp_rr_arena_composite", "mp_rr_arena_skygarden", "mp_rr_party_crasher"]

    def gate(self):
        refdir = WS + "/asset_pipeline/%s_s30/ref" % self.m
        refs = []
        for r in self.GATE_REFS:
            if not os.path.isfile("%s/%s/maps/%s.bsp" % (refdir, r, r)):
                unpack(SERVER + "/vpk/englishserver_%s.bsp.pak000_dir.vpk" % r, "%s/%s" % (refdir, r),
                       ["maps", "manifest", "scripts", "cfg"], "%s/%s.unpack.log" % (self.logs, r))
            refs.append("%s/%s/maps" % (refdir, r))
        refs.append(WS + "/asset_pipeline/s30_maps/mp_rr_canyonlands_staging_mu1/bsp_dedi")
        r = subprocess.run([sys.executable, os.path.dirname(os.path.abspath(__file__)) + "/dedi_bsp_gate.py",
                            self.w + "/bsp_dedi", self.m] + refs)
        if r.returncode:
            sys.exit("gate failed")

    def common(self):
        out = WS + "/asset_pipeline/%s_s30" % self.m
        src = SERVER + "/vpk/englishserver_mp_common.bsp.pak000_dir.vpk"
        cur = out + "/current"
        shutil.rmtree(cur, ignore_errors=True)
        # revpk -unpack drops files whose folder does not exist: unpack once for the manifest,
        # create every folder it names, unpack again
        unpack(src, cur, ["manifest"], self.logs + "/common_unpack1.log")
        paths = manifest_paths(cur + "/manifest/englishserver_mp_common.txt")
        unpack(src, cur, sorted({os.path.dirname(p) for p in paths if os.path.dirname(p)}), self.logs + "/common_unpack2.log")
        lost = [p for p in paths if not os.path.isfile(cur + "/" + p)]
        if lost:
            sys.exit("common: unpack lost %d files: %s" % (len(lost), lost[:8]))
        ws = out + "/ws"
        shutil.rmtree(ws, ignore_errors=True)
        shutil.copytree(cur, ws)
        mine = re.compile(r"^maps/%s(\.bsp|_)" % re.escape(self.m))
        old = [p for p in paths if mine.match(p)]
        for p in old:
            os.remove(ws + "/" + p)
        new = sorted(os.listdir(self.w + "/bsp_dedi"), key=lambda f: (not f.endswith(".bsp"), f.endswith(".ent"), f))
        for f in new:
            shutil.copy2(self.w + "/bsp_dedi/" + f, ws + "/maps/" + f)
        # manifest: this map's entries as dedi_map.py's own server VPKs write them, the rest as unpacked
        blocks = manifest_blocks(cur + "/manifest/englishserver_mp_common.txt")
        lines = ['"BuildManifest"', "{"]
        for f in new:
            lines += ['\t"maps\\%s"' % f, "\t{", '\t\t"preloadSize"\t\t"0"', '\t\t"loadFlags"\t\t"257"',
                      '\t\t"textureFlags"\t\t"0"', '\t\t"useCompression"\t\t"1"', '\t\t"deDuplicate"\t\t"1"', "\t}"]
        for p, body in blocks:
            if not mine.match(p):
                lines += ['\t"%s"' % p.replace("/", "\\"), "\t{"] + ["\t\t" + l.strip() for l in body.strip().splitlines()] + ["\t}"]
        lines.append("}")
        open(ws + "/manifest/englishserver_mp_common.txt", "w", newline="\r\n").write("\n".join(lines) + "\n")
        shutil.rmtree(out + "/vpk", ignore_errors=True)
        os.makedirs(out + "/vpk")
        run([REVPK_DIR + "/revpk.exe", "-pack", "english", "server", "mp_common", ws + "/", out + "/vpk/", "-1", "default"],
            self.logs + "/common_pack.log", cwd=REVPK_DIR)
        # round trip: the built pair unpacks to exactly ws (every file, byte for byte)
        ver = out + "/verify"
        shutil.rmtree(ver, ignore_errors=True)
        built = out + "/vpk/englishserver_mp_common.bsp.pak000_dir.vpk"
        unpack(built, ver, sorted({os.path.dirname(p) for p in manifest_paths(ws + "/manifest/englishserver_mp_common.txt")
                                   if os.path.dirname(p)} | {"manifest"}), self.logs + "/common_verify.log")
        want = sorted(manifest_paths(ws + "/manifest/englishserver_mp_common.txt"))
        got = sorted(manifest_paths(ver + "/manifest/englishserver_mp_common.txt"))
        bad = [p for p in want if not os.path.isfile(ver + "/" + p) or open(ver + "/" + p, "rb").read() != open(ws + "/" + p, "rb").read()]
        if want != got or bad:
            sys.exit("common: round trip differs (%d listed vs %d, %d files differ: %s)" % (len(want), len(got), len(bad), bad[:8]))
        same = [p for p in paths if not mine.match(p)]
        changed = [p for p in same if open(cur + "/" + p, "rb").read() != open(ws + "/" + p, "rb").read()]
        if changed:
            sys.exit("common: files outside %s changed: %s" % (self.m, changed))
        print("common: %d %s files replaced by %d; %d other files byte-identical to the installed VPK; round trip ok"
              % (len(old), self.m, len(new), len(same)))
        print("common: built", sorted(os.listdir(out + "/vpk")))


def unpack(vpk, out, folders, log):
    for f in folders:
        os.makedirs(out + "/" + f, exist_ok=True)
    run([REVPK_DIR + "/revpk.exe", "-unpack", vpk, out + "/"], log, cwd=REVPK_DIR)


def manifest_blocks(path):
    t = open(path, encoding="utf-8").read()
    return [(p.replace("\\", "/"), b) for p, b in re.findall(r'^\t"([^"]+)"\s*\{([^}]*)\}', t, re.M)]


def manifest_paths(path):
    return [p for p, _ in manifest_blocks(path)]


STEPS = ["export", "stage", "convert", "closure", "vpk", "install"]
EXTRA = ["gate", "common"]

if __name__ == "__main__":
    args = sys.argv[1:]
    if "--server" in args:   # install into another dedi copy (docs/22: the map-port test dedi)
        i = args.index("--server")
        SERVER = args[i + 1].replace("\\", "/").rstrip("/")
        del args[i:i + 2]
    m = Map(args[0])
    for s in args[1:] or STEPS:
        if s not in STEPS + EXTRA:
            sys.exit("unknown step %s (%s)" % (s, ", ".join(STEPS + EXTRA)))
        print("== %s" % s)
        getattr(m, s)()
