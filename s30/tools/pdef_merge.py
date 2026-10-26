"""Merge the S3 dedi's persistence definition into S30's (cfg/server/*.pdef).

The dedi sends its pdef to the client (svc_PersistenceDefFile), and the S30 client
expects S30's fields; the dedi's Flowstate/S21 scripts still use fields, enums and
struct members S30 dropped (isACheater, credits, ranked, club*, eAutogenSlot, ...).
Result: S30's two files with the dedi's extras added, never changing S30's order:

- enum/struct present in both: dedi-only members appended before $..._END;
- enum/struct/field only in the dedi: appended at the end of the same file
  (persistent_player_data.pdef or pdef_autogen.pdef), enums, then structs, then fields;
- same name, different declaration (e.g. xp_match[25] vs [32]): S30's kept, reported.

usage: pdef_merge.py <s30 dir> <dedi dir> <out dir>
"""
import os
import re
import sys

FILES = ("persistent_player_data.pdef", "pdef_autogen.pdef")
DECL = re.compile(r"^\s*(\S+)\s+([A-Za-z_][A-Za-z0-9_]*)")


def read(path):
	return open(path, "rb").read().replace(b"\x00", b"").decode("utf-8", "replace").replace("\r\n", "\n").split("\n")


def code(line):
	return line.split("//", 1)[0].strip()


def parse(lines):
	"""Top-level items in order: ('enum'|'struct', name, start, end) and ('field', name, idx)."""
	items, i = [], 0
	while i < len(lines):
		c = code(lines[i])
		m = re.match(r"\$(ENUM|STRUCT)_START\s+(\S+)", c)
		if m:
			kind, name = m.group(1).lower(), m.group(2)
			end = i + 1
			while end < len(lines) and not code(lines[end]).startswith("$%s_END" % m.group(1)):
				end += 1
			items.append((kind, name, i, end))
			i = end + 1
			continue
		if c and not c.startswith("$") and not c.startswith("#"):
			d = DECL.match(c)
			if d:
				items.append(("field", d.group(2), i))
		i += 1
	return items


def members(lines, start, end, kind):
	"""Member name -> declaration line inside an enum/struct block."""
	out = {}
	for line in lines[start + 1:end]:
		c = code(line)
		if not c:
			continue
		if kind == "enum":
			out[c.split()[0]] = line
		else:
			d = DECL.match(c)
			if d:
				out[d.group(2)] = line
	return out


def main(s30_dir, dedi_dir, out_dir):
	s30 = {f: read(os.path.join(s30_dir, f)) for f in FILES}
	dedi = {f: read(os.path.join(dedi_dir, f)) for f in FILES}
	s30_items = {f: parse(s30[f]) for f in FILES}
	# names S30 declares anywhere (both files), per kind
	s30_names = {}
	for f in FILES:
		for it in s30_items[f]:
			s30_names[(it[0], it[1])] = (f, it)

	report = []
	insert_before = {f: {} for f in FILES}   # line index of $END -> extra member lines
	append = {f: {"enum": [], "struct": [], "field": []} for f in FILES}
	for f in FILES:
		for it in parse(dedi[f]):
			kind, name = it[0], it[1]
			if (kind, name) in s30_names:
				sf, sit = s30_names[(kind, name)]
				if kind == "field":
					if code(dedi[f][it[2]]) != code(s30[sf][sit[2]]):
						report.append("field %s: kept S30 '%s' (dedi '%s')" % (name, code(s30[sf][sit[2]]), code(dedi[f][it[2]])))
					continue
				ours = members(s30[sf], sit[2], sit[3], kind)
				extra = [l for n, l in members(dedi[f], it[2], it[3], kind).items() if n not in ours]
				if extra:
					insert_before[sf].setdefault(sit[3], []).extend(extra)
					report.append("%s %s: %d dedi-only members appended" % (kind, name, len(extra)))
				continue
			if kind == "field":
				append[f]["field"].append(dedi[f][it[2]])
			else:
				append[f][kind].extend(dedi[f][it[2]:it[3] + 1])
			report.append("%s %s: dedi-only, appended to %s" % (kind, name, f))

	os.makedirs(out_dir, exist_ok=True)
	merged = {}
	for f in FILES:
		blocks = append[f]["enum"] + append[f]["struct"]
		block_names = set(re.findall(r"\$(?:ENUM|STRUCT)_START\s+(\S+)", "\n".join(blocks)))
		# The dedi-only enums/structs go (in the dedi's order) right before the first
		# line of this file that uses one of them, so no use precedes its definition.
		first_use = None
		if block_names:
			use = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, block_names)))
			for i, line in enumerate(s30[f]):
				texts = [line] + insert_before[f].get(i, [])
				if any(use.search(code(t)) for t in texts):
					first_use = i if i not in insert_before[f] else i
					break
			if first_use is not None:
				# go back to the start of the enclosing block, if any
				j = first_use
				while j >= 0 and not re.match(r"\s*\$(ENUM|STRUCT)_START", code(s30[f][j])):
					if re.match(r"\s*\$(ENUM|STRUCT)_END", code(s30[f][j])) and j != first_use:
						j = first_use
						break
					j -= 1
				first_use = j if j >= 0 else first_use
		out = []
		for i, line in enumerate(s30[f]):
			if first_use is not None and i == first_use:
				out.append("// ---- S3 dedi compat: enums/structs the dedi's scripts use that S30 dropped (pdef_merge.py) ----")
				out.extend(blocks)
				out.append("// ---- end S3 dedi compat ----")
			if i in insert_before[f]:
				out.append("    // S3 dedi compat (pdef_merge.py)")
				out.extend(insert_before[f][i])
			out.append(line)
		while out and not out[-1].strip():
			out.pop()
		extra = (blocks if first_use is None else []) + append[f]["field"]
		if extra:
			out += ["", "// ---- S3 dedi compat: declarations the dedi's scripts use that S30 dropped (pdef_merge.py) ----"]
			out += extra
		merged[f] = out
		open(os.path.join(out_dir, f), "w", encoding="utf-8", newline="\r\n").write("\n".join(out) + "\n")

	# Every enum/struct name used must be defined earlier (the base file #includes the
	# autogen file, so all of the autogen's definitions precede the base's uses).
	order = merged["pdef_autogen.pdef"] + merged["persistent_player_data.pdef"]
	defined, problems = set(), []
	all_defs = set(re.findall(r"\$(?:ENUM|STRUCT)_START\s+(\S+)", "\n".join(order)))
	for line in order:
		c = code(line)
		m = re.match(r"\$(?:ENUM|STRUCT)_START\s+(\S+)", c)
		if m:
			defined.add(m.group(1))
			continue
		for tok in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", c):
			if tok in all_defs and tok not in defined:
				problems.append("used before definition: %s in '%s'" % (tok, c))
	for p in problems[:20]:
		print(p)
	print("%d use-before-definition problems" % len(problems))
	if problems:
		return 1
	open(os.path.join(out_dir, "merge_report.txt"), "w").write("\n".join(report) + "\n")
	kinds = {}
	for r in report:
		k = r.split(" ")[0] + (" (kept S30)" if "kept S30" in r else " (members)" if "members" in r else " (dedi-only)")
		kinds[k] = kinds.get(k, 0) + 1
	print(kinds)
	print("wrote", out_dir)
	return 0


if __name__ == "__main__":
	sys.exit(main(*sys.argv[1:4]))
