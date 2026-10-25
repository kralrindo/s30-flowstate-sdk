"""Pattern census: run every byte signature in the SDK source against each game image.

Extracts each hex signature string literal (adjacent literals are concatenated, as the
compiler does), then counts its matches in every binary given. The S21 client and S3
dedi columns tell which product a pattern belongs to (a pattern counts as a client
pattern when it matches the S21 client images); the S30 column is what this port needs.

A unique hit on S30 is NOT proof the pattern found the same function: it is the
starting point for verification, nothing more.

usage: pattern_census.py <sdk_src_dir> <out.json> label=path [label=path ...]
"""
import json
import multiprocessing
import os
import re
import sys

LIT_SEQ = re.compile(r'"((?:[^"\\\n]|\\.)*)"(?:\s*"((?:[^"\\\n]|\\.)*)")*')
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
HEX_PAT = re.compile(r"^\s*(?:[0-9A-Fa-f]{2}|\?\??)(?:\s+(?:[0-9A-Fa-f]{2}|\?\??))+\s*$")
NAME_HINTS = [
	re.compile(r"NETOBS_\w+\(\s*(\w+)"),
	re.compile(r"\.GetPtr\(\s*(\w+)"),
	re.compile(r"\b(v_\w+)"),
	re.compile(r"\b(g_\w+)\s*="),
	re.compile(r"\b(p\w+)\s*=\s*Module_FindPattern"),
]
MAX_HITS = 4

_images = {}


def load_images(specs):
	for label, path in specs:
		with open(path, "rb") as f:
			_images[label] = f.read()


def to_regex(pat):
	parts = []
	for tok in pat.split():
		if tok.startswith("?"):
			parts.append(b".")
		else:
			parts.append(re.escape(bytes([int(tok, 16)])))
	return re.compile(b"".join(parts), re.DOTALL)


def count(job):
	idx, pat = job
	rx = to_regex(pat)
	out = {}
	for label, data in _images.items():
		hits = []
		for m in rx.finditer(data):
			hits.append(m.start())
			if len(hits) >= MAX_HITS:
				break
		out[label] = {"n": len(hits), "first": hits[0] if hits else None}
	return idx, out


def extract(src_dir):
	pats = []
	for root, dirs, files in os.walk(src_dir):
		dirs[:] = [d for d in dirs if d not in ("thirdparty", "protoc")]
		for fn in files:
			if not fn.endswith((".cpp", ".h", ".inl")):
				continue
			path = os.path.join(root, fn)
			rel = os.path.relpath(path, src_dir).replace("\\", "/")
			text = open(path, encoding="utf-8", errors="replace").read()
			for m in LIT_SEQ.finditer(text):
				s = "".join(LIT.findall(m.group(0)))
				if not HEX_PAT.match(s):
					continue
				toks = s.split()
				if len(toks) < 3 or all(t.startswith("?") for t in toks):
					continue
				line = text.count("\n", 0, m.start()) + 1
				ctx_start = text.rfind("\n", 0, max(0, m.start() - 200))
				ctx = text[ctx_start: m.end() + 300]
				name = None
				for rx in NAME_HINTS:
					hm = rx.search(ctx[ctx.find(m.group(0)[:20]) - 120 if ctx.find(m.group(0)[:20]) > 120 else 0:])
					if hm:
						name = hm.group(1)
						break
				pats.append({"file": rel, "line": line, "name": name, "pattern": " ".join(toks)})
	return pats


def main():
	src_dir, out_path = sys.argv[1], sys.argv[2]
	specs = [a.split("=", 1) for a in sys.argv[3:]]
	pats = extract(src_dir)
	print("extracted %d signature literals" % len(pats), flush=True)
	with multiprocessing.Pool(os.cpu_count(), initializer=load_images, initargs=(specs,)) as pool:
		for done, (idx, res) in enumerate(pool.imap_unordered(count, [(i, p["pattern"]) for i, p in enumerate(pats)], chunksize=8)):
			pats[idx]["hits"] = res
			if done % 200 == 0:
				print("  %d/%d" % (done, len(pats)), flush=True)
	json.dump({"images": dict(specs), "patterns": pats}, open(out_path, "w"), indent=1)
	print("wrote", out_path)


if __name__ == "__main__":
	main()
