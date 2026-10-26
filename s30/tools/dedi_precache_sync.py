"""Make the S3 dedi precache the weapons and impact tables the S30 client precaches.

S30's client scripts precache from S30-only feature scripts (e.g.
weapons/mp_ability_boost_kit_phase_rewind.nut, heirloom melee) whose server half
the dedi's S21-era scripts do not have; such a name is missing from the dedi's
WeaponNames / ImpactEffectTableNames string table and the client bridge has to
skip its precache. This reads the names client_s30 logged as

    client PrecacheWeapon '<name>' skipped: not in the server's WeaponNames
    client PrecacheImpactEffectTable '<name>' skipped: not in the server's ImpactEffectTableNames

and writes them, merged with what the block already lists, into a SERVER-only
block of the dedi's sh_weapons.gnut (after the weapon_cubemap precache).
Weapons without a weapon script on the dedi (platform/scripts/weapons/<name>.txt)
are left out. The first run keeps the original as sh_weapons.gnut.s21; line
endings are kept.

Both tables hold 256 names on the dedi; the client log says how many each has
(CreateStringTable '<table>' n/256).

usage: dedi_precache_sync.py <client_s30 log> [<more logs> ...] <dedi platform dir>
"""
import os
import re
import shutil
import sys

BEGIN = "\t// ---- S30 client precaches (s30/tools/dedi_precache_sync.py) ----"
END = "\t// ---- end S30 client precaches ----"
ANCHOR = "\t\tPrecacheWeapon( $\"weapon_cubemap\" )\n\t#endif\n"
SKIPPED_WEAPON = re.compile(r"client PrecacheWeapon '([A-Za-z0-9_]+)' skipped: not in the server's WeaponNames")
SKIPPED_IMPACT = re.compile(r"client PrecacheImpactEffectTable '([A-Za-z0-9_]+)' skipped: not in the server's ImpactEffectTableNames")
LISTED_WEAPON = re.compile(r'PrecacheWeapon\( \$"([A-Za-z0-9_]+)" \)')
LISTED_IMPACT = re.compile(r'PrecacheImpactEffectTable\( "([A-Za-z0-9_]+)" \)')
TABLE_SIZE = re.compile(r"CreateStringTable '(WeaponNames|ImpactEffectTableNames)' (\d+)/(\d+)")
CRLF = b"\x0d\x0a"
LF = "\x0a"


def main(args):
	if len(args) < 2:
		print(__doc__)
		return 2
	platform = args[-1]
	weapons, impacts, sizes = set(), set(), {}
	for log in args[:-1]:
		text = open(log, encoding="utf-8", errors="replace").read()
		weapons |= set(SKIPPED_WEAPON.findall(text))
		impacts |= set(SKIPPED_IMPACT.findall(text))
		for table, used, cap in TABLE_SIZE.findall(text):
			sizes[table] = (int(used), int(cap))
	path = os.path.join(platform, "scripts", "vscripts", "sh_weapons.gnut")
	raw = open(path, "rb").read()
	crlf = CRLF in raw
	text = raw.replace(CRLF, CRLF[1:]).decode("utf-8")
	listed = 0
	if BEGIN in text:
		block = text[text.index(BEGIN):text.index(END) + len(END) + 1]
		weapons |= set(LISTED_WEAPON.findall(block))
		impacts |= set(LISTED_IMPACT.findall(block))
		listed = len(LISTED_WEAPON.findall(block)) + len(LISTED_IMPACT.findall(block))
		text = text.replace(block, "")
	elif ANCHOR not in text:
		print("sh_weapons.gnut: weapon_cubemap precache not found")
		return 1
	missing = sorted(n for n in weapons if not os.path.isfile(os.path.join(platform, "scripts", "weapons", n + ".txt")))
	keep = sorted(n for n in weapons if n not in missing)
	for n in missing:
		print("no weapon script on the dedi, left out: %s" % n)
	lines = [BEGIN, "\t#if SERVER"]
	lines += ["\t\tPrecacheWeapon( $\"%s\" )" % n for n in keep]
	lines += ["\t\tPrecacheImpactEffectTable( \"%s\" )" % n for n in sorted(impacts)]
	lines += ["\t#endif", END]
	if not os.path.exists(path + ".s21"):
		shutil.copyfile(path, path + ".s21")
	text = text.replace(ANCHOR, ANCHOR + LF.join(lines) + LF, 1)
	out = text.encode("utf-8")
	if crlf:
		out = out.replace(CRLF[1:], CRLF)
	open(path, "wb").write(out)
	print("%d weapons: %s" % (len(keep), ", ".join(keep)))
	print("%d impact tables: %s" % (len(impacts), ", ".join(sorted(impacts))))
	# The logged sizes include the names a block of that run already precached (%d now listed).
	print("block had %d name(s) before this run" % listed)
	for table, (used, cap) in sorted(sizes.items()):
		print("%s: %d/%d on the dedi in the last log" % (table, used, cap))
	return 0


if __name__ == "__main__":
	sys.exit(main(sys.argv[1:]))
