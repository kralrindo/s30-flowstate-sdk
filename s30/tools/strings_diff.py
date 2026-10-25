"""Extract ASCII strings and MSVC RTTI type names from two images and diff selected families.

usage: strings_diff.py <a.exe> <b.exe> <outdir> [labelA labelB]
Writes <outdir>/<label>_strings.txt, <label>_rtti.txt and a diff summary per family.
"""
import os
import re
import sys

STR = re.compile(rb"[\x20-\x7e]{5,}")
RTTI = re.compile(rb"\.\?A[VU]([A-Za-z0-9_@?$]+)@@")


def scan(path):
	data = open(path, "rb").read()
	strings = set(m.group(0).decode() for m in STR.finditer(data))
	rtti = set(m.group(1).decode() for m in RTTI.finditer(data))
	return strings, rtti


FAMILIES = {
	"netmsg": re.compile(r"^(svc|net|clc|SVC|CLC|NET)_[A-Za-z0-9]+$"),
	"rtti_netmsg": re.compile(r"^(SVC|CLC|NET|svc|clc|net)_"),
	"convar_like": re.compile(r"^(sv|cl|net|host|mp|sdk|script|pak|r|mat|fs|rui|ui|bot)_[a-z0-9_]{2,}$"),
	"launch_param": re.compile(r"^[-+][a-z][a-z0-9_]{2,}$"),
	"script_native": re.compile(r"^[A-Z][A-Za-z0-9]+_[A-Z][A-Za-z0-9_]+$"),
}


def main():
	a, b, outdir = sys.argv[1:4]
	la, lb = (sys.argv[4], sys.argv[5]) if len(sys.argv) > 5 else ("a", "b")
	os.makedirs(outdir, exist_ok=True)
	sa, ra = scan(a)
	sb, rb_ = scan(b)
	for label, s, r in ((la, sa, ra), (lb, sb, rb_)):
		open(os.path.join(outdir, label + "_strings.txt"), "w").write("\n".join(sorted(s)))
		open(os.path.join(outdir, label + "_rtti.txt"), "w").write("\n".join(sorted(r)))
	summary = []
	for fam, rx in FAMILIES.items():
		src_a, src_b = (ra, rb_) if fam.startswith("rtti") else (sa, sb)
		fa = set(x for x in src_a if rx.match(x))
		fb = set(x for x in src_b if rx.match(x))
		summary.append("%-14s %s=%5d %s=%5d common=%5d only_%s=%5d only_%s=%5d" % (fam, la, len(fa), lb, len(fb), len(fa & fb), la, len(fa - fb), lb, len(fb - fa)))
		with open(os.path.join(outdir, "diff_%s.txt" % fam), "w") as f:
			f.write("## only in %s\n%s\n\n## only in %s\n%s\n" % (la, "\n".join(sorted(fa - fb)), lb, "\n".join(sorted(fb - fa))))
	summary.append("rtti total     %s=%d %s=%d common=%d" % (la, len(ra), lb, len(rb_), len(ra & rb_)))
	open(os.path.join(outdir, "summary.txt"), "w").write("\n".join(summary) + "\n")
	print("\n".join(summary))


if __name__ == "__main__":
	main()
