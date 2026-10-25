"""Print PE header facts and per-section entropy (packed/encrypted sections read ~8.0)."""
import collections
import math
import sys

import pefile


def entropy(data):
	if not data:
		return 0.0
	counts = collections.Counter(data)
	n = len(data)
	return -sum(c / n * math.log2(c / n) for c in counts.values())


for path in sys.argv[1:]:
	pe = pefile.PE(path, fast_load=True)
	fh = pe.FILE_HEADER
	oh = pe.OPTIONAL_HEADER
	print("== %s" % path)
	print("  TimeDateStamp=0x%08X SizeOfImage=0x%X EntryPoint=0x%X" % (fh.TimeDateStamp, oh.SizeOfImage, oh.AddressOfEntryPoint))
	for s in pe.sections:
		name = s.Name.rstrip(b"\x00").decode(errors="replace")
		ent = entropy(s.get_data()[: 4 << 20])
		print("  %-10s va=0x%08X vsz=0x%08X raw=0x%08X ent=%.2f" % (name, s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData, ent))
