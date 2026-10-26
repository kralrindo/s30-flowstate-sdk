"""Find net-message registration in an R5 client IDB and dump each registered message.

ConnectionStart-style functions build every net message: allocate, store the message vtable,
then register it on the netchannel. This finds the functions that store the most distinct
vtables into fresh objects, and for a chosen function lists each vtable in order with the
constant its GetType slot returns and the RVAs of its first slots.

usage: ida_netmsg.py <db.i64> scan [top]
       ida_netmsg.py <db.i64> dump <function rva> [slots]
"""
import sys

import idapro  # must be first

import ida_auto
import ida_bytes
import ida_funcs
import ida_nalt
import ida_ua
import idautils
import idc

BASE = None
MIN = 20


def rva(e):
	return "0x%X" % (e - BASE)


def vtable_stores(func):
	"""(insn ea, vtable ea) for each `lea reg, [rip+vt]` whose target is a pointer table into code
	followed (within 3 insns) by a store of that reg into [reg] (offset 0)."""
	out = []
	items = list(idautils.FuncItems(func.start_ea))
	for i, e in enumerate(items):
		if idc.print_insn_mnem(e) != "lea" or idc.get_operand_type(e, 1) != idc.o_mem:
			continue
		vt = idc.get_operand_value(e, 1)
		first = ida_bytes.get_qword(vt)
		f = ida_funcs.get_func(first)
		if not f or f.start_ea != first:
			continue
		reg = idc.print_operand(e, 0)
		for n in items[i + 1:i + 4]:
			if idc.print_insn_mnem(n) == "mov" and idc.get_operand_type(n, 0) == idc.o_phrase \
					and idc.print_operand(n, 1) == reg and idc.get_operand_value(n, 0) == 0:
				out.append((e, vt))
				break
	return out


def returns_const(f_ea):
	"""Constant returned by a tiny `mov eax, imm; ret` function, else None."""
	insns = []
	e = f_ea
	for _ in range(4):
		insn = ida_ua.insn_t()
		if not ida_ua.decode_insn(insn, e):
			return None
		insns.append((idc.print_insn_mnem(e), e))
		if insns[-1][0] == "retn":
			break
		e += insn.size
	if len(insns) == 2 and insns[0][0] == "mov" and idc.print_operand(insns[0][1], 0) == "eax" \
			and idc.get_operand_type(insns[0][1], 1) == idc.o_imm:
		return idc.get_operand_value(insns[0][1], 1)
	if len(insns) == 2 and insns[0][0] == "xor" and idc.print_operand(insns[0][1], 0) == "eax":
		return 0
	return None


def cmd_scan(top=15, minimum=20):
	global MIN
	MIN = minimum
	res = []
	for fea in idautils.Functions():
		f = ida_funcs.get_func(fea)
		if f.end_ea - f.start_ea < 0x400:
			continue
		vts = vtable_stores(f)
		n = len({vt for _, vt in vts})
		if n >= int(MIN):
			res.append((n, fea))
	for n, fea in sorted(res, reverse=True)[:int(top)]:
		print("%s  %d distinct vtables stored" % (rva(fea), n))


def registered_vtables(func, register_ea):
	"""(call ea, vtable ea) for each call to `register_ea` in `func`: the vtable is the last
	`lea reg, [vtable]` seen before the call (vtable = pointer table whose slot 0 starts a function)."""
	out, last = [], None
	for e in idautils.FuncItems(func.start_ea):
		m = idc.print_insn_mnem(e)
		if m == "lea" and idc.get_operand_type(e, 1) == idc.o_mem:
			t = idc.get_operand_value(e, 1)
			f = ida_funcs.get_func(ida_bytes.get_qword(t))
			if f and f.start_ea == ida_bytes.get_qword(t):
				last = t
		elif m == "call" and idc.get_operand_value(e, 0) == register_ea:
			out.append((e, last))
			last = None
	return out


def cmd_dump(func_rva, register_rva, slots=12):
	f = ida_funcs.get_func(BASE + int(func_rva, 16))
	seen = []
	for e, vt in registered_vtables(f, BASE + int(register_rva, 16)):
		if vt is None:
			print("  ? at %s: no vtable found before the call" % rva(e))
			continue
		seen.append(vt)
		ptrs = [ida_bytes.get_qword(vt + 8 * k) for k in range(int(slots))]
		consts = [returns_const(p) for p in ptrs]
		print("%3d at %s vt %s  consts %s  slots %s" % (len(seen) - 1, rva(e), rva(vt),
			{k: c for k, c in enumerate(consts) if c is not None}, " ".join(rva(p) for p in ptrs)))


def main():
	global BASE
	idapro.open_database(sys.argv[1], True)
	ida_auto.auto_wait()
	BASE = ida_nalt.get_imagebase()
	try:
		{"scan": cmd_scan, "dump": cmd_dump}[sys.argv[2]](*sys.argv[3:])
	finally:
		idapro.close_database(False)


if __name__ == "__main__":
	main()
