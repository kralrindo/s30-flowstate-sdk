"""Gate: a converted dedi BSP (bspconv -dedi output) against BSPs the S3 dedi already loads.

    py dedi_bsp_gate.py <map dir> <map> <ref dir>[=<ref map>] ...

<map dir> holds <map>.bsp and its .bsp_lump / .ent files. Each reference is a folder with
<ref map>.bsp and its files (an unpacked dedi server VPK's maps/ folder, or a conversion that
runs on the dedi); the ref map name defaults to the one .bsp in the folder.

Checks, each against every reference:
  header    ident rBSP, version, map revision equal
  files     every lump with a length in the header has a .bsp_lump file of exactly that length,
            and no lump file is missing from the header
  set       every lump the map ships as a file is shipped as a file by a reference
  versions  each lump's header version equals the references'
  stride    a lump's length is a multiple of the gcd of that lump's lengths across the
            references (the record size, or a multiple of it), where that gcd is > 1
  sprp      game lump: one 'sprp' entry whose version equals the references'; the 64-byte
            static prop records fit the lump and each names a model in the dictionary (the
            same parse must hold for every reference)
  ent       each .ent starts with the references' ENTITIES02 header line
Exit status 1 when any check fails.
"""
import glob
import math
import os
import struct
import sys

GAME_LUMP = 0x23
# Lumps whose record size is known: a gcd over a few references can be a multiple of it
# (mp_rr_olympus_mu3 ships 3390 bytes of packed vertices; the references happen to be multiples of 48).
RECORD_SIZE = {0x14: 6}   # LUMP_PACKED_VERTICES: int16 x, y, z


def load(folder, name=None):
    if not name:
        bsps = [f for f in os.listdir(folder) if f.endswith(".bsp")]
        if len(bsps) != 1:
            sys.exit("%s: %d .bsp files, name the map" % (folder, len(bsps)))
        name = bsps[0][:-4]
    d = open(os.path.join(folder, name + ".bsp"), "rb").read()
    ident, ver, rev = struct.unpack_from("<4sii", d, 0)
    lumps = {}
    for i in range(128):
        off, ln, lv, fcc = struct.unpack_from("<iiii", d, 16 + 16 * i)
        lumps[i] = (ln, lv)
    files = {}
    for f in glob.glob(os.path.join(folder, name + ".bsp.*.bsp_lump")):
        files[int(f.split(".")[-2], 16)] = os.path.getsize(f)
    ents = sorted(glob.glob(os.path.join(folder, name + "_*.ent")))
    return {"folder": folder, "name": name, "ident": ident, "ver": ver, "rev": rev, "lumps": lumps, "files": files,
            "ents": ents}


def sprp(m):
    p = os.path.join(m["folder"], "%s.bsp.%04x.bsp_lump" % (m["name"], GAME_LUMP))
    d = open(p, "rb").read()
    count = struct.unpack_from("<i", d, 0)[0]
    if count != 1:
        return None, "game lump has %d entries" % count
    gid, flags, gver, gofs, glen = struct.unpack_from("<4sHHii", d, 4)
    if gid != b"prps":   # 'sprp' stored little-endian
        return None, "game lump id %r" % gid
    # the entry's data follows the 20-byte directory in this lump file
    body = d[20:20 + glen] if len(d) >= 20 + glen else None
    if body is None:
        return None, "game lump entry length %d past the file (%d)" % (glen, len(d))
    # [dict count][128-byte names][prop count][2 ints][64-byte props][tail]: StaticPropLump_t is
    # 64 bytes on S3 (bspconv), the model index is the u16 at +28 (holds in every reference)
    ndict = struct.unpack_from("<i", body, 0)[0]
    pos = 4 + 128 * ndict
    nprops = struct.unpack_from("<i", body, pos)[0]
    pos += 12
    if nprops < 0 or pos + 64 * nprops > len(body):
        return None, "%d props of 64 bytes past the lump (%d bytes after the header)" % (nprops, len(body) - pos)
    bad = [k for k in range(nprops) if struct.unpack_from("<H", body, pos + 64 * k + 28)[0] >= ndict]
    if bad:
        return None, "%d props name a model index >= %d (first prop %d)" % (len(bad), ndict, bad[0])
    used = {struct.unpack_from("<H", body, pos + 64 * k + 28)[0] for k in range(nprops)}
    return {"ver": gver, "stride": 64, "props": nprops, "dict": ndict, "used": len(used),
            "tail": len(body) - pos - 64 * nprops}, None


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    m = load(sys.argv[1], sys.argv[2])
    refs = []
    for a in sys.argv[3:]:
        folder, _, name = a.partition("=")
        refs.append(load(folder, name or None))
    fails = []

    def fail(msg):
        fails.append(msg)
        print("  FAIL", msg)

    print("map %s: rBSP v%d rev %d, %d lump files, %d .ent" % (m["name"], m["ver"], m["rev"], len(m["files"]), len(m["ents"])))
    for r in refs:
        if (m["ident"], m["ver"], m["rev"]) != (r["ident"], r["ver"], r["rev"]):
            fail("header %s v%d rev %d != %s %s v%d rev %d" % (m["ident"], m["ver"], m["rev"], r["name"], r["ident"], r["ver"], r["rev"]))
    for i, (ln, lv) in m["lumps"].items():
        if ln and i not in m["files"]:
            fail("lump %02x: header length %d, no file" % (i, ln))
        if i in m["files"] and m["files"][i] != ln:
            fail("lump %02x: file %d bytes, header %d" % (i, m["files"][i], ln))
    shipped = set()
    for r in refs:
        shipped |= set(r["files"])
    for i in sorted(m["files"]):
        if i not in shipped:
            fail("lump %02x: no reference ships it" % i)
        for r in refs:
            if i in r["files"] and r["lumps"][i][1] != m["lumps"][i][1]:
                fail("lump %02x: version %d, %s has %d" % (i, m["lumps"][i][1], r["name"], r["lumps"][i][1]))
        lens = [r["files"][i] for r in refs if r["files"].get(i)]
        g = 0
        for n in lens:
            g = math.gcd(g, n)
        g = RECORD_SIZE.get(i, g)
        if g > 1 and m["files"][i] % g:
            fail("lump %02x: %d bytes not a multiple of %d (gcd of %s)" % (i, m["files"][i], g, lens))
    sp, err = sprp(m)
    if err:
        fail("sprp: " + err)
    else:
        print("  sprp v%d: %d props of 64 bytes, %d model names (%d used), %d tail bytes" %
              (sp["ver"], sp["props"], sp["dict"], sp["used"], sp["tail"]))
        for r in refs:
            rs, rerr = sprp(r)
            if rerr:
                print("  (ref %s sprp: %s)" % (r["name"], rerr))
                continue
            if rs["ver"] != sp["ver"]:
                fail("sprp version %d, %s has %d" % (sp["ver"], r["name"], rs["ver"]))
    heads = {open(e, "rb").readline().split(b" ")[0] for r in refs for e in r["ents"]}
    for e in m["ents"]:
        h = open(e, "rb").readline().split(b" ")[0]
        if h not in heads:
            fail("%s: header %r, references %r" % (os.path.basename(e), h, heads))
    print("gate: %s (%d references)" % ("FAILED, %d checks" % len(fails) if fails else "ok", len(refs)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
