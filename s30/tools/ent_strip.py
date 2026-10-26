"""Remove entity blocks of the given classnames from a converted map's .ent files (docs/22 bisection).

    py ent_strip.py <bsp_dedi dir> <out dir> <classname> [<classname> ...]

Copies every file of <bsp_dedi dir> to <out dir>, then drops each { ... } block whose
"classname" is listed from the .ent files (the lump-0 ENTITIES copy is left alone; the dedi
spawns from the .ent files). Prints the count removed per class.
"""
import os
import re
import shutil
import sys


def main():
    src, dst, names = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)
    removed = {}
    for f in os.listdir(dst):
        if not f.endswith(".ent"):
            continue
        p = os.path.join(dst, f)
        data = open(p, "rb").read()
        head = data.split(b"\n", 1)[0] + b"\n" if data.startswith(b"ENTITIES") else b""
        body = data[len(head):]

        def keep(m):
            c = re.search(rb'"classname" "([^"]+)"', m.group(0))
            name = c.group(1).decode() if c else ""
            if name in names:
                removed[name] = removed.get(name, 0) + 1
                return b""
            return m.group(0)

        body = re.sub(rb"\{[^{}]*\}\s*", keep, body)
        open(p, "wb").write(head + body)
    print("removed:", removed)


if __name__ == "__main__":
    main()
