"""Headless IDA (idalib) queries against an existing .i64.

usage: ida_query.py <db.i64> <command> [args...]  (several commands may be chained with ' ; ')
  strxref <text>        functions referencing a string that contains <text> (RVA + name)
  strexact <text>       strings exactly equal to <text>, with xrefs
  decomp <rva|name>     Hex-Rays pseudocode of the function containing <rva>
  disasm <rva> [n]      n instructions from <rva>
  callers <rva>         call sites of the function containing <rva>
  xrefs <rva> [n]       all code + data references to the function containing <rva>
  name <rva> <name>     rename the function at <rva> (saved)
RVAs are hex, relative to the image base.
"""
import sys

import idapro  # must be first

import ida_auto
import ida_bytes
import ida_funcs
import ida_hexrays
import ida_name
import ida_nalt
import idautils
import idc

BASE = None


def ea(v):
	v = v.strip()
	if v.lower().startswith("0x") or all(c in "0123456789abcdefABCDEF" for c in v):
		return BASE + int(v, 16)
	return ida_name.get_name_ea(idc.BADADDR, v)


def rva(e):
	return "0x%X" % (e - BASE)


def fname(e):
	f = ida_funcs.get_func(e)
	return "%s %s" % (rva(f.start_ea), ida_funcs.get_func_name(f.start_ea)) if f else "%s (no func)" % rva(e)


def cmd_strxref(text):
	hits = 0
	for s in idautils.Strings():
		val = str(s)
		if text not in val:
			continue
		hits += 1
		print("string %s %r" % (rva(s.ea), val[:120]))
		for x in idautils.XrefsTo(s.ea):
			print("   xref %s in %s" % (rva(x.frm), fname(x.frm)))
		if hits >= 25:
			print("   ... (truncated)")
			break


def cmd_strexact(text):
	"""Strings equal to <text> (short names like 'echo' that substring search drowns)."""
	for s in idautils.Strings():
		if str(s) == text:
			print("string %s %r" % (rva(s.ea), text))
			for x in idautils.XrefsTo(s.ea):
				print("   xref %s in %s" % (rva(x.frm), fname(x.frm)))


def cmd_decomp(target):
	e = ea(target)
	f = ida_funcs.get_func(e)
	if not f:
		print("no function at", target)
		return
	try:
		cf = ida_hexrays.decompile(f.start_ea)
		print("// %s" % fname(f.start_ea))
		print(str(cf))
	except Exception as exc:  # decompiler unavailable or failed
		print("decompile failed: %s" % exc)


def cmd_disasm(target, n="30"):
	e = ea(target)
	for _ in range(int(n)):
		print("%s  %s" % (rva(e), idc.generate_disasm_line(e, 0)))
		e = idc.next_head(e, idc.BADADDR)


def cmd_callers(target):
	f = ida_funcs.get_func(ea(target))
	for x in idautils.CodeRefsTo(f.start_ea, 0):
		print("   call %s in %s" % (rva(x), fname(x)))


def cmd_xrefs(target, limit="40"):
	"""All references (code and data) to the function containing <rva>."""
	f = ida_funcs.get_func(ea(target))
	start = f.start_ea if f else ea(target)
	refs = list(idautils.XrefsTo(start))
	print("   %d refs" % len(refs))
	for x in refs[: int(limit)]:
		print("   %s %s in %s" % ("code" if x.iscode else "data", rva(x.frm), fname(x.frm)))


def cmd_name(target, new):
	ida_name.set_name(ea(target), new, ida_name.SN_FORCE)
	print("named", target, new)


def main():
	global BASE
	db = sys.argv[1]
	if idapro.open_database(db, run_auto_analysis=False):
		sys.exit("open_database failed")
	ida_auto.auto_wait()
	BASE = ida_nalt.get_imagebase()
	ida_hexrays.init_hexrays_plugin()
	save = False
	for chunk in " ".join(sys.argv[2:]).split(" ; "):
		parts = chunk.split()
		if not parts:
			continue
		print("=== %s" % chunk)
		fn = globals().get("cmd_" + parts[0])
		if not fn:
			print("unknown command", parts[0])
			continue
		fn(*parts[1:]) if parts[0] not in ("strxref", "strexact") else fn(chunk.split(None, 1)[1])
		save = save or parts[0] == "name"
	idapro.close_database(save=save)


if __name__ == "__main__":
	main()
