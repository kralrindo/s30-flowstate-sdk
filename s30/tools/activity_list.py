"""Activity (ACT_*) registration list of an Apex image, in registration order.

The engine builds its activity table in ActivityList_RegisterSharedActivities:
one ActivityList_RegisterActivity(name, id[, flag]) call per activity, with the
id as an immediate (edx). Ids are positions in that table; the table's order is
what differs between seasons. This walks the registration function's calls and
returns [(id, name)].

usage: py activity_list.py <exe> <RegisterSharedActivities rva> <RegisterActivity rva[,rva...]> [out.tsv]
(the S3 dedi registers through two bodies: 0xB38DA0 (name, id, flag) and its no-flag twin 0xB38E60)
"""
import sys
import pefile
import capstone


def function_end(pe, rva):
	pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
	end = None
	for f in pe.DIRECTORY_ENTRY_EXCEPTION:
		s = f.struct
		if s.BeginAddress == rva:
			end = s.EndAddress
	# chunks that follow (the function may be split): keep extending while contiguous
	return end


def extract(path, reg_shared, reg_act):
	pe = pefile.PE(path, fast_load=True)
	img = pe.get_memory_mapped_image()
	base = pe.OPTIONAL_HEADER.ImageBase
	end = function_end(pe, reg_shared)
	if end is None:
		sys.exit("no pdata entry at 0x%X" % reg_shared)
	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	out = []
	edx = None
	rcx = None
	regs = {}  # 32-bit registers holding a known immediate (the dedi builds ids as lea edx, [r8 + n])
	pos = reg_shared
	# Linear sweep; the registration function is straight-line code. A tail jmp ends it.
	while True:
		insns = list(md.disasm(bytes(img[pos:pos + 0x10000]), base + pos))
		if not insns:
			break
		stop = False
		for y in insns:
			a = y.address - base
			op = y.op_str
			if y.mnemonic == 'mov' and op.startswith('edx, '):
				try:
					edx = int(op.split(', ')[1], 16)
				except ValueError:
					edx = None
			elif y.mnemonic == 'xor' and op == 'edx, edx':
				edx = 0
			elif y.mnemonic == 'lea' and op.startswith('edx, [') and 'rip' not in op:
				inner = op[len('edx, ['):-1].replace(' ', '')
				reg, _, k = inner.partition('+')
				reg32 = {'r8': 'r8d', 'r9': 'r9d', 'rax': 'eax', 'rbx': 'ebx', 'rsi': 'esi', 'rdi': 'edi'}.get(reg, reg)
				edx = regs[reg32] + int(k or '0', 16) if reg32 in regs else None
			elif y.mnemonic == 'xor' and op.split(', ')[0] == op.split(', ')[-1]:
				regs[op.split(', ')[0]] = 0
			elif y.mnemonic == 'mov' and op.split(', ')[0] in ('r8d', 'r9d', 'eax', 'ebx', 'esi', 'edi'):
				try:
					regs[op.split(', ')[0]] = int(op.split(', ')[1], 16)
				except ValueError:
					regs.pop(op.split(', ')[0], None)
			elif y.mnemonic == 'lea' and op.startswith('rcx, [rip'):
				disp = int(op.split(' ')[-1].rstrip(']'), 16) if '+' in op else -int(op.split(' ')[-1].rstrip(']'), 16)
				rcx = a + y.size + disp
			elif y.mnemonic in ('call', 'jmp') and op.startswith('0x'):
				t = int(op, 16) - base
				if t in reg_act:
					if rcx is None or edx is None:
						sys.exit("call at 0x%X without an immediate name/id" % a)
					e = img.index(b'\0', rcx)
					out.append((edx, bytes(img[rcx:e]).decode('latin1')))
					rcx = None
					edx = None
				if y.mnemonic == 'jmp':
					stop = True
					break
			elif y.mnemonic == 'ret':
				stop = True
				break
			pos = a + y.size
		if stop or pos >= end + 0x10000:
			break
	return out


def main():
	if len(sys.argv) < 4:
		sys.exit(__doc__)
	rows = extract(sys.argv[1], int(sys.argv[2], 16), {int(x, 16) for x in sys.argv[3].split(',')})
	text = "".join("%d\t%s\n" % r for r in rows)
	if len(sys.argv) > 4:
		open(sys.argv[4], "w").write(text)
	else:
		sys.stdout.write(text)
	print("%d activities, ids %d..%d" % (len(rows), min(r[0] for r in rows), max(r[0] for r in rows)), file=sys.stderr)


if __name__ == "__main__":
	main()
