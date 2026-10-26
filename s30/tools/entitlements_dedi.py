"""Give every entitlements.rson block the keys the S3 dedi requires.

The S3 dedi's entitlements loader (r5apex_ds 0x234A00) walks each block for "pc" and then
"pc_offer" and raises a fatal "Invalid entitlement in entitlements.rson for '%s'" /
"Invalid offerID ..." when one is missing; no other key is read. S30 has blocks with only
`enum` and `name` (BATTLEPASS_ULT, ...); for those `pc` is set to the `name` value (else the
enum) and `pc_offer` to "" (S3's own file has empty offers, e.g. TWITCH_PACK). Input is the
quoted file from rson_quote.py.

S30 also registers entitlement item flavors whose enum its own entitlements.rson does not list
(PSPLUS_PACK_28..32, XBOX_GOLD_PACK_17..22); the server scripts look every flavor's enum up in
the const table built from the rson. With the exported itemflav/entitlement settings dir given,
one block per such flavor is appended (after the S30 entries, so their indices stay), holding the
rson keys the flavor's settings carry.

    py entitlements_dedi.py <in.rson> <out.rson> [<settings/itemflav/entitlement dir>]
"""
import glob
import json
import re
import sys

KV = re.compile(r'^\t([A-Za-z_][A-Za-z0-9_]*): "(.*)"$')
RSON_KEYS = ("enum", "pc", "ps4", "xb1", "switch", "switch_2", "pc_offer", "steam_offer", "xb1_offer", "gdk")


def flavor_blocks(flavor_dir, listed):
    blocks = []
    for f in sorted(glob.glob(flavor_dir + "/*.json")):
        s = json.load(open(f, encoding="utf-8"))["settings"]
        if s.get("enum") and s["enum"] not in listed:
            if '"' in "".join(str(s.get(k, "")) for k in RSON_KEYS):
                sys.exit("value with a quote in %s" % f)
            blocks.append(["\t%s: \"%s\"" % (k, s[k]) for k in RSON_KEYS if k in s])
    return blocks


def main(src, dst, flavor_dir=None):
    out, block, fixed = [], None, []
    for line in open(src, encoding="utf-8").read().splitlines():
        if line == "{":
            block = []
            continue
        if line == "}":
            keys = dict(KV.match(l).groups() for l in block)
            if "pc" not in keys:
                block.append('\tpc: "%s"' % keys.get("name", keys["enum"]))
            if "pc_offer" not in keys:
                block.append('\tpc_offer: ""')
            if len(block) != len(keys):
                fixed.append(keys["enum"])
            out += ["{"] + block + ["}"]
            block = None
            continue
        if block is not None:
            if not KV.match(line):
                sys.exit("unexpected line: %r" % line)
            block.append(line)
        else:
            out.append(line)
    print("%d blocks given pc/pc_offer: %s" % (len(fixed), ", ".join(fixed)))
    if flavor_dir:
        listed = set(re.findall(r'^\tenum: "(.*)"$', "\n".join(out), re.M))
        added = flavor_blocks(flavor_dir, listed)
        for block in added:
            out += ["{"] + block + ["}"]
        print("%d flavor enums appended: %s" % (len(added), ", ".join(b[0].split('"')[1] for b in added)))
    open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:4]))
