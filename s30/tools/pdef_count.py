"""Count the flattened entries of a pdef (base file + its #included autogen), the number the
engine's "Persistent dataDef already at max N entries" check counts: one per leaf field per
array element, structs expanded. Also prints the biggest top-level contributors.

usage: pdef_count.py <persistent_player_data.pdef> [<pdef_autogen.pdef>]
       (the autogen path defaults to the one next to the base file)
"""
import os
import re
import sys


def read(path):
	return open(path, "rb").read().replace(b"\x00", b"").decode("utf-8", "replace").replace("\r\n", "\n").split("\n")


def code(line):
	return line.split("//", 1)[0].strip()


def load(base, autogen):
	lines = []
	for l in read(base):
		m = re.match(r'#include\s+"([^"]+)"', code(l))
		lines += read(autogen) if m else [l]
	enums, structs, top = {}, {}, []
	i = 0
	while i < len(lines):
		c = code(lines[i])
		m = re.match(r"\$(ENUM|STRUCT)_START\s+(\S+)", c)
		if m:
			kind, name, body = m.group(1), m.group(2), []
			i += 1
			while not re.match(r"\$%s_END" % kind, code(lines[i])):
				if code(lines[i]):
					body.append(code(lines[i]))
				i += 1
			if kind == "ENUM":
				enums[name] = len(body)
			else:
				structs[name] = body
		elif c and not c.startswith("$") and not c.startswith("#"):
			top.append(c)
		i += 1
	return enums, structs, top


def count_decl(decl, enums, structs, memo):
	typ, rest = decl.split(None, 1)
	n = 1
	for idx in re.findall(r"\[([^\]]+)\]", rest):
		idx = idx.strip()
		n *= int(idx) if idx.isdigit() else enums[idx]
	if typ in structs:
		if typ not in memo:
			memo[typ] = sum(count_decl(d, enums, structs, memo) for d in structs[typ])
		return n * memo[typ]
	return n


def main(base, autogen=None):
	autogen = autogen or os.path.join(os.path.dirname(base), "pdef_autogen.pdef")
	enums, structs, top = load(base, autogen)
	memo, per = {}, []
	for d in top:
		per.append((count_decl(d, enums, structs, memo), d))
	total = sum(n for n, _ in per)
	print("entries %d  (structs %d, enums %d, top-level fields %d)" % (total, len(structs), len(enums), len(top)))
	for n, d in sorted(per, reverse=True)[:8]:
		print("  %6d  %s" % (n, d))
	return 0


if __name__ == "__main__":
	sys.exit(main(*sys.argv[1:3]))
