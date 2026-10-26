"""Registered net messages of an R5 client exe, without IDA (pefile + capstone).

CNetChan::RegisterMessage stores each message at netchan->m_NetMessages[msg->GetType()]; its
prologue checks the array count (`cmp dword ptr [rcx + <count off>], 0`). This finds that
function by the count offset, takes the function that calls it most often (ConnectionStart),
and lists, per call in order, the vtable loaded before it, the constant each tiny slot returns
(GetType, sizeof) and the slot RVAs.

usage: pe_netmsg.py <exe> <count offset hex> [slots]
"""
import collections
import sys

import capstone
import pefile


def main(path, count_off, nslots=12):
	pe = pefile.PE(path, fast_load=True)
	base = pe.OPTIONAL_HEADER.ImageBase
	img = pe.get_memory_mapped_image()
	text = next(s for s in pe.sections if s.Name.startswith(b".text"))
	tva, tend = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	q = lambda r: int.from_bytes(img[r:r + 8], "little") - base

	# RegisterMessage: function whose first ~8 insns contain cmp dword ptr [rcx + count_off], 0
	needle = b"\x83\xb9" + int(count_off, 16).to_bytes(4, "little") + b"\x00"
	regs = []
	at = img.find(needle, tva, tend)
	while at != -1:
		start = img.rfind(b"\xcc", tva, at) + 1   # functions are int3-padded
		if at - start < 0x20:
			regs.append(start)
		at = img.find(needle, at + 1, tend)
	print("RegisterMessage candidates:", ["0x%X" % r for r in regs])

	# call rel32 sites per caller function
	calls = collections.defaultdict(list)
	data = img[tva:tend]
	for reg in regs:
		i = data.find(b"\xe8")
		while i != -1:
			if tva + i + 5 + int.from_bytes(data[i + 1:i + 5], "little", signed=True) == reg:
				site = tva + i
				fstart = img.rfind(b"\xcc\xcc", tva, site) + 2
				calls[(reg, fstart)].append(site)
			i = data.find(b"\xe8", i + 1)
	(reg, func), sites = max(calls.items(), key=lambda kv: len(kv[1]))
	print("ConnectionStart 0x%X: %d calls to RegisterMessage 0x%X" % (func, len(sites), reg))

	def const_ret(r):
		ins = list(md.disasm(bytes(img[r:r + 16]), base + r, 2))
		if len(ins) == 2 and ins[1].mnemonic == "ret":
			if ins[0].mnemonic == "mov" and ins[0].op_str.startswith("eax, ") and "[" not in ins[0].op_str:
				try:
					return int(ins[0].op_str[5:], 0)
				except ValueError:   # register source
					return None
			if ins[0].mnemonic == "xor" and ins[0].op_str == "eax, eax":
				return 0
		return None

	last, n, end = None, 0, max(sites) + 5
	for ins in md.disasm(bytes(img[func:end]), base + func):
		r = ins.address - base
		if ins.mnemonic == "lea" and "[rip + " in ins.op_str:
			t = r + ins.size + int(ins.op_str.split("[rip + ")[1].rstrip("]"), 0)
			if tva <= q(t) < tend:
				last = t
		elif ins.mnemonic == "call" and r in sites:
			slots = [q(last + 8 * k) for k in range(int(nslots))] if last else []
			consts = {k: c for k, c in enumerate(const_ret(s) for s in slots) if c is not None}
			print("%3d at 0x%X vt %s  consts %s  slots %s" % (n, r, "0x%X" % last if last else "?",
				consts, " ".join("0x%X" % s for s in slots)))
			n += 1
			last = None


if __name__ == "__main__":
	main(*sys.argv[1:])
