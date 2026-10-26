"""Integer script constants an Apex image registers (name -> value).

The VM registers each engine constant as RegisterConstant(vm, name, value):
  mov r8d, <value> | xor r8d, r8d | lea r8d, [reg + n]
  lea rdx, [rip + name]
  mov rcx, <vm>
  call <RegisterConstant>
RegisterConstant is found as the call that follows `lea rdx, <anchor>` for an
anchor constant name given on the command line.

usage: py script_consts.py <exe> <anchor name> [prefix ...]
"""
import re
import sys
import pefile
import capstone


def main():
	if len(sys.argv) < 3:
		sys.exit(__doc__)
	pe = pefile.PE(sys.argv[1], fast_load=True)
	img = bytes(pe.get_memory_mapped_image())
	base = pe.OPTIONAL_HEADER.ImageBase
	text = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
	t0, t1 = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
	anchor = sys.argv[2].encode() + b'\0'
	sa = [m.start() for m in re.finditer(re.escape(anchor), img) if img[m.start() - 1] == 0]
	if len(sa) != 1:
		sys.exit("anchor string found %d times" % len(sa))
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	# the lea rdx referencing the anchor, then the first call after it
	target = None
	for m in re.finditer(rb'\x48\x8d\x15', img[t0:t1]):
		a = t0 + m.start()
		if a + 7 + int.from_bytes(img[a + 3:a + 7], 'little', signed=True) == sa[0]:
			for y in md.disasm(img[a:a + 32], base + a):
				if y.mnemonic == 'call':
					target = int(y.op_str, 16) - base
					break
	if target is None:
		sys.exit("no RegisterConstant call after the anchor")
	print("RegisterConstant 0x%X" % target, file=sys.stderr)
	# every call site of the target; decode the 3 preceding instructions per site
	prefixes = [p.encode() for p in sys.argv[3:]]
	out = {}
	for m in re.finditer(rb'\xe8', img[t0:t1]):
		a = t0 + m.start()
		if a + 5 + int.from_bytes(img[a + 1:a + 5], 'little', signed=True) != target:
			continue
		window = img[a - 24:a + 5]
		best = None
		for start in range(0, 24):
			ins = list(md.disasm(window[start:], base + a - 24 + start))
			if ins and ins[-1].address - base == a and sum(i.size for i in ins) == 29 - start:
				best = ins
				break
		if not best:
			continue
		r8 = None
		name = None
		for y in best:
			op = y.op_str
			if y.mnemonic == 'mov' and op.startswith('r8d, '):
				try:
					r8 = int(op.split(', ')[1], 16)
				except ValueError:
					r8 = None
			elif y.mnemonic == 'xor' and op == 'r8d, r8d':
				r8 = 0
			elif y.mnemonic == 'lea' and op.startswith('r8d, [') and 'rip' not in op:
				r8 = None
			elif y.mnemonic == 'lea' and op.startswith('rdx, [rip'):
				d = int(op.split(' ')[-1].rstrip(']'), 16)
				d = d if '+' in op else -d
				s = y.address - base + y.size + d
				e = img.find(b'\0', s)
				name = img[s:e]
		if name is None or r8 is None:
			continue
		if prefixes and not any(name.startswith(p) for p in prefixes):
			continue
		v = r8 - (1 << 32) if r8 & 0x80000000 else r8
		out.setdefault(name.decode('latin1'), set()).add(v)
	for n in sorted(out, key=lambda k: (k, min(out[k]))):
		print("%s\t%s" % (n, ",".join(str(v) for v in sorted(out[n]))))


if __name__ == "__main__":
	main()
