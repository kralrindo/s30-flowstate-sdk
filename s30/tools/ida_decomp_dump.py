"""Decompile a list of functions from an IDB into <outdir>/<label>.c, normalized for diffing
across builds: addresses (sub_/off_/dword_/qword_/unk_/loc_/byte_/word_/stru_/a...) become
placeholders and local variable numbers are dropped.

usage: ida_decomp_dump.py <db.i64> <outdir> <label>=<rva> [<label>=<rva> ...]
       ida_decomp_dump.py <db.i64> <outdir> @<file with label=rva lines>
"""
import os
import re
import sys

import idapro  # must be first

import ida_auto
import ida_hexrays
import ida_nalt

NORM = [
	(re.compile(r"\b(sub|off|dword|qword|unk|loc|byte|word|stru|xmmword|funcs|nullsub|j_sub)_[0-9A-F]+\b"), r"\1_X"),
	(re.compile(r"\ba[A-Z][A-Za-z0-9_]*\b"), "aSTR"),
	(re.compile(r"\b(v|a)\d+\b"), r"\1"),
	(re.compile(r"//.*"), ""),
]


def normalize(text):
	for rx, rep in NORM:
		text = rx.sub(rep, text)
	return "\n".join(l.rstrip() for l in text.splitlines() if l.strip())


def main(db, outdir, *specs):
	if specs and specs[0].startswith("@"):
		specs = [l.strip() for l in open(specs[0][1:]) if "=" in l]
	if idapro.open_database(db, run_auto_analysis=False):
		sys.exit("open_database failed")
	ida_auto.auto_wait()
	base = ida_nalt.get_imagebase()
	os.makedirs(outdir, exist_ok=True)
	for spec in specs:
		label, r = spec.split("=")
		try:
			text = str(ida_hexrays.decompile(base + int(r, 16)))
		except Exception as e:   # noqa: BLE001 - report and continue
			text = "// decompile failed: %s" % e
		open(os.path.join(outdir, label + ".c"), "w", encoding="utf-8").write(normalize(text) + "\n")
	idapro.close_database(False)
	print("wrote %d functions to %s" % (len(specs), outdir))


if __name__ == "__main__":
	main(*sys.argv[1:])
