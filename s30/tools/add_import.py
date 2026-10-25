"""Add a DLL import to a PE copy the way the S21 Flowstate exes carry loader.dll.

Appends a '.l2' section (0xC0000060, like S21's) holding a rebuilt import descriptor array:
every original descriptor, then <dll>!<func>, then the terminator. Repoints the import data
directory at it and clears the certificate and bound-import directories. The original
IAT/ILT stay where they are.

usage: add_import.py <in.exe> <out.exe> [dll=loader.dll] [func=DummyExport]
Refuses to write over its input.
"""
import os
import struct
import sys

import pefile

DESC_SIZE = 20
SECTION_CHARS = 0xC0000060


def align(v, a):
	return (v + a - 1) // a * a


def main():
	src, dst = sys.argv[1], sys.argv[2]
	dll = (sys.argv[3] if len(sys.argv) > 3 else "loader.dll").encode()
	func = (sys.argv[4] if len(sys.argv) > 4 else "DummyExport").encode()
	if os.path.abspath(src).lower() == os.path.abspath(dst).lower():
		sys.exit("refusing to patch in place")

	pe = pefile.PE(src)
	oh = pe.OPTIONAL_HEADER
	imp = oh.DATA_DIRECTORY[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]]
	names = [e.dll.lower() for e in pe.DIRECTORY_ENTRY_IMPORT]
	if dll.lower() in names:
		sys.exit("%s already imported" % dll.decode())

	n_desc = len(pe.DIRECTORY_ENTRY_IMPORT)
	old_desc = pe.get_data(imp.VirtualAddress, n_desc * DESC_SIZE)

	# section header slot
	last = pe.sections[-1]
	hdr_off = last.get_file_offset() + 40
	first_raw = min(s.PointerToRawData for s in pe.sections if s.PointerToRawData)
	if hdr_off + 40 > first_raw:
		sys.exit("no room for another section header")
	# The header slot may sit past SizeOfHeaders but before the first section's data
	# (S30: headers end 0x3F8, SizeOfHeaders 0x400, data at 0x600); grow it to cover the slot.
	new_size_of_headers = max(oh.SizeOfHeaders, align(hdr_off + 40, oh.FileAlignment))

	data = bytearray(pe.__data__)
	sect_va = align(last.VirtualAddress + max(last.Misc_VirtualSize, last.SizeOfRawData), oh.SectionAlignment)
	raw_ptr = align(len(data), oh.FileAlignment)

	# layout inside the new section
	desc_off = 0
	desc_len = (n_desc + 2) * DESC_SIZE
	ilt_off = align(desc_off + desc_len, 8)
	iat_off = ilt_off + 16
	hint_off = iat_off + 16
	hint = struct.pack("<H", 0) + func + b"\x00"
	name_off = align(hint_off + len(hint), 2)
	body_len = name_off + len(dll) + 1

	body = bytearray(align(body_len, oh.FileAlignment))
	body[desc_off:desc_off + len(old_desc)] = old_desc
	new_desc = struct.pack("<IIIII", sect_va + ilt_off, 0, 0, sect_va + name_off, sect_va + iat_off)
	body[desc_off + n_desc * DESC_SIZE: desc_off + (n_desc + 1) * DESC_SIZE] = new_desc
	struct.pack_into("<Q", body, ilt_off, sect_va + hint_off)
	struct.pack_into("<Q", body, iat_off, sect_va + hint_off)
	body[hint_off:hint_off + len(hint)] = hint
	body[name_off:name_off + len(dll)] = dll

	data += b"\x00" * (raw_ptr - len(data))
	data += body

	sh = struct.pack("<8sIIIIIIHHI", b".l2", body_len, sect_va, len(body), raw_ptr, 0, 0, 0, 0, SECTION_CHARS)
	data[hdr_off:hdr_off + 40] = sh

	fh_off = pe.FILE_HEADER.get_file_offset()
	struct.pack_into("<H", data, fh_off + 2, pe.FILE_HEADER.NumberOfSections + 1)
	oh_off = oh.get_file_offset()
	struct.pack_into("<I", data, oh_off + 56, align(sect_va + body_len, oh.SectionAlignment))  # SizeOfImage
	struct.pack_into("<I", data, oh_off + 60, new_size_of_headers)  # SizeOfHeaders
	struct.pack_into("<I", data, oh_off + 64, 0)  # CheckSum
	dd_off = oh_off + 112  # PE32+ data directories
	struct.pack_into("<II", data, dd_off + 1 * 8, sect_va + desc_off, (n_desc + 1) * DESC_SIZE)  # import
	struct.pack_into("<II", data, dd_off + 4 * 8, 0, 0)  # certificate
	struct.pack_into("<II", data, dd_off + 11 * 8, 0, 0)  # bound imports

	with open(dst, "wb") as f:
		f.write(data)

	check = pefile.PE(dst)
	got = [e.dll.decode() for e in check.DIRECTORY_ENTRY_IMPORT]
	assert got[-1].lower() == dll.decode().lower() and len(got) == n_desc + 1, got
	print("wrote %s: %d imports, last %s!%s, .l2 at va 0x%X" % (dst, len(got), got[-1], func.decode(), sect_va))


if __name__ == "__main__":
	main()
