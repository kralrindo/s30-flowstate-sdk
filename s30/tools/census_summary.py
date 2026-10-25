"""Summarize pattern_census.json: classify patterns by product and S30 resolution."""
import collections, json, sys
d = json.load(open(sys.argv[1]))
P = d["patterns"]
def cls(p):
	h = p["hits"]
	cl = h["s21dx11"]["n"] >= 1 or h["s21dx12"]["n"] >= 1
	sv = h["s3ds"]["n"] >= 1
	return "client" if cl and not sv else "server" if sv and not cl else "both" if cl and sv else "none"
def s30(p):
	n = p["hits"]["s30"]["n"]
	return "unique" if n == 1 else "multi" if n > 1 else "miss"
tab = collections.Counter((cls(p), s30(p)) for p in P)
for c in ("client", "both", "server", "none"):
	row = {k: tab[(c, k)] for k in ("unique", "multi", "miss")}
	print("%-7s total=%4d  s30 unique=%4d multi=%3d miss=%4d" % (c, sum(row.values()), row["unique"], row["multi"], row["miss"]))
# S21-unique client patterns only
cu = [p for p in P if cls(p) in ("client", "both") and (p["hits"]["s21dx12"]["n"] == 1 or p["hits"]["s21dx11"]["n"] == 1)]
c2 = collections.Counter(s30(p) for p in cu)
print("client patterns unique on S21:", len(cu), dict(c2))
by = collections.defaultdict(collections.Counter)
for p in cu:
	top = "/".join(p["file"].split("/")[:2])
	by[top][s30(p)] += 1
for k, v in sorted(by.items(), key=lambda kv: -sum(kv[1].values()))[:30]:
	t = sum(v.values()); print("  %-40s %4d  unique %5.1f%%" % (k, t, 100 * v["unique"] / t))
