"""Merge the S30.1 CLIENT and UI script trees into one S21-style folder.

    py merge_s30_scripts.py <client export> <ui export> <client scripts.rson> <ui scripts.rson> <out dir>

The exports are RSX `--exporttypes wrap` dumps of script_client(01).rpak / script_ui(01).rpak
(`.../wrap/scripts/vscripts`, files named `<path>.client` / `<path>.ui`); the rson lists are the
packs' unnamed `rson` assets (RSX `--exporttypes rson`).

Respawn's build resolves CLIENT / UI / SERVER per VM and blanks the lines of the other VMs, so a
shared file has the same line count in both packs and a line is either identical in both or
blank in one of them (checked: 272 shared files, never a differing line). Shared files get their
runs of client-only lines back inside `#if CLIENT` and UI-only runs inside `#if UI`; a file one
VM ships is copied as is (only that VM's list loads it). Everything else, `#if DEV` included, is
untouched. The lists merge as the client blocks with `When: "CLIENT"` and the UI blocks with
`When: "UI"`, a block's own condition kept: `When: "CLIENT && DEV"`. The scripts in
s30/scripts (stubs for the dev-only natives retail lacks) are copied in and load first in both
VMs under DEV. Statement calls of the dev-only natives no stub can stand in for
(COMMENT_CALLS) are commented out, as S21 did.
"""
import os
import re
import sys

SUFFIX = {"client": ".client", "ui": ".ui"}
EXTRA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts")
EXTRA_WHEN = "DEV"
# UpdateOVAssetUI: arrays of two element types, which no script signature takes.
COMMENT_CALLS = ["UpdateOVAssetUI"]
CALL_STMT = re.compile(r"^(\s*)((?:%s)\s*\(.*\)\s*;?\s*)$" % "|".join(COMMENT_CALLS))


def read_lines(path):
    data = open(path, "rb").read().decode("utf-8", errors="surrogateescape")
    newline = "\r\n" if "\r\n" in data else "\n"
    return data.replace("\r\n", "\n").split("\n"), newline


def collect(root, suffix):
    files = {}
    for dp, _, names in os.walk(root):
        for n in names:
            if n.endswith(suffix):
                rel = os.path.relpath(os.path.join(dp, n), root).replace("\\", "/")
                files[rel[: -len(suffix)]] = os.path.join(dp, n)
    return files


def merge_lines(client, ui):
    out = []
    mode = None  # None, "CLIENT", "UI"
    for c, u in zip(client, ui):
        # Whitespace-only lines are blank in both (a stray tab must not open a VM block).
        if c == u or (not c.strip(" \t\x00") and not u.strip(" \t\x00")):
            want, line = None, c
        elif not u.strip():
            want, line = "CLIENT", c
        elif not c.strip():
            want, line = "UI", u
        else:
            raise ValueError("lines differ: %r / %r" % (c, u))
        # A line blank in both keeps the current block open (no churn around blank lines).
        if want is None and not line.strip() and mode is not None:
            out.append(line)
            continue
        if want != mode:
            if mode is not None:
                out.append("#endif")
            if want is not None:
                out.append("#if " + want)
            mode = want
        out.append(line)
    if mode is not None:
        out.append("#endif")
    return out


def comment_calls(lines):
    """Comment out whole-line calls of COMMENT_CALLS; another use stops the merge."""
    out = []
    for line in lines:
        m = CALL_STMT.match(line)
        if m:
            line = m.group(1) + "//" + m.group(2)
        elif re.search(r"(?<![\w.])(?:%s)\b" % "|".join(COMMENT_CALLS), line) and not line.lstrip().startswith("//"):
            raise ValueError("not a call statement: %r" % line)
        out.append(line)
    return out


def rson_blocks(path):
    """[(condition, [scripts])] in list order."""
    text = open(path, "rb").read().decode("utf-8").replace("\r\n", "\n")
    blocks = []
    for m in re.finditer(r"When:\s*(\"[^\"]*\"|\S+)\s*Scripts:\s*\[(.*?)\]", text, re.S):
        cond = m.group(1).strip('"')
        scripts = [s.strip() for s in m.group(2).split("\n") if s.strip() and not s.strip().startswith("//")]
        blocks.append((cond, scripts))
    return blocks


def write_rson(path, sections):
    lines = ["// S30.1 CLIENT and UI script lists, merged (s30/tools/merge_s30_scripts.py).", "{"]
    for vm, blocks in sections:
        for cond, scripts in blocks:
            when = vm if cond == "true" else "%s && %s" % (vm, cond)
            lines.append('\tWhen: "%s"' % when)
            lines.append("\tScripts:")
            lines.append("\t[")
            lines.extend("\t\t" + s for s in scripts)
            lines.append("\t]")
            lines.append("")
    lines.append("}")
    open(path, "wb").write(("\r\n".join(lines) + "\r\n").encode("utf-8"))


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    client_root, ui_root, client_rson, ui_rson, out = sys.argv[1:]
    client = collect(client_root, SUFFIX["client"])
    ui = collect(ui_root, SUFFIX["ui"])
    shared = merged = 0
    for rel in sorted(set(client) | set(ui)):
        dst = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if rel in client and rel in ui:
            c, newline = read_lines(client[rel])
            u, _ = read_lines(ui[rel])
            if len(c) != len(u):
                sys.exit("%s: %d client lines, %d UI lines" % (rel, len(c), len(u)))
            lines = merge_lines(c, u)
            shared += 1
            merged += lines != c
        else:
            lines, newline = read_lines(client.get(rel) or ui[rel])
        lines = comment_calls(lines)
        open(dst, "wb").write(newline.join(lines).encode("utf-8", errors="surrogateescape"))
    extra = sorted(os.listdir(EXTRA_DIR)) if os.path.isdir(EXTRA_DIR) else []
    for name in extra:
        data = open(os.path.join(EXTRA_DIR, name), "rb").read()
        open(os.path.join(out, name), "wb").write(data)
    head = [(EXTRA_WHEN, extra)] if extra else []
    write_rson(os.path.join(out, "scripts.rson"), [("CLIENT", head + rson_blocks(client_rson)), ("UI", head + rson_blocks(ui_rson))])
    print("%d files (%d client, %d ui, %d shared, %d of them with VM blocks) -> %s" % (
        len(set(client) | set(ui)), len(client), len(ui), shared, merged, out))


if __name__ == "__main__":
    main()
