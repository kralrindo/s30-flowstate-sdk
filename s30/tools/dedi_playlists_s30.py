"""Builds the dedi's playlists_r5_patch.txt so the S30 client's lobby gets S30's playlist
schedules and rotations, while the S3 dedi keeps parsing its own data unchanged.

Facts (s30/docs/18-playlists.md):
  - The S30 client replaces its playlist data with the text the dedi sends in svc_Playlists
    (0x3F5CF0 -> 0x3D9880 -> parser 0x3D7210). The dedi sends its raw file text
    (SendServerInfo 0x304BE0 copies the buffer the playlist reader filled).
  - S30's parser splits that text at "@@@@ -<[PaTcH]>- @@@@" (strstr on the raw text) and
    merges the part after it over the part before it (KV merge 0x5E0F20 mode 4).
  - S30 reads "PlaylistSchedule ranked { ... }" as node "PlaylistSchedule ranked" with value
    "ranked" (0x5DFA10); the schedule and rotation names are that value. The dedi file's
    quoted "PlaylistSchedule ranked" has no value, so S30 named the schedule "" and built
    "-ranked-0" (format "%s-%.*s-%zu" in 0x3D64C0).
  - The S3 dedi (r5apex_ds) has no PlaylistSchedule / PlaylistRotation / PaTcH strings, and
    its KeyValues reader (0x475DC0 / 0x4765E0) is stock Valve: it skips // comments, parses
    every top-level key as a root peer, and only logs errors (0x472C70 -> 0x43C980).

Output: <dedi base without its quoted rotation/schedule blocks>
        // @@@@ -<[PaTcH]>- @@@@          <- a comment to S3, the split point to S30
        playlists { Gamemodes{missing} Playlists{missing} PlaylistRotation.. PlaylistSchedule.. }

usage: py dedi_playlists_s30.py <dedi playlists_r5_patch.txt> <S30 playlists_r5.txt> <out file>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import playlist_kv as kv

MARKER = "@@@@ -<[PaTcH]>- @@@@"
UI_SCHEDULES = ("ranked", "regular_1", "regular_2", "ltm", "mixtape", "training", "firing_range")


def is_sched_or_rot(name):
    return name.startswith("PlaylistSchedule") or name.startswith("PlaylistRotation")


def root_children(nodes, label):
    root = kv.find(nodes, "playlists")
    if not root or root["children"] is None:
        sys.exit("%s: no playlists root" % label)
    return root["children"]


def node_text(text, n, indent="    "):
    # The node's own text, from its key to the end of its value/block.
    return indent + text[n["start"]:n["end"]].strip() + "\n"


def schedule_targets(rot_sched_nodes):
    """Playlist names a schedule or rotation entry points at (entry key up to its first space)."""
    out = []
    for n in rot_sched_nodes:
        for c in n["children"] or []:
            if c["name"] == "rotation":
                out += [r["name"].split(" ", 1)[0] for r in c["children"] or []]
            elif c["name"] in ("rotationStartTime", "vars"):
                continue
            else:
                out.append(c["name"].split(" ", 1)[0])
    return out


def inherit_of(n):
    v = kv.find(n["children"], "inherit")
    return v["value"] if v else None


def gamemodes_of(n):
    g = kv.find(n["children"], "gamemodes")
    return [c["name"] for c in (g["children"] or [])] if g else []


#------------------------------------------------------------------------------
# Gate emulations
#------------------------------------------------------------------------------
def s30_view(text):
    """The S30 client's playlist tree for text: split at the marker, merge (mode 4)."""
    i = text.find(MARKER)
    base = root_children(kv.parse(text[:i] if i >= 0 else text), "base")
    if i < 0:
        return base
    patch = root_children(kv.parse(text[i + len(MARKER):]), "patch")
    merge(base, patch)
    return base


def merge(base, patch):
    # 0x5E0F20 mode 4: a value node ("-" deletes) replaces the base node of that name; a
    # subtree merges into the same-named base subtree, or is appended.
    for p in patch:
        b = next((x for x in base if x["name"].lower() == p["name"].lower()), None)
        if p["value"] is not None:
            if b:
                base.remove(b)
            if p["value"] != "-":
                base.append(p)
        elif b and b["value"] is None and b["children"] is not None:
            merge(b["children"], p["children"] or [])
        else:
            base.append(p)


def s3_parse(text):
    """The S3 dedi's KeyValues reader (0x475DC0 / 0x4765E0): Valve semantics. Returns the
    list of root peers (name, tree) and the error count."""
    toks = kv.tokenize(text.split("\x00", 1)[0])
    pos = [0]
    errors = []

    def nxt():
        t = toks[pos[0]] if pos[0] < len(toks) else None
        pos[0] += 1
        return t

    def rec():
        nodes = []
        while True:
            k = nxt()
            if k is None:
                errors.append("got EOF instead of keyname")
                return nodes
            if k[0] == "}":
                return nodes
            v = nxt()
            if v is not None and v[0] == "cond":
                v = nxt()
            if v is None:
                errors.append("got NULL key at %d" % k[2])
                return nodes
            if v[0] == "}":
                errors.append("got } in key at %d" % v[2])
                return nodes
            if v[0] == "{":
                nodes.append((k[1], rec()))
            else:
                nodes.append((k[1], v[1]))
                t = toks[pos[0]] if pos[0] < len(toks) else None
                if t is not None and t[0] == "cond":
                    pos[0] += 1

    roots = []
    while pos[0] < len(toks):
        k = nxt()
        v = nxt()
        if v is not None and v[0] == "cond":
            v = nxt()
        if v is not None and v[0] == "{":
            roots.append((k[1], rec()))
        else:
            errors.append("root key %r without a block at %d" % (k[1], k[2]))
    return roots, errors


#------------------------------------------------------------------------------
def build(dedi_path, s30_path, out_path):
    dedi = open(dedi_path, encoding="utf-8", errors="strict", newline="").read()
    s30 = open(s30_path, encoding="utf-8", errors="strict", newline="").read().split("\x00", 1)[0]
    if MARKER in dedi:
        sys.exit("%s already has a patch section -- build from the original" % dedi_path)
    nl = "\r\n" if "\r\n" in dedi else "\n"

    dn = kv.parse(dedi)
    sn = kv.parse(s30)
    droot, sroot = root_children(dn, "dedi"), root_children(sn, "s30")

    # 1. Base: the dedi's text without its quoted rotation/schedule blocks (S30 cannot name
    #    them, S3 does not read them).
    cuts = sorted((n["start"], n["end"]) for n in droot if is_sched_or_rot(n["name"]))
    base, last = [], 0
    for s, e in cuts:
        base.append(dedi[last:s])
        last = e
    base.append(dedi[last:])
    base = "".join(base)

    # 2. Patch: S30's rotations and schedules, the playlists they name that the dedi lacks
    #    (with their inherit chains), and the gamemodes those use that the dedi lacks.
    s_rs = [n for n in sroot if is_sched_or_rot(n["name"])]
    s_pl = {n["name"]: n for n in kv.find(sroot, "Playlists")["children"]}
    d_pl = {n["name"] for n in kv.find(droot, "Playlists")["children"]}
    s_gm = {n["name"]: n for n in kv.find(sroot, "Gamemodes")["children"]}
    d_gm = {n["name"] for n in kv.find(droot, "Gamemodes")["children"]}
    rot_names = {n["value"] for n in s_rs if n["name"].startswith("PlaylistRotation")}

    add_pl, todo = [], [t for t in schedule_targets(s_rs) if t not in rot_names and t not in d_pl]
    while todo:
        name = todo.pop(0)
        if name in add_pl or name in d_pl:
            continue
        if name not in s_pl:
            sys.exit("S30 schedule target %s is not an S30 playlist" % name)
        add_pl.append(name)
        parent = inherit_of(s_pl[name])
        if parent and parent not in d_pl:
            todo.append(parent)
    add_gm = sorted({g for p in add_pl for g in gamemodes_of(s_pl[p]) if g not in d_gm})
    for g in add_gm:
        if g not in s_gm:
            sys.exit("gamemode %s is in no S30 Gamemodes block" % g)

    patch = ["playlists", "{"]
    if add_gm:
        patch += ["    Gamemodes", "    {"] + [node_text(s30, s_gm[g], "        ").rstrip("\n") for g in add_gm] + ["    }"]
    patch += ["    Playlists", "    {"] + [node_text(s30, s_pl[p], "        ").rstrip("\n") for p in sorted(add_pl)] + ["    }"]
    patch += [node_text(s30, n).rstrip("\n") for n in s_rs]
    patch += ["}"]

    header = (
        "// S30 client patch (s30/tools/dedi_playlists_s30.py, s30/docs/18-playlists.md): the S30 client merges"
        " what follows the marker over this file; the S3 dedi reads the marker line as a comment and the"
        " second root below as an unused peer (it logs KeyValues errors for the S30 syntax).")
    out = base.rstrip() + nl + nl + header + nl + "// " + MARKER + nl + nl.join(patch) + nl
    text = out.replace("\r\n", "\n").replace("\n", nl)

    # Gates ---------------------------------------------------------------------
    fail = []
    # S30: every name the lobby resolves exists.
    view = s30_view(text)
    v_pl = {n["name"]: n for n in kv.find(view, "Playlists")["children"]}
    v_gm = {n["name"] for n in kv.find(view, "Gamemodes")["children"]}
    v_rs = [n for n in view if is_sched_or_rot(n["name"])]
    v_rot = {n["value"]: n for n in v_rs if n["name"].startswith("PlaylistRotation")}
    v_sched = {n["value"]: n for n in v_rs if n["name"].startswith("PlaylistSchedule")}
    for n in v_rs:
        if not n["value"]:
            fail.append("S30: %s has no name (value)" % n["name"])
    for s in UI_SCHEDULES:
        if s not in v_sched:
            fail.append("S30: lobby schedule %s missing" % s)
    for t in schedule_targets(v_rs):
        if t not in v_rot and t not in v_pl:
            fail.append("S30: schedule/rotation target %s resolves to nothing" % t)
    for p in add_pl:
        chain, cur = [], p
        while cur:
            if cur not in v_pl:
                fail.append("S30: %s inherits missing %s" % (p, cur))
                break
            chain.append(cur)
            for g in gamemodes_of(v_pl[cur]):
                if g not in v_gm:
                    fail.append("S30: %s uses missing gamemode %s" % (cur, g))
            cur = inherit_of(v_pl[cur])
            if cur in chain:
                fail.append("S30: inherit loop at %s" % cur)
                break
    # S3: the dedi's own root is the current file's, minus only the removed blocks.
    old_roots, old_err = s3_parse(dedi)
    new_roots, new_err = s3_parse(text)
    want = [(k, v) for (k, v) in old_roots[0][1] if not is_sched_or_rot(k)]
    if not new_roots or new_roots[0][0] != "playlists" or new_roots[0][1] != want:
        fail.append("S3: the dedi's first playlists root changed beyond the removed rotation/schedule blocks")
    if old_err:
        fail.append("S3: the current dedi file already has %d KeyValues errors: %s" % (len(old_err), old_err[:3]))
    print("S3 view: %d root(s); %d KeyValues errors, all in the S30 patch root (dedi's own root unchanged)"
          % (len(new_roots), len(new_err)))
    print("removed %d quoted rotation/schedule blocks; added %d gamemodes %s, %d playlists, %d rotations/schedules"
          % (len(cuts), len(add_gm), add_gm, len(add_pl), len(s_rs)))
    print("S30 view: schedules %s" % sorted(v_sched))
    if fail:
        for f in fail:
            print("GATE FAIL:", f)
        sys.exit(1)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("wrote %s (%d bytes); gates passed" % (out_path, len(text.encode("utf-8"))))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2], sys.argv[3])
