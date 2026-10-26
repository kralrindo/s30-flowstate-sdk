"""Find an IDA-style byte pattern ("48 8B ?? ...") in a PE's .text and print each match RVA.

usage: pe_pattern.py <exe> "<pattern>"
"""
import re
import sys

import pefile


def main(path, pattern):
	pe = pefile.PE(path, fast_load=True)
	img = pe.get_memory_mapped_image()
	text = next(s for s in pe.sections if s.Name.startswith(b".text"))
	rx = re.compile(b"".join(b"." if t in ("?", "??") else re.escape(bytes([int(t, 16)]))
							  for t in pattern.split()), re.S)
	start, end = text.VirtualAddress, text.VirtualAddress + text.Misc_VirtualSize
	hits = [m.start() + start for m in rx.finditer(img[start:end])]
	for h in hits[:20]:
		print("0x%X" % h)
	print(len(hits), "matches")


if __name__ == "__main__":
	main(*sys.argv[1:])
