"""Match S21 client net messages to S30 ones (milestone B1).

Input: the pe_netmsg.py dumps of both exes (registration order, GetType = slot 7, sizeof =
slot 11, ReadFromBuffer = slot 4, WriteToBuffer = slot 5). Messages are aligned by registration
order and object size (difflib), then each pair's ReadFromBuffer is compared as a mnemonic
sequence (operands and addresses dropped) so a body change shows up as a low similarity.

usage: netmsg_match.py <s21 dump> <s21 exe> <s30 dump> <s30 exe> [out.md]
"""
import difflib
import re
import sys

import capstone
import pefile

LINE = re.compile(r"^\s*(\d+) at 0x([0-9A-F]+) vt 0x([0-9A-F]+)\s+consts (\{.*?\})\s+slots (.*)$")


def load(dump):
	out = []
	for l in open(dump):
		m = LINE.match(l)
		if m:
			consts = eval(m.group(4))
			slots = [int(s, 16) for s in m.group(5).split()]
			out.append({"vt": int(m.group(3), 16), "type": consts.get(7), "size": consts.get(11),
						"rfb": slots[4], "wtb": slots[5], "process": slots[3]})
	return out


class Exe:
	def __init__(self, path):
		self.pe = pefile.PE(path, fast_load=True)
		self.base = self.pe.OPTIONAL_HEADER.ImageBase
		self.img = self.pe.get_memory_mapped_image()
		self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

	def shape(self, r, limit=400):
		"""Mnemonics + immediates (not addresses/displacements of rip) of the function at r."""
		seq = []
		for ins in self.md.disasm(bytes(self.img[r:r + 0x1000]), self.base + r):
			ops = ins.op_str
			if "rip" in ops or ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j"):
				ops = ""
			seq.append(ins.mnemonic + " " + ops)
			if ins.mnemonic in ("ret", "int3") or len(seq) >= limit:
				break
		return seq


def main(d21, e21, d30, e30, out=None):
	a, b = load(d21), load(d30)
	x21, x30 = Exe(e21), Exe(e30)
	sm = difflib.SequenceMatcher(None, [m["size"] for m in a], [m["size"] for m in b], autojunk=False)
	rows = []
	for tag, i1, i2, j1, j2 in sm.get_opcodes():
		if tag == "equal" or (tag == "replace" and i2 - i1 == j2 - j1):
			for i, j in zip(range(i1, i2), range(j1, j2)):
				rows.append((a[i], b[j]))
		else:
			for i in range(i1, i2):
				rows.append((a[i], None))
			for j in range(j1, j2):
				rows.append((None, b[j]))
	lines = ["| S21 type | S30 type | sizeof S21/S30 | RFB similarity | WTB similarity | note |",
			 "|---:|---:|---|---:|---:|---|"]
	for m21, m30 in sorted(rows, key=lambda r: (r[1] or r[0])["type"] if r[1] else 1000 + r[0]["type"]):
		if m21 and m30:
			rs = difflib.SequenceMatcher(None, x21.shape(m21["rfb"]), x30.shape(m30["rfb"])).ratio()
			ws = difflib.SequenceMatcher(None, x21.shape(m21["wtb"]), x30.shape(m30["wtb"])).ratio()
			note = "" if rs > 0.95 and m21["size"] == m30["size"] else "CHECK"
			lines.append("| %d | %d | %s/%s | %.2f | %.2f | %s |" % (m21["type"], m30["type"], m21["size"], m30["size"], rs, ws, note))
		elif m30:
			lines.append("| - | %d | -/%s | | | S30 only |" % (m30["type"], m30["size"]))
		else:
			lines.append("| %d | - | %s/- | | | S21 only |" % (m21["type"], m21["size"]))
	text = "\n".join(lines)
	print(text)
	if out:
		open(out, "w").write(text + "\n")


if __name__ == "__main__":
	main(*sys.argv[1:])
