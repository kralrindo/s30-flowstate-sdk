"""Gate: each built dedi pak, re-listed by RSX, holds exactly its build list's assets, every
type at the version the S3 dedi reads.

    py dedi_pak_gate.py <build dir>     (build/<pak>.json + build/paks/<pak>.rpak)
"""
import collections
import csv
import json
import os
import subprocess
import sys

RSX = "F:/apex_builds/m2299605425033557595/tools_src/rsx/bin/Release/rsx.exe"
DEDI_VERSIONS = {"mdl_": "10", "aseq": "7", "arig": "4", "stgs": "1", "stlt": "0", "dtbl": "1", "txls": "1"}


def main(build):
    bad = 0
    for mp in sorted(f for f in os.listdir(build) if f.endswith(".json") and not f.startswith("build_list")):
        pak = mp[:-5]
        rpak = os.path.join(build, "paks", pak + ".rpak")
        lst = os.path.join(build, "paks", pak + ".list.csv")
        subprocess.run([RSX, "-nogui", "-nocachedb", "-noanimparse", "--list", lst, "--listformat", "csv", rpak],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        rows = [r for r in csv.DictReader(open(lst, encoding="utf-8", errors="replace")) if r["file_name"] == pak + ".rpak"]
        want = {int(e["$guid"], 16) for e in json.load(open(os.path.join(build, mp)))["files"]}
        got = {int(r["guid"], 16) for r in rows}
        wrong = collections.Counter((r["type"], r["pakver"]) for r in rows if DEDI_VERSIONS.get(r["type"]) != r["pakver"])
        ok = got == want and not wrong
        bad += not ok
        print("%-32s %s %6d assets %s%s" % (pak, "ok  " if ok else "FAIL", len(rows),
                                            dict(collections.Counter(r["type"] for r in rows)),
                                            "" if got == want else " missing %d extra %d" % (len(want - got), len(got - want))) +
              ("" if not wrong else " wrong versions %s" % dict(wrong)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
