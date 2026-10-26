"""Make room in the dedi's pdef by dropping per-mode stats for modes nothing can reach.

stats.modes[eModesFlavors] costs 685 entries per mode, and the dedi's flattened pdef is capped at
26000 (SDK persistence_ext.cpp); S30's seasons and ranked periods do not fit next to all 15 S21
modes. A mode keeps its stats if a dedi playlist sets stats_match_type to it (the only way the
__Mode_* stat helpers write one) or a challenge's stat ref names it (stats.modes[<mode>].*).
Every other mode is removed from both places that must agree:

- eModesFlavors in the dedi's pdef_autogen.pdef (the modes[] array shrinks with it); S30's pdef
  only declares eModesFlavors, nothing there is indexed by it;
- STATS_ALTERNATE_MODE_REFS in the dedi's sh_stats_defs.gnut (the choices the modes stat enum
  struct registers); the entries are commented out, the original kept as .s21 (written once).

--drop mode,... removes modes the rule above would keep. arenas (only old S13-S15 challenges name
stats.modes[arenas]; no playlist runs it) is dropped on the user's call: such a challenge ref then
does not resolve, which sh_challenges.gnut's level init skips (Assert + continue).

    py dedi_mode_trim.py <pdef_autogen.pdef in/out> <dedi platform dir> <challenge itemflav export dir> [--drop mode,...]
"""
import os
import re
import shutil
import sys

MARK = "// S30 pdef trim: "


def kept_modes(platform, challenges):
	text = open(os.path.join(platform, "playlists_r5_patch.txt"), "rb").read().decode("utf-8", "replace")
	keep = {m.lower() for m in re.findall(r'stats_match_type\s+"?([A-Za-z0-9_]+)', text)}
	for top, _, files in os.walk(challenges):
		for f in files:
			if f.endswith(".json"):
				keep |= set(re.findall(r"stats\.modes\[([a-z0-9_]+)\]", open(os.path.join(top, f), encoding="utf-8", errors="replace").read()))
	return keep


def trim_pdef(path, keep):
	raw = open(path, "rb").read().decode("utf-8")
	nl = "\r\n" if "\r\n" in raw else "\n"
	lines = raw.split(nl)
	st = [i for i, l in enumerate(lines) if l.strip() == "$ENUM_START eModesFlavors"]
	if len(st) != 1:
		sys.exit("%s: eModesFlavors found %d times" % (path, len(st)))
	en = next(i for i in range(st[0], len(lines)) if lines[i].strip() == "$ENUM_END")
	dropped = [lines[i].strip() for i in range(st[0] + 1, en) if lines[i].strip() and lines[i].strip().lower() not in keep]
	lines[st[0] + 1:en] = [l for l in lines[st[0] + 1:en] if not l.strip() or l.strip().lower() in keep]
	open(path, "wb").write(nl.join(lines).encode("utf-8"))
	return dropped


def trim_refs(scripts, dropped):
	path = os.path.join(scripts, "sh_stats_defs.gnut")
	bak = path + ".s21"
	if not os.path.isfile(bak):
		shutil.copyfile(path, bak)
	lines = open(bak, "rb").read().decode("utf-8").split("\n")
	st = [i for i, l in enumerate(lines) if "global const array<string> STATS_ALTERNATE_MODE_REFS" in l]
	if len(st) != 1:
		sys.exit("sh_stats_defs.gnut: STATS_ALTERNATE_MODE_REFS found %d times" % len(st))
	i, n = st[0] + 1, 0
	while lines[i].strip() != "]":
		m = re.match(r'(\s*)"([^"]+)",\s*$', lines[i])
		if m and m.group(2) in dropped:
			lines[i] = m.group(1) + MARK + lines[i][len(m.group(1)):]
			n += 1
		i += 1
	if n != len(dropped):
		sys.exit("sh_stats_defs.gnut: %d of %d dropped modes found in STATS_ALTERNATE_MODE_REFS" % (n, len(dropped)))
	open(path, "wb").write("\n".join(lines).encode("utf-8"))


def main(pdef, platform, challenges, drop=()):
	keep = kept_modes(platform, challenges) - set(drop)
	dropped = trim_pdef(pdef, keep)
	trim_refs(os.path.join(platform, "scripts", "vscripts"), dropped)
	print("modes kept (playlists + challenge refs - --drop): %s" % ", ".join(sorted(keep)))
	print("modes dropped from eModesFlavors and STATS_ALTERNATE_MODE_REFS: %s" % ", ".join(dropped))
	return 0


if __name__ == "__main__":
	args = sys.argv[1:]
	drop = ()
	if "--drop" in args:
		i = args.index("--drop")
		drop = tuple(args[i + 1].split(","))
		del args[i:i + 2]
	sys.exit(main(*args[:3], drop=drop))
