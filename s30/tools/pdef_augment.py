"""Add S30's new legends, weapons, seasons and challenge slots to the S3 dedi's pdef_autogen.pdef.

The dedi keeps its own (S21-era) pdef -- its scripts are S21's, and S30's full pdef needs more
structs than the dedi's parser allows (40; s30/docs/10). What S30 adds per new item lives in
enums (no cap that matters: 192) and in members of structs the dedi already has, so:

- every S30 enum member the dedi's enum lacks is appended to it; S30-only enums are added
  (at the top of the file, before any use);
- every S30 member a shared struct lacks is appended to it, if its type and its array index
  (enum name or number) are already known to the dedi's pdef or added above;
- S30-only structs, and members that need one, are skipped (reported).

The dedi's persistent_player_data.pdef is not touched. Order of existing declarations is kept.

Every added member multiplies through the arrays it sits in; the dedi's parser caps the flattened
pdef at 26000 entries (SDK persistence_ext.cpp; pdef_count.py counts them). --no-grow lists enums
whose S30 members are left out because they only enlarge the dedi's S21-era stat arrays.

usage: pdef_augment.py <s30 pdef_autogen.pdef> <dedi pdef_autogen.pdef> <dedi persistent_player_data.pdef>
                       <out file> [--no-grow enumA,enumB,...]
"""
import re
import sys

PRIMITIVE = re.compile(r"^(int|bool|float|string\{\d+\})$")


def read(path):
	return open(path, "rb").read().replace(b"\x00", b"").decode("utf-8", "replace").replace("\r\n", "\n").split("\n")


def code(line):
	return line.split("//", 1)[0].strip()


def blocks(lines):
	"""(kind, name) -> (start, end, [(member name, line)]) for $ENUM/$STRUCT blocks."""
	out, i = {}, 0
	while i < len(lines):
		m = re.match(r"\$(ENUM|STRUCT)_START\s+(\S+)", code(lines[i]))
		if m:
			kind, name = m.group(1), m.group(2)
			j, mem = i + 1, []
			while not re.match(r"\$%s_END" % kind, code(lines[j])):
				c = code(lines[j])
				if c:
					parts = c.split()
					mem.append((parts[0] if kind == "ENUM" else parts[1].split("[")[0], lines[j]))
				j += 1
			out[(kind, name)] = (i, j, mem)
			i = j
		i += 1
	return out


def member_deps(line):
	"""Type name and array index tokens of a struct member declaration."""
	c = code(line)
	typ, rest = c.split(None, 1)
	idx = re.findall(r"\[([^\]]+)\]", rest)
	return typ, [x.strip() for x in idx]


def main(s30_path, dedi_path, base_path, out_path, no_grow=()):
	s30, dedi = read(s30_path), read(dedi_path)
	sb, db = blocks(s30), blocks(dedi)
	base_text = "\n".join(read(base_path))
	known_types = {n for (k, n) in db if k == "STRUCT"} | set(re.findall(r"\$STRUCT_START\s+(\S+)", base_text))
	known_enums = {n for (k, n) in db if k == "ENUM"} | set(re.findall(r"\$ENUM_START\s+(\S+)", base_text))

	report, new_enum_lines, insert_before = [], [], {}
	for (kind, name), (st, en, mem) in sb.items():
		if kind != "ENUM":
			continue
		if (kind, name) in db:
			if name in no_grow:
				report.append("enum %s: S30 members left out (--no-grow)" % name)
				continue
			have = {m for m, _ in db[(kind, name)][2]}
			extra = [l for m, l in mem if m not in have]
			if extra:
				insert_before.setdefault(db[(kind, name)][1], []).extend(extra)
				report.append("enum %s: +%d S30 members" % (name, len(extra)))
		else:
			new_enum_lines += s30[st:en + 1]
			known_enums.add(name)
			report.append("enum %s: S30-only, added (%d members)" % (name, len(mem)))

	skipped = []
	for (kind, name), (st, en, mem) in sb.items():
		if kind != "STRUCT":
			continue
		if (kind, name) not in db:
			skipped.append("struct %s (S30-only)" % name)
			continue
		have = {m for m, _ in db[(kind, name)][2]}
		extra = []
		for m, l in mem:
			if m in have:
				continue
			typ, idx = member_deps(l)
			type_ok = bool(PRIMITIVE.match(typ)) or typ in known_types or typ in known_enums
			idx_ok = all(x.isdigit() or x in known_enums for x in idx)
			if type_ok and idx_ok:
				extra.append(l)
			else:
				skipped.append("%s.%s (%s)" % (name, m, code(l)))
		if extra:
			insert_before.setdefault(db[(kind, name)][1], []).extend(extra)
			report.append("struct %s: +%d S30 members" % (name, len(extra)))

	out = []
	first_decl = next(i for i, l in enumerate(dedi) if re.match(r"\$(ENUM|STRUCT)_START", code(l)) or (code(l) and not code(l).startswith("$")))
	for i, line in enumerate(dedi):
		if i == first_decl and new_enum_lines:
			out.append("// ---- S30 enums the dedi's pdef lacked (pdef_augment.py) ----")
			out += new_enum_lines
			out.append("// ---- end S30 enums ----")
		if i in insert_before:
			out.append("    // S30 additions (pdef_augment.py)")
			out += insert_before[i]
		out.append(line)
	while out and not out[-1].strip():
		out.pop()
	open(out_path, "w", encoding="utf-8", newline="\r\n").write("\n".join(out) + "\n")

	# use-before-definition check over autogen (included first) + base
	order = out + read(base_path)
	all_defs = set(re.findall(r"\$(?:ENUM|STRUCT)_START\s+(\S+)", "\n".join(order)))
	defined, problems = set(), []
	for line in order:
		c = code(line)
		m = re.match(r"\$(?:ENUM|STRUCT)_START\s+(\S+)", c)
		if m:
			defined.add(m.group(1))
			continue
		for tok in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", c):
			if tok in all_defs and tok not in defined:
				problems.append("%s in '%s'" % (tok, c))
	structs = len(re.findall(r"\$STRUCT_START", "\n".join(order)))
	enums = len(re.findall(r"\$ENUM_START", "\n".join(order)))
	for r in report:
		print(r)
	print("skipped %d (S30-only structs or members needing them)" % len(skipped))
	open(out_path + ".skipped.txt", "w").write("\n".join(skipped) + "\n")
	print("structs %d enums %d, use-before-definition problems %d" % (structs, enums, len(problems)))
	for p in problems[:10]:
		print("  ", p)
	return 1 if problems or structs > 40 or enums > 192 else 0


if __name__ == "__main__":
	args = sys.argv[1:]
	no_grow = ()
	if "--no-grow" in args:
		i = args.index("--no-grow")
		no_grow = tuple(args[i + 1].split(","))
		del args[i:i + 2]
	sys.exit(main(*args[:4], no_grow=no_grow))
