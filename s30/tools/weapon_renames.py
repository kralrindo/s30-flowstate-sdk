"""Find server weapon scripts whose S30 counterpart has a different name.

For every server weapon .txt with no S30 wepn of the same name, list the S30-only scripts
whose identifying keys (printname, shortprintname, weaponClass, weaponType, viewmodel,
playermodel) are all equal. A pair is reported only with that evidence; nothing is renamed.

    py weapon_renames.py <server scripts/weapons dir> <S30 wepn txt dir>
"""
import os
import re
import sys

KEYS = ("printname", "shortprintname", "weaponClass", "weaponType", "viewmodel", "playermodel")
KV = re.compile(r'^\t"(%s)"\s+"([^"]*)"' % "|".join(KEYS))


def ident(path):
    """Top-level (one tab deep) identifying keys of a weapon script."""
    out = {}
    for l in open(path, encoding="utf-8", errors="replace"):
        m = KV.match(l)
        if m and m.group(1) not in out:
            out[m.group(1)] = m.group(2).lower()
    return out


def main(server_dir, s30_dir):
    server = {f for f in os.listdir(server_dir) if f.endswith(".txt")}
    s30 = {f for f in os.listdir(s30_dir) if f.endswith(".txt")}
    s30_only = {f: ident(os.path.join(s30_dir, f)) for f in s30 - server}
    pairs, unmatched = [], []
    for f in sorted(server - s30):
        a = ident(os.path.join(server_dir, f))
        if not a.get("printname"):
            unmatched.append(f)
            continue
        hits = [g for g, b in s30_only.items() if all(a.get(k) == b.get(k) for k in KEYS)]
        if hits:
            pairs.append((f, hits))
        else:
            unmatched.append(f)
    print("server scripts with no S30 file of the same name: %d" % len(server - s30))
    print("renamed in S30 (all of %s equal): %d" % (", ".join(KEYS), len(pairs)))
    for f, hits in pairs:
        print("  %s -> %s" % (f[:-4], ", ".join(h[:-4] for h in sorted(hits))))
    print("no S30 match: %d" % len(unmatched))
    for f in unmatched:
        print("  " + f[:-4])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
