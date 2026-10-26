"""Compare S21 and S30 net message bodies from normalized Hex-Rays output (ida_decomp_dump.py).

The bit reader/writer is inlined, so a message's wire format shows up as the sequence of bit
widths it reads: `if ( v < N )` ... `v - N` for each inlined ReadUBitLong(N), and constant
arguments of out-of-line calls (`sub_X(a, N)`). Each matched pair (netmsg_match.py rows) is
compared on that width sequence and on the whole normalized text.

usage: netmsg_bodies.py <s21 dir> <s30 dir> <pairs "s21:s30,...">
"""
import difflib
import os
import re
import sys

WIDTH = re.compile(r"\bv < (\d+) \)|\bsub_X\(a, (\d+)\)|\b(\d+) - v\b")


def widths(text):
	"""Bit widths in read order. 32 is dropped (the word refill uses it on every read) and a run of
	the same width counts once (Hex-Rays emits one inlined read as one or two width tests)."""
	out = []
	for m in WIDTH.finditer(text):
		n = int(next(g for g in m.groups() if g))
		if n not in (0, 1, 32) and (not out or out[-1] != n):
			out.append(n)
	return out


def read(d, label):
	p = os.path.join(d, label + ".c")
	return open(p, encoding="utf-8").read() if os.path.exists(p) else ""


def main(d21, d30, pairs):
	for pair in pairs.split(","):
		a, b = (int(x) for x in pair.split(":"))
		for kind in ("rfb", "wtb"):
			t21, t30 = read(d21, "t%02d_%s" % (a, kind)), read(d30, "t%02d_%s" % (b, kind))
			w21, w30 = widths(t21), widths(t30)
			text = difflib.SequenceMatcher(None, t21.splitlines(), t30.splitlines()).ratio()
			same = "same widths" if w21 == w30 else "WIDTHS DIFFER"
			print("%2d -> %2d %s  text %.2f  %s  s21 %s  s30 %s" % (a, b, kind, text, same, w21, w30))


if __name__ == "__main__":
	main(*sys.argv[1:])
