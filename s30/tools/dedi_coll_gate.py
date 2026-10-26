"""Gate: the BVH4 collision of every converted dedi model (v54), walked the way the engine
walks it.

Each part (r5::v8 mstudiocollheader_t) is traced from node 0: node indices must lie inside
the part's node array, child contents / bundle entries / surface props must index their
tables, and leaves must lie inside the part's leaf block. The last part's node array has no
successor offset, so it must hold exactly the nodes its tree reaches -- rmdlconv once sized
it from studiohdr offsets and dropped whole node arrays (184 models, projectile_rocket and
the firing range loot mats among them), leaving the header pointing past the model.

    py dedi_coll_gate.py <converted mdl_v54 root>
"""
import collections
import os
import struct
import sys

BVH_OFFSET = 0x214  # r5::v8::studiohdr_t bvhOffset


def u32(d, o):
    return struct.unpack_from("<I", d, o)[0]


def check(d):
    """Returns a list of problems in one model's collision (empty when it has none)."""
    bvh = u32(d, BVH_OFFSET)
    if not bvh:
        return []
    cm_i, sp_i, sn_i, nh = struct.unpack_from("<4i", d, bvh)
    ncm = (sn_i - cm_i) // 4
    nsp = (cm_i - sp_i) // 8
    errs = []
    if nsp < 0 or ncm <= 0 or nh <= 0:
        return ["collision tables: %d surface props, %d content masks, %d parts" % (nsp, ncm, nh)]
    for sp in range(nsp):
        if d[bvh + sp_i + 8 * sp + 3] >= ncm:
            errs.append("surface prop %d content mask out of range" % sp)
    hdrs = [struct.unpack_from("<4i", d, bvh + 16 + 32 * i) for i in range(nh)]
    for i, (_, node_i, vert_i, leaf_i) in enumerate(hdrs):
        node_end = hdrs[i + 1][1] if i + 1 < nh else len(d) - bvh
        leaf_end = hdrs[i + 1][2] if i + 1 < nh else hdrs[0][1]
        nnodes = (node_end - node_i) // 64
        nleaf = (leaf_end - leaf_i) // 4
        if leaf_i < vert_i or leaf_end < leaf_i:
            errs.append("part %d: leaf block [0x%x, 0x%x) out of order" % (i, leaf_i, leaf_end))
            continue
        leaf = lambda k: u32(d, bvh + leaf_i + 4 * k)
        stack, seen = [0], set()
        while stack:
            n = stack.pop()
            if n in seen:
                continue
            if n >= nnodes:
                errs.append("part %d: node %d past its %d-node array" % (i, n, nnodes))
                continue
            seen.add(n)
            meta = struct.unpack_from("<4I", d, bvh + node_i + 64 * n + 48)
            if (meta[0] & 0xFF) >= ncm:
                errs.append("part %d node %d: contents %d of %d" % (i, n, meta[0] & 0xFF, ncm))
            types = (meta[2] & 15, meta[2] >> 4 & 15, meta[3] & 15, meta[3] >> 4 & 15)
            for c in range(4):
                t, ci = types[c], meta[c] >> 8
                if t == 0:
                    stack.append(ci)
                elif t == 1:
                    continue
                elif ci >= nleaf:
                    errs.append("part %d node %d: type %d leaf %d past %d dwords" % (i, n, t, ci, nleaf))
                elif t == 3:
                    cnt = leaf(ci)
                    pay = ci + 1 + cnt
                    if pay > nleaf:
                        errs.append("part %d: bundle %d holds %d entries past the leaf block" % (i, ci, cnt))
                        continue
                    for k in range(cnt):
                        e = leaf(ci + 1 + k)
                        if (e & 0xFF) >= ncm:
                            errs.append("part %d: bundle %d entry %d contents %d" % (i, ci, k, e & 0xFF))
                        if 4 <= (e >> 8 & 0xFF) <= 7 and pay < nleaf and (leaf(pay) & 0xFFF) >= nsp:
                            errs.append("part %d: bundle %d entry %d surface prop %d" % (i, ci, k, leaf(pay) & 0xFFF))
                        pay += e >> 16
                    if pay > nleaf:
                        errs.append("part %d: bundle %d payload past the leaf block" % (i, ci))
                elif 4 <= t <= 7 and (leaf(ci) & 0xFFF) >= nsp:
                    errs.append("part %d node %d: surface prop %d of %d" % (i, n, leaf(ci) & 0xFFF, nsp))
        if i + 1 == nh and nnodes != len(seen):
            errs.append("last part: %d nodes stored, %d reachable" % (nnodes, len(seen)))
    return errs


def main(root):
    total = bad = 0
    kinds = collections.Counter()
    for top, _, files in os.walk(root):
        for f in files:
            if not f.endswith(".rmdl"):
                continue
            p = os.path.join(top, f)
            d = open(p, "rb").read()
            if len(d) < BVH_OFFSET + 4 or not u32(d, BVH_OFFSET):
                continue
            total += 1
            errs = check(d)
            if errs:
                bad += 1
                kinds[errs[0].split(":")[0] if ":" in errs[0] else errs[0]] += 1
                if bad <= 20:
                    print("FAIL %s: %s" % (os.path.relpath(p, root), "; ".join(errs[:3])))
    print("collision gate: %d models with collision, %d failed" % (total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
