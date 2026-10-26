"""Every net message vtable in an R5 exe, found by the INetMessage base slots all of them share.

All CNetMessage vtables point slots 1, 2, 6, 8 and 9 at the same base methods (SetNetChannel,
SetReliable, IsReliable, GetGroup, GetNetChannel). This scans the read-only data for 8-aligned
pointer tables matching them and prints, per vtable, GetType (slot 7), sizeof (slot 11), and
the Process/ReadFromBuffer/WriteToBuffer slots (3/4/5). Pass the five base RVAs (hex).

usage: pe_netmsg_vtables.py <exe> <s1> <s2> <s6> <s8> <s9>
"""
import sys

import capstone
import pefile


def main(path, *base_slots):
	want = {1: int(base_slots[0], 16), 2: int(base_slots[1], 16), 6: int(base_slots[2], 16),
			8: int(base_slots[3], 16), 9: int(base_slots[4], 16)}
	pe = pefile.PE(path, fast_load=True)
	base = pe.OPTIONAL_HEADER.ImageBase
	img = pe.get_memory_mapped_image()
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	q = lambda r: int.from_bytes(img[r:r + 8], "little") - base

	def const_ret(r):
		ins = list(md.disasm(bytes(img[r:r + 16]), base + r, 2))
		if len(ins) == 2 and ins[1].mnemonic == "ret":
			if ins[0].mnemonic == "mov" and ins[0].op_str.startswith("eax, ") and "[" not in ins[0].op_str:
				try:
					return int(ins[0].op_str[5:], 0)
				except ValueError:
					return None
			if ins[0].mnemonic == "xor" and ins[0].op_str == "eax, eax":
				return 0
		return None

	rows = []
	for s in pe.sections:
		if not s.Name.startswith(b".rdata"):
			continue
		for vt in range(s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize - 0x60, 8):
			if all(q(vt + 8 * k) == v for k, v in want.items()):
				rows.append((const_ret(q(vt + 56)), const_ret(q(vt + 88)), vt,
							 q(vt + 24), q(vt + 32), q(vt + 40)))
	for t, size, vt, proc, rfb, wtb in sorted(rows, key=lambda r: (r[0] is None, r[0] or 0)):
		print("type %-4s sizeof %-7s vt 0x%X  process 0x%X rfb 0x%X wtb 0x%X" % (t, size, vt, proc, rfb, wtb))
	print(len(rows), "message vtables")


if __name__ == "__main__":
	main(*sys.argv[1:])
