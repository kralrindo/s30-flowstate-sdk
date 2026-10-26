"""Loose datatables (platform/datatable/**.csv) that shadow a datatable in the dedi paks.

The SDK loads a loose CSV in place of the pak's table of the same name; with the paks
rebuilt from S30 the rpak table takes priority (user, 2026-10-26), so a loose table whose
"datatable/<path>.rpak" guid is in a built pak is moved out (to <platform>/datatable_s21/,
same relative path). Loose tables no pak carries (Flowstate's own) stay.

    py loose_datatables.py <server platform dir> <dedi build dir> [--apply]
"""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rtech_guid import string_to_guid  # noqa: E402


def main(platform, build, apply):
    packed = set()
    for f in os.listdir(build):
        if f.endswith(".json") and not f.startswith("build_list"):
            packed |= {int(e["$guid"], 16) for e in json.load(open(os.path.join(build, f)))["files"] if e["_type"] == "dtbl"}
    root = os.path.join(platform, "datatable")
    shadowing, own = [], []
    for r, _, files in os.walk(root):
        for f in files:
            if not f.endswith(".csv"):
                continue
            rel = os.path.relpath(os.path.join(r, f), root).replace("\\", "/")
            g = string_to_guid("datatable/" + rel[:-4].lower() + ".rpak")
            (shadowing if g in packed else own).append(rel)
    print("loose tables: %d shadow a pak table, %d are loose-only" % (len(shadowing), len(own)))
    for rel in sorted(shadowing):
        print("  shadows pak:", rel)
    if apply:
        for rel in shadowing:
            src = os.path.join(root, rel)
            dst = os.path.join(platform, "datatable_s21", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.move(src, dst)
        print("moved %d to %s" % (len(shadowing), os.path.join(platform, "datatable_s21")))
    return 0


if __name__ == "__main__":
    a = [x for x in sys.argv[1:] if x != "--apply"]
    sys.exit(main(a[0], a[1], "--apply" in sys.argv))
