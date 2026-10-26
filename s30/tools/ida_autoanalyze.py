"""Headless IDA (idalib) auto-analysis: open a binary, wait for analysis, save the .i64.

usage: ida_autoanalyze.py <binary>
"""
import sys
import time

import idapro  # must be first

path = sys.argv[1]
t = time.time()
if idapro.open_database(path, run_auto_analysis=True):
	sys.exit("open_database failed")
import ida_auto  # noqa: E402
import ida_funcs  # noqa: E402

ida_auto.auto_wait()
print("functions:", ida_funcs.get_func_qty(), "elapsed %.0fs" % (time.time() - t), flush=True)
idapro.close_database(save=True)
