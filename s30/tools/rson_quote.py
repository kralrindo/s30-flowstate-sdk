"""Rewrite an RSX-exported RSON list of flat blocks with every value quoted.

RSX writes `key: value` unquoted and leaves empty values bare (`steam_offer: `); the
dedi's own scripts/entitlements.rson quotes such values (`steam_offer: ""`, offer ids
with '-' and '.'). Each `key: value` line becomes `key: "value"`; braces and blank lines
are kept, indentation becomes one tab.

    py rson_quote.py <in.rson> <out.rson>
"""
import re
import sys

KV = re.compile(r'^\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*?)\s*$')


def main(src, dst):
    out, blocks = [], 0
    for line in open(src, encoding="utf-8", errors="replace").read().splitlines():
        s = line.strip()
        if s in ("{", "}", ""):
            out.append(s)
            blocks += s == "{"
            continue
        m = KV.match(line)
        if not m:
            sys.exit("unexpected line: %r" % line)
        value = m.group(2)
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        if '"' in value:
            sys.exit("value with a quote: %r" % line)
        out.append('\t%s: "%s"' % (m.group(1), value))
    open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    print("%d blocks -> %s" % (blocks, dst))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
