"""Comment out weapon-script callbacks whose script function the server does not define.

S30 weapon scripts copied onto the dedi name callbacks for content the Flowstate scripts
do not port yet (Snapback, ClockUp, Overload, the Norse heirloom...). For the given weapon
.txt files, every "<callback key>" "<function>" line whose function is not defined in any
server .nut/.gnut is prefixed with "// " (the key is kept in the comment).

    py weapon_comment_callbacks.py <server scripts dir> <list of weapon .txt names>
"""
import os
import re
import sys

CALLBACK = re.compile(r'^(\s*)"(On[A-Za-z]+|[A-Za-z_]+Callback[A-Za-z_]*)"\s+"([A-Za-z_][A-Za-z0-9_]*)"')


def main(scripts, names_file):
    defined = set()
    for root, _, files in os.walk(os.path.join(scripts, "vscripts")):
        for f in files:
            if f.endswith((".nut", ".gnut")):
                text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
                defined.update(re.findall(r"\bfunction\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(", text))
    total = 0
    for name in [l.strip() for l in open(names_file) if l.strip()]:
        path = os.path.join(scripts, "weapons", name)
        lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
        n = 0
        for i, l in enumerate(lines):
            m = CALLBACK.match(l)
            if m and m.group(3) not in defined:
                lines[i] = m.group(1) + "// " + l.lstrip()
                n += 1
        if n:
            open(path, "w", encoding="utf-8", newline="").write("\n".join(lines))
            print("%-55s %d callbacks commented" % (name, n))
            total += n
    print("total", total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
