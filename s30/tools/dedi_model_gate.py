"""Gate: S30-converted dedi models (v54) vs the S21 dedi's shipped v54 models of the same name.

Same-named models are often revised between seasons, so differences are expected; the gate
reports how many fields agree and flags systematic breakage: zero bones / hitbox sets /
bodyparts where the S21 model has them, collision dropped (bvh/phy) where S21 had it, bad
header identity, or a model whose bounds are degenerate.

    py dedi_model_gate.py <converted mdl_v54 root> <S21 dedi export root> <S30 raw export root>
"""
import collections
import os
import struct
import sys

# r5::v8::studiohdr_t (v54): id 0, version 4, checksum 8, sznameindex 12, name[64] 16, length 80,
# eyeposition 84, illumposition 96, hull_min 108, hull_max 120, view_bbmin 132, view_bbmax 144, flags 156.
FIELDS = [
    ("flags", 0x9C), ("numbones", 0xA0), ("numhitboxsets", 0xB0), ("numlocalseq", 0xC0),
    ("numtextures", 0xD0), ("numskinref", 0xE0), ("numskinfamilies", 0xE4), ("numbodyparts", 0xEC),
    ("numlocalattachments", 0xF4), ("numikchains", 0x124), ("numlocalposeparameters", 0x134),
]
HULL = 0x6C   # hull_min, hull_max, view_bbmin, view_bbmax: 12 floats
MASS, CONTENTS = 0x150, 0x154


def header(path):
    d = open(path, "rb").read()
    if d[:4] != b"IDST" or struct.unpack_from("<i", d, 4)[0] != 54:
        return None
    h = {n: struct.unpack_from("<i", d, o)[0] for n, o in FIELDS}
    h["bounds"] = struct.unpack_from("<12f", d, HULL)
    h["mass"] = struct.unpack_from("<f", d, MASS)[0]
    h["contents"] = struct.unpack_from("<i", d, CONTENTS)[0]
    h["phy"] = os.path.isfile(path[:-5] + ".phy")
    h["name"] = d[16:80].split(b"\0")[0].decode("latin1")
    return h


def main(new_root, ref_root, src_root):
    agree = collections.Counter()
    total = 0
    alarms = collections.defaultdict(list)
    bad_hdr = []
    for root, _, files in os.walk(ref_root):
        for f in files:
            if not f.endswith(".rmdl"):
                continue
            rel = os.path.relpath(os.path.join(root, f), ref_root)
            p = os.path.join(new_root, rel)
            if not os.path.isfile(p):
                continue
            a, b = header(p), header(os.path.join(root, f))
            if a is None:
                bad_hdr.append(rel)
                continue
            if b is None:
                continue
            total += 1
            for k in [n for n, _ in FIELDS] + ["mass", "contents", "phy"]:
                if a[k] == b[k]:
                    agree[k] += 1
            for k in ("numbones", "numhitboxsets", "numbodyparts"):
                if b[k] > 0 and a[k] == 0:
                    alarms["%s lost" % k].append(rel)
            if b["phy"] and not a["phy"] and os.path.isfile(os.path.join(src_root, rel)[:-5] + ".phy"):
                alarms["phy lost"].append(rel)   # only a loss if the S30 source had physics
            if all(abs(v) < 1e-6 for v in a["bounds"]) and any(abs(v) > 1e-6 for v in b["bounds"]):
                alarms["bounds zero"].append(rel)
    print("models compared:", total, " bad v54 header:", len(bad_hdr))
    for k in [n for n, _ in FIELDS] + ["mass", "contents", "phy"]:
        print("  %-24s %5d / %d agree" % (k, agree[k], total))
    for k, v in sorted(alarms.items()):
        print("ALARM %s: %d, e.g. %s" % (k, len(v), v[:3]))
    return 1 if bad_hdr or alarms else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
