"""Model paths the dedi's scripts name that no dedi pak carries, and their S30 path.

Scans every .nut/.gnut/.txt/.csv/.rson under the server's platform/ for "....rmdl" strings,
checks each against the models in the rebuilt dedi paks (dedi_paks.py build lists), and for
a missing one lists the packed S30 models with the same file name. Only a unique same-name
model is offered as the S30 path; nothing is changed without --apply, and --apply rewrites
only those unique matches (originals kept as <file>.s21).

    py script_model_paths.py <server platform dir> <dedi build dir> [--apply]
"""
import collections
import json
import os
import re
import sys

PATH = re.compile(r'(?i)\bmdl[/\\][\w/\\.\-]+?\.rmdl\b')
EXTS = (".nut", ".gnut", ".txt", ".csv", ".rson")


def main(platform, build, apply):
    packed = set()
    for f in os.listdir(build):
        if f.endswith(".json") and not f.startswith("build_list"):
            packed |= {e["_path"].lower() for e in json.load(open(os.path.join(build, f)))["files"] if e["_type"] == "mdl_"}
    by_name = collections.defaultdict(set)
    for p in packed:
        by_name[p.rsplit("/", 1)[-1]].add(p)

    refs = collections.defaultdict(set)   # missing path -> files naming it
    for root, _, files in os.walk(platform):
        for f in files:
            if not f.endswith(EXTS):
                continue
            p = os.path.join(root, f)
            for m in PATH.finditer(open(p, encoding="utf-8", errors="replace").read()):
                path = m.group(0).replace("\\", "/").lower()
                if path not in packed:
                    refs[path].add(p)

    unique, ambiguous, none = {}, {}, []
    for path in sorted(refs):
        cands = by_name.get(path.rsplit("/", 1)[-1], set())
        if len(cands) == 1:
            unique[path] = next(iter(cands))
        elif cands:
            ambiguous[path] = sorted(cands)
        else:
            none.append(path)
    print("model paths named by scripts and not in the dedi paks: %d" % len(refs))
    print("same file name, one S30 model (%d):" % len(unique))
    for a, b in unique.items():
        print("  %s -> %s   [%d files]" % (a, b, len(refs[a])))
    print("same file name, several S30 models (%d):" % len(ambiguous))
    for a, bs in ambiguous.items():
        print("  %s -> %s" % (a, bs))
    print("no S30 model of that name (%d):" % len(none))
    for a in none:
        print("  %s   e.g. %s" % (a, sorted(refs[a])[0].replace(platform, "")))

    if apply:
        files = collections.defaultdict(list)
        for a, b in unique.items():
            for f in refs[a]:
                files[f].append((a, b))
        for f, pairs in files.items():
            text = open(f, encoding="utf-8", errors="replace").read()
            new = text
            for a, b in pairs:
                new = re.sub(re.escape(a).replace("/", r"[/\\]"), b, new, flags=re.I)
            if new != text:
                if not os.path.isfile(f + ".s21"):
                    os.replace(f, f + ".s21")
                open(f, "w", encoding="utf-8", newline="").write(new)
        print("rewrote %d files" % len(files))
    return 0


if __name__ == "__main__":
    a = [x for x in sys.argv[1:] if x != "--apply"]
    sys.exit(main(a[0], a[1], "--apply" in sys.argv))
