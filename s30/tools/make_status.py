"""Render the S30 port progress image (s30/status/status.svg) and its table view (STATUS.md).

Left treemap: every SDK byte signature that matches the S21 client image exactly once,
grouped by subsystem. Each cell is one signature:
  verified  - resolves on S30 and the target was checked by hand (s30/status/verified.json)
  resolved  - matches the S30 image exactly once, not yet checked
  ambiguous - matches the S30 image more than once
  missing   - no match on S30; needs a new signature or a rewrite

Right panel: integration milestones from s30/status/milestones.json; a milestone is done only
when its test passed.

usage: make_status.py <census.json> [out_dir]
"""
import collections
import datetime
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STATUS_DIR = os.path.join(HERE, "..", "status")

SURFACE = "#1e2127"
PANEL_GAP = "#2b3038"
TEXT = "#e6e6e6"
TEXT_MUTED = "#a8adb5"
STATE_COLOR = {
	"verified": "#3fb950",
	"resolved": "#238636",
	"ambiguous": "#b8860b",
	"missing": "#5b616b",
	"done": "#3fb950",
	"wip": "#b8860b",
	"todo": "#5b616b",
}
BADGE_LABEL = "#555b63"
BADGE_PARTIAL = "#d4a72c"
BADGE_FULL = "#3fb950"
FONT = "Segoe UI, Helvetica, Arial, sans-serif"
MIN_GROUP = 10


def esc(s):
	return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def sig_key(p):
	return "%s:%s:%s" % (p["file"], p["name"] or "", p["pattern"])


def classify(census, verified):
	groups = collections.defaultdict(list)
	for p in census["patterns"]:
		h = p["hits"]
		if h["s21dx11"]["n"] != 1 and h["s21dx12"]["n"] != 1:
			continue
		n30 = h["s30"]["n"]
		if n30 == 1:
			state = "verified" if sig_key(p) in verified else "resolved"
		elif n30 > 1:
			state = "ambiguous"
		else:
			state = "missing"
		parts = p["file"].split("/")
		group = "/".join(parts[:2]) if len(parts) > 2 else parts[0]
		groups[group].append((state, p))
	merged = collections.defaultdict(list)
	for g, items in groups.items():
		merged[g if len(items) >= MIN_GROUP else "other"].extend(items)
	order = {"verified": 0, "resolved": 1, "ambiguous": 2, "missing": 3}
	for items in merged.values():
		items.sort(key=lambda sp: (order[sp[0]], sp[1]["file"], sp[1]["line"]))
	return dict(merged)


def squarify(values, x, y, w, h):
	"""Squarified treemap. values sorted descending; returns rects in the same order."""
	rects = []
	values = list(values)
	total = float(sum(values))
	scale = w * h / total if total else 0
	areas = [v * scale for v in values]
	i = 0
	while i < len(areas):
		short = min(w, h)
		row = [areas[i]]
		j = i + 1

		def worst(r):
			s = sum(r)
			return max(max(s * s / (short * short * a), short * short * a / (s * s)) for a in r)

		while j < len(areas) and worst(row + [areas[j]]) <= worst(row):
			row.append(areas[j])
			j += 1
		s = sum(row)
		if w >= h:
			cw = s / h
			cy = y
			for a in row:
				ch = a / cw
				rects.append((x, cy, cw, ch))
				cy += ch
			x += cw
			w -= cw
		else:
			ch = s / w
			cx = x
			for a in row:
				cw = a / ch
				rects.append((cx, y, cw, ch))
				cx += cw
			y += ch
			h -= ch
		i = j
	return rects


def grid(n, x, y, w, h):
	"""Lay n near-square cells over a rect, row-major."""
	if n <= 0:
		return []
	best = None
	for cols in range(1, n + 1):
		rows = -(-n // cols)
		cw, ch = w / cols, h / rows
		score = max(cw / ch, ch / cw)
		if best is None or score < best[0]:
			best = (score, cols, rows)
	_, cols, rows = best
	cw, ch = w / cols, h / rows
	return [(x + (k % cols) * cw, y + (k // cols) * ch, cw, ch) for k in range(n)]


def badge(out, x, y, label, value, full):
	lw = 8 + 7.2 * len(label)
	vw = 10 + 7.6 * len(value)
	out.append('<rect x="%.0f" y="%.0f" width="%.0f" height="30" fill="%s"/>' % (x, y, lw, BADGE_LABEL))
	out.append('<rect x="%.0f" y="%.0f" width="%.0f" height="30" fill="%s"/>' % (x + lw, y, vw, BADGE_FULL if full else BADGE_PARTIAL))
	out.append('<text x="%.0f" y="%.0f" fill="#ffffff" font-size="15" text-anchor="middle">%s</text>' % (x + lw / 2, y + 20, esc(label)))
	out.append('<text x="%.0f" y="%.0f" fill="#111111" font-size="15" text-anchor="middle">%s</text>' % (x + lw + vw / 2, y + 20, esc(value)))
	return x + lw + vw + 8


def group_label(out, x, y, w, text):
	if w < 30:
		return
	maxch = int((w - 6) / 6.9)
	t = text if len(text) <= maxch else text[: max(3, maxch - 1)] + "…"
	out.append('<text x="%.1f" y="%.1f" fill="#ffffff" font-size="13" stroke="%s" stroke-width="3" paint-order="stroke">%s</text>' % (x + 4, y + 15, SURFACE, esc(t)))


def draw_cells(out, items, rx, ry, rw, rh, tip):
	for (cx, cy, cw, ch), (state, obj) in zip(grid(len(items), rx, ry, rw, rh), items):
		gap = 1.0 if min(cw, ch) > 4 else 0.5
		out.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"><title>%s</title></rect>' % (
			cx + gap / 2, cy + gap / 2, max(cw - gap, 0.4), max(ch - gap, 0.4), STATE_COLOR[state], esc(tip(state, obj))))


def legend(out, x, y, entries):
	for state, label in entries:
		out.append('<rect x="%.0f" y="%.0f" width="12" height="12" fill="%s"/>' % (x, y - 10, STATE_COLOR[state]))
		out.append('<text x="%.0f" y="%.0f" fill="%s" font-size="13">%s</text>' % (x + 17, y, TEXT_MUTED, esc(label)))
		x += 24 + 7.0 * len(label)


def render(census, verified, milestones, out_dir):
	groups = classify(census, verified)
	sig_counts = collections.Counter(s for items in groups.values() for s, _ in items)
	sig_total = sum(sig_counts.values())
	sig_ok = sig_counts["verified"] + sig_counts["resolved"]

	phases = milestones["phases"]
	ms_items = [(it["state"], it) for ph in phases for it in ph["items"]]
	ms_done = sum(1 for s, _ in ms_items if s == "done")

	W, H = 1280, 640
	out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" font-family="%s">' % (W, H, W, H, FONT)]
	out.append('<rect width="%d" height="%d" fill="%s"/>' % (W, H, SURFACE))
	out.append('<text x="24" y="52" fill="%s" font-size="32" font-weight="600">S30 Flowstate SDK status</text>' % TEXT)
	out.append('<line x1="24" y1="74" x2="%d" y2="74" stroke="%s"/>' % (W - 24, PANEL_GAP))
	bx = badge(out, 24, 96, "signatures*", "%.1f%%" % (100.0 * sig_ok / sig_total), sig_ok == sig_total)
	bx = badge(out, bx, 96, "verified", "%.1f%%" % (100.0 * sig_counts["verified"] / sig_total), sig_counts["verified"] == sig_total)
	badge(out, bx, 96, "milestones", "%d/%d" % (ms_done, len(ms_items)), ms_done == len(ms_items))

	# left: signatures
	lx, ly, lw, lh = 24, 196, 600, 360
	out.append('<text x="%d" y="186" fill="%s" font-size="17">SDK signatures on S30*: %.1f%% (%d/%d)</text>' % (lx, TEXT, 100.0 * sig_ok / sig_total, sig_ok, sig_total))
	names = sorted(groups, key=lambda g: -len(groups[g]))
	for g, (gx, gy, gw, gh) in zip(names, squarify([len(groups[g]) for g in names], lx, ly, lw, lh)):
		pad = 1.5
		draw_cells(out, groups[g], gx + pad, gy + pad, gw - 2 * pad, gh - 2 * pad,
			lambda s, p: "%s  %s:%d  %s" % (s, p["file"], p["line"], p["name"] or ""))
		group_label(out, gx + pad, gy + pad, gw, g.replace(".cpp", "").replace(".h", ""))

	# right: milestones
	rx, ry, rw, rh = 656, 196, 600, 360
	out.append('<text x="%d" y="186" fill="%s" font-size="17">Integration milestones: %d/%d tests passing</text>' % (rx, TEXT, ms_done, len(ms_items)))
	phase_rects = squarify([len(ph["items"]) for ph in phases], rx, ry, rw, rh)
	for ph, (px, py, pw, ph_h) in zip(phases, phase_rects):
		pad = 1.5
		items = [(it["state"], it) for it in ph["items"]]
		inner_top = py + pad + 20
		cells = grid(len(items), px + pad, inner_top, pw - 2 * pad, ph_h - 2 * pad - 20)
		out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="20" fill="%s"/>' % (px + pad, py + pad, pw - 2 * pad, PANEL_GAP))
		group_label(out, px + pad, py + pad + 1, pw, ph["name"])
		for (cx, cy, cw, ch), (state, it) in zip(cells, items):
			out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s"><title>%s %s: %s</title></rect>' % (
				cx + 1, cy + 1, cw - 2, ch - 2, STATE_COLOR[state], it["id"], esc(it["name"]), esc(it["test"])))
			label = it["id"]
			out.append('<text x="%.1f" y="%.1f" fill="#ffffff" font-size="12" stroke="%s" stroke-width="3" paint-order="stroke">%s</text>' % (cx + 5, cy + 16, STATE_COLOR[state], esc(label)))
			if cw > 60 and ch > 34:
				maxch = int((cw - 10) / 5.6)
				nm = it["name"] if len(it["name"]) <= maxch else it["name"][: maxch - 1] + "…"
				out.append('<text x="%.1f" y="%.1f" fill="#ffffff" font-size="11">%s</text>' % (cx + 5, cy + 31, esc(nm)))

	legend(out, lx, 584, [
		("verified", "verified (%d)" % sig_counts["verified"]),
		("resolved", "resolves, unchecked (%d)" % sig_counts["resolved"]),
		("ambiguous", "ambiguous (%d)" % sig_counts["ambiguous"]),
		("missing", "missing (%d)" % sig_counts["missing"]),
	])
	legend(out, rx, 584, [("done", "test passes"), ("wip", "in progress"), ("todo", "not started")])
	stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
	out.append('<text x="24" y="618" fill="%s" font-size="12">* a signature counts when it matches the S30 image exactly once; that does not prove it found the same function (see verified). S30.1 R5pc_r5-301 CL11570498. %s</text>' % (TEXT_MUTED, stamp))
	out.append("</svg>")
	open(os.path.join(out_dir, "status.svg"), "w", encoding="utf-8").write("\n".join(out))

	# table view
	md = ["# S30 port status", "", "Generated by `s30/tools/make_status.py` (%s). Image: `status.svg`." % stamp, "",
		"## Signatures", "", "Signatures that match the S21 client image exactly once, by subsystem.", "",
		"| subsystem | total | verified | resolves | ambiguous | missing | resolves % |", "|---|---:|---:|---:|---:|---:|---:|"]
	for g in names:
		c = collections.Counter(s for s, _ in groups[g])
		t = len(groups[g])
		md.append("| %s | %d | %d | %d | %d | %d | %.1f |" % (g, t, c["verified"], c["resolved"], c["ambiguous"], c["missing"], 100.0 * (c["verified"] + c["resolved"]) / t))
	md.append("| **all** | %d | %d | %d | %d | %d | %.1f |" % (sig_total, sig_counts["verified"], sig_counts["resolved"], sig_counts["ambiguous"], sig_counts["missing"], 100.0 * sig_ok / sig_total))
	md += ["", "## Milestones", "", "| id | phase | milestone | state | test |", "|---|---|---|---|---|"]
	for ph in phases:
		for it in ph["items"]:
			md.append("| %s | %s | %s | %s | %s |" % (it["id"], ph["name"], it["name"], it["state"], it["test"]))
	open(os.path.join(out_dir, "STATUS.md"), "w", encoding="utf-8").write("\n".join(md) + "\n")
	print("signatures %d/%d resolve (%d verified), milestones %d/%d" % (sig_ok, sig_total, sig_counts["verified"], ms_done, len(ms_items)))


def main():
	census = json.load(open(sys.argv[1]))
	out_dir = sys.argv[2] if len(sys.argv) > 2 else STATUS_DIR
	vpath = os.path.join(STATUS_DIR, "verified.json")
	verified = set(json.load(open(vpath))["verified"]) if os.path.exists(vpath) else set()
	milestones = json.load(open(os.path.join(STATUS_DIR, "milestones.json")))
	render(census, verified, milestones, out_dir)


if __name__ == "__main__":
	main()
