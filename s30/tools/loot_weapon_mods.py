"""Weapon mods named by S30's survival_loot that the server weapon scripts do not define.

The server registers each survival_loot main-weapon row with GetWeaponInfoFileKeyField_WithMods,
which fails ("Invalid mod list") when a baseMods entry is not a mod of the row's weapon script.
For every main_weapon row this lists the baseMods missing from the server's
platform/scripts/weapons/<baseWeapon>.txt and whether S30's own weapon txt defines them.

With --apply, each missing mod S30 defines is copied from the S30 txt (RSX wepn export) into the
server txt's "Mods" block, before its closing brace (RSX's "// val unk" comment lines dropped;
original kept as .s21).

The S3 engine keeps only the first 31 mods of a weapon script (r5apex_ds+0x8E98C7: the parser's
32-slot array at WeaponData+0x2438 stops at index 0x1F) and silently drops the rest; a weapon over
the cap is reported, and a needed mod past it counts as missing. --remove <weapon>:<mod>[,<mod>...]
(repeatable) deletes those mod blocks from the server txt to make room.

    py loot_weapon_mods.py <S30 survival_loot.csv> <server weapons dir> <S30 weapons dir>
                           [--apply] [--remove <weapon>:<mod>[,<mod>...]]...
"""
import csv
import os
import re
import shutil
import sys

MAX_MODS = 31
TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([{}])|//[^\n]*')


def weapon_mods(path):
    """Names of the blocks directly under the top-level "Mods" block, in file order."""
    if not os.path.exists(path):
        return None
    text = open(path, encoding="utf-8", errors="replace").read()
    mods, depth, mods_depth, prev = [], 0, None, None
    for m in TOKEN.finditer(text):
        s, brace = m.group(1), m.group(2)
        if brace == "{":
            depth += 1
            if prev == "Mods" and mods_depth is None:
                mods_depth = depth
            elif mods_depth is not None and depth == mods_depth + 1 and prev is not None and prev not in mods:
                mods.append(prev)
            prev = None
        elif brace == "}":
            if mods_depth is not None and depth == mods_depth:
                mods_depth = None
            depth -= 1
            prev = None
        elif s is not None:
            prev = s
    return mods


def block_span(text, start):
    """(start, end) of the {...} block whose '{' is the first one at or after `start`."""
    i, depth = text.index("{", start), 0
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return i, j
    raise ValueError("unbalanced braces")


def mod_text(path, mod):
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r'^[ \t]*"%s"[ \t]*\r?\n[ \t]*\{' % re.escape(mod), text, re.M)
    _, end = block_span(text, m.end() - 1)
    lines = text[m.start():end + 1].replace("\r\n", "\n").split("\n")
    return "\n".join(l for l in lines if not l.strip().startswith("// val unk"))


def add_mods(path, blocks):
    if not os.path.exists(path + ".s21"):
        shutil.copy2(path, path + ".s21")
    raw = open(path, "rb").read().decode("utf-8", errors="replace")
    eol = "\r\n" if "\r\n" in raw else "\n"
    m = re.search(r'^[ \t]*"?Mods"?[ \t]*\r?\n[ \t]*\{', raw, re.M)
    _, close = block_span(raw, m.end() - 1)
    line_start = raw.rfind("\n", 0, close) + 1   # insert before the line holding Mods' closing brace
    add = "".join(eol + b.replace("\n", eol) + eol for b in blocks)
    open(path, "wb").write((raw[:line_start] + add.lstrip("\r\n") + eol + raw[line_start:]).encode("utf-8"))


def remove_mods(path, mods):
    if not os.path.exists(path + ".s21"):
        shutil.copy2(path, path + ".s21")
    raw = open(path, "rb").read().decode("utf-8", errors="replace")
    for mod in mods:
        m = re.search(r'^[ \t]*"%s"[ \t]*\r?\n[ \t]*\{' % re.escape(mod), raw, re.M)
        if not m:
            sys.exit("%s: no mod %s" % (path, mod))
        _, end = block_span(raw, m.end() - 1)
        end = raw.find("\n", end) + 1 or len(raw)   # through the closing brace's line
        raw = raw[:m.start()] + raw[end:]
    open(path, "wb").write(raw.encode("utf-8"))


def main(loot_csv, server_dir, s30_dir, apply=False, remove=()):
    for spec in remove:
        weapon, mods = spec.split(":")
        remove_mods(os.path.join(server_dir, weapon + ".txt"), mods.split(","))
        print("removed from %s: %s" % (weapon, mods.replace(",", " ")))
    rows = list(csv.DictReader(open(loot_csv, encoding="utf-8", errors="replace")))
    cache, missing = {}, []
    for i, r in enumerate(rows):
        if r["type"] != "main_weapon":   # also drops RSX's trailing column-type row
            continue
        weapon = r["baseWeapon"] or r["ref"]
        mods = [m for m in r.get("baseMods", "").split() if m]
        if not mods:
            continue
        for side, d in (("server", server_dir), ("s30", s30_dir)):
            if (side, weapon) not in cache:
                cache[(side, weapon)] = weapon_mods(os.path.join(d, weapon + ".txt"))
        have = cache[("server", weapon)]
        if have is None:
            missing.append((i, r["ref"], weapon, "<no server weapon txt>", None))
            continue
        for m in mods:
            if m not in have[:MAX_MODS]:
                s30 = cache[("s30", weapon)]
                missing.append((i, r["ref"], weapon, m if m not in have else m + " (past cap)",
                                None if s30 is None else m in s30))
    for i, ref, weapon, mod, in_s30 in missing:
        print("row %3d %-40s %-34s %-34s s30=%s" % (i, ref, weapon, mod, in_s30))
    print("%d missing (row, mod) pairs, %d distinct weapon/mod" % (len(missing), len({(w, m) for _, _, w, m, _ in missing})))
    over = sorted("%s (%d)" % (w, len(m)) for (side, w), m in cache.items() if side == "server" and m and len(m) > MAX_MODS)
    print("loot weapons over the %d-mod cap: %s" % (MAX_MODS, ", ".join(over) or "none"))
    if apply:
        per_weapon = {}
        for _, _, weapon, mod, in_s30 in missing:
            if in_s30 and not mod.endswith("(past cap)") and mod not in per_weapon.setdefault(weapon, []):
                per_weapon[weapon].append(mod)
        for weapon, mods in sorted(per_weapon.items()):
            add_mods(os.path.join(server_dir, weapon + ".txt"),
                     [mod_text(os.path.join(s30_dir, weapon + ".txt"), m) for m in mods])
            print("added to %s: %s" % (weapon, " ".join(mods)))
    return 0


if __name__ == "__main__":
    argv, args, remove = sys.argv[1:], [], []
    while argv:
        a = argv.pop(0)
        if a == "--remove":
            remove.append(argv.pop(0))
        elif a != "--apply":
            args.append(a)
    sys.exit(main(*args[:3], apply="--apply" in sys.argv, remove=remove))
