"""Integer slots of script enum/const tables an Apex image builds natively.

Tables such as TRACE_COLLISION_GROUP_* are built slot by slot:
  lea rdx, [rip + name] ; mov rcx, vm ; call sq_pushstring
  mov edx, <value> | xor edx, edx | lea edx, [r8 + n] ; mov rcx, vm ; call sq_pushinteger
  ... ; call sq_newslot
This finds every `lea rdx` of a string with one of the given prefixes and reads the
integer pushed by the second call after it.

usage: py script_enum_slots.py <exe> <prefix> [prefix ...]
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
	prefixes = [p.encode() for p in sys.argv[2:]]
	strings = {}
	for p in prefixes:
		for m in re.finditer(re.escape(p) + rb'[A-Za-z0-9_]*\0', img):
			if img[m.start() - 1] == 0:
				strings[m.start()] = m.group()[:-1].decode()
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	out = {}
	for m in re.finditer(rb'\x48\x8d\x15', img[t0:t1]):
		a = t0 + m.start()
		s = a + 7 + int.from_bytes(img[a + 3:a + 7], 'little', signed=True)
		if s not in strings:
			continue
		calls = 0
		edx = None
		r8 = None
		for y in md.disasm(img[a + 7:a + 64], base + a + 7):
			op = y.op_str
			if y.mnemonic == 'call':
				calls += 1
				if calls == 2:
					break
				continue
			if calls != 1:
				continue
			if y.mnemonic == 'mov' and op.startswith('edx, '):
				try:
					edx = int(op.split(', ')[1], 16)
				except ValueError:
					edx = None
			elif y.mnemonic == 'xor' and op == 'edx, edx':
				edx = 0
			elif y.mnemonic == 'xor' and op == 'r8d, r8d':
				r8 = 0
			elif y.mnemonic == 'lea' and op.startswith('edx, [r8') and r8 is not None:
				k = op[len('edx, [r8'):-1].replace(' ', '')
				edx = r8 + int(k, 16) if k.startswith('+') else r8 - int(k[1:], 16)
		if calls == 2 and edx is not None:
			v = edx - (1 << 32) if edx & 0x80000000 else edx
			out.setdefault(strings[s], set()).add(v)
	for n in sorted(out):
		print("%s\t%s" % (n, ",".join(str(v) for v in sorted(out[n]))))


if __name__ == "__main__":
	main()
