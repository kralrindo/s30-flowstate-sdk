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
        for f in glob.glob(DEDI_BUILD + "/*.json"):
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
            shutil.copy2(src, dst)
            print("installed", dst)


STEPS = ["export", "stage", "convert", "closure", "vpk", "install"]

if __name__ == "__main__":
    m = Map(sys.argv[1])
    for s in sys.argv[2:] or STEPS:
        if s not in STEPS:
            sys.exit("unknown step %s (%s)" % (s, ", ".join(STEPS)))
        print("== %s" % s)
        getattr(m, s)()
