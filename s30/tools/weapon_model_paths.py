"""Point the dedi's weapon scripts at S30's model paths.

S30 renamed most weapon models (mdl/weapons/rspn101/ptpov_rspn101.rmdl ->
mdl/techart/mshop/weapons/class/assault/r301/r301_base_v.rmdl), and its weapon scripts
ship as wepn assets (RSX exports them as the same keyvalues .txt). For every server
weapon script with an S30 counterpart of the same name, each "<key>" "<...>.rmdl" value is
matched by its block path and key; where S30's value differs, the server script takes it.
Originals are kept as <file>.s21; a model that the dedi paks do not carry is reported.

    py weapon_model_paths.py <server scripts/weapons dir> <S30 wepn txt dir> <listing.csv> [...]
        [--apply]
"""
import csv
import os
import re
import sys

EMPTY = "mdl/dev/empty_model.rmdl"
LINE = re.compile(r'^(\s*)"([^"]+)"\s+"([^"]*\.rmdl)"(.*)$')
OPEN = re.compile(r'^\s*"?([^"\s{}]+)"?\s*$')


def model_keys(lines):
    """(block path, key, occurrence) -> (line index, value) for every .rmdl value."""
    out, path, pending, seen = {}, [], None, {}
    for i, l in enumerate(lines):
        s = l.strip()
        m = LINE.match(l)
        if m:
            k = ("/".join(path), m.group(2))
            n = seen.get(k, 0)
            seen[k] = n + 1
            out[k + (n,)] = (i, m.group(3))
            continue
        if s == "{":
            path.append(pending or "")
            pending = None
        elif s == "}":
            if path:
                path.pop()
        else:
            o = OPEN.match(l)
            pending = o.group(1) if o and not s.startswith("//") else pending
    return out


def main(argv):
    apply = "--apply" in argv
    args = [a for a in argv if a != "--apply"]
    server_dir, s30_dir, lists = args[0], args[1], args[2:]
    packed = set()
    for lst in lists:
        with open(lst, newline="", encoding="utf-8", errors="replace") as f:
            packed.update(r["asset_name"].replace("\\", "/").lower() for r in csv.DictReader(f) if r["type"] == "mdl_")
    changed_files = changes = skipped = 0
    unpacked = set()
    for f in sorted(os.listdir(server_dir)):
        sp, tp = os.path.join(server_dir, f), os.path.join(s30_dir, f)
        if not f.endswith(".txt") or not os.path.isfile(tp):
            continue
        src = open(sp, encoding="utf-8", errors="replace").read().split("\n")
        s30 = model_keys(open(tp, encoding="utf-8", errors="replace").read().split("\n"))
        edits = []
        for k, (i, old) in model_keys(src).items():
            new = s30.get(k, (None, None))[1]
            if not new or new.lower() == old.lower():
                continue
            # A placeholder on either side is a design choice, not a renamed model
            # (Flowstate's heirloom melee scripts keep empty_model and set models in script).
            if EMPTY in (old.lower(), new.lower()):
                skipped += 1
                continue
            if new.lower() not in packed:
                unpacked.add("%s: %s" % (f, new))   # keep the S21 value (S30 itself has a typo'd 'eapons' path)
                continue
            edits.append((i, old, new, k))
        if not edits:
            continue
        changed_files += 1
        for i, old, new, k in edits:
            m = LINE.match(src[i])
            src[i] = '%s"%s" "%s"%s' % (m.group(1), m.group(2), new, m.group(4))
            changes += 1
            print("%s: %s%s %s -> %s" % (f, (k[0] + "/") if k[0] else "", k[1], old, new))
        if apply:
            if not os.path.isfile(sp + ".s21"):
                os.replace(sp, sp + ".s21")
            open(sp, "w", encoding="utf-8", newline="").write("\n".join(src))
    print("%d values in %d scripts %s; %d placeholder differences left alone" %
          (changes, changed_files, "rewritten" if apply else "would change (dry run)", skipped))
    if unpacked:
        print("kept the S21 value, S30 model not in the dedi paks (%d): %s" % (len(unpacked), sorted(unpacked)[:20]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
