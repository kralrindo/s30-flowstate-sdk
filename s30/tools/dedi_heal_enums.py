"""Renumber the dedi's heal-item enums to the S30 client's eHealthPickupType.

The S30 client reads consumable/heal-kit numbers as its eHealthPickupType
(sh_survival_loot.gnut): HEALTH_SMALL 0, HEALTH_LARGE 1, SHIELD_LARGE 2,
SHIELD_SMALL 3, COMBO_FULL 4, CLOCKUP_SYRINGE 5, MED_WHEEL_CONSUMABLE_END 6,
ULTIMATE 7. The S3 dedi sends its own numbers in the netvars
healingKitTypeCurrentlyBeingUsed / selectedHealthPickupType and in
ServerToClient_SetClientChargeTime: eConsumableType has ULTIMATE 5 and
eHealthPickupType swaps the two shields. Both dedi enums get the S30 layout;
the originals are kept as <file>.s21.

usage: py dedi_heal_enums.py <dedi platform dir>
"""
import os
import re
import shutil
import sys

S30_BODY = [
	"HEALTH_SMALL",
	"HEALTH_LARGE",
	"SHIELD_LARGE",
	"SHIELD_SMALL",
	"COMBO_FULL",
	"CLOCKUP_SYRINGE",
	"MED_WHEEL_CONSUMABLE_END",
	"ULTIMATE",
	"",
	"_count",
]

TARGETS = [
	("scripts/vscripts/weapons/mp_ability_consumable.nut", "eConsumableType", []),
	("scripts/vscripts/sh_survival_loot.gnut", "eHealthPickupType", ["", "INVALID = -1"]),
]


def patch(path, name, tail):
	src = path + ".s21"
	if not os.path.exists(src):
		shutil.copyfile(path, src)
	data = open(src, "rb").read().decode("utf-8")
	nl = "\r\n" if "\r\n" in data else "\n"
	m = re.search(r"global enum " + name + r"\s*\{[^}]*\}", data)
	if not m:
		sys.exit("%s: enum %s not found" % (path, name))
	body = S30_BODY + tail
	text = "global enum " + name + nl + "{" + nl
	text += "".join(("\t" + line if line else "") + nl for line in body)
	text += "}"
	out = data[:m.start()] + text + data[m.end():]
	open(path, "wb").write(out.encode("utf-8"))
	print("%s: %s -> S30 eHealthPickupType layout" % (os.path.relpath(path), name))


def main():
	if len(sys.argv) != 2:
		sys.exit(__doc__)
	for rel, name, tail in TARGETS:
		patch(os.path.join(sys.argv[1], rel), name, tail)


if __name__ == "__main__":
	main()
