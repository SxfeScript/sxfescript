#!/usr/bin/env python3
"""Time one process launch in milliseconds.

Exists because run.sh is a /bin/sh script and dash -- /bin/sh on Debian and
Ubuntu -- has no `time` builtin, so the harness could not measure its own
startup rows over a non-interactive ssh session. Also gives sub-millisecond
resolution, which `time -p` does not.
"""
import subprocess, sys, time

if len(sys.argv) < 2:
    sys.exit("usage: timeone.py <command> [args...]")
start = time.perf_counter()
proc = subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL)
print("%.1f ms" % ((time.perf_counter() - start) * 1000))
sys.exit(proc.returncode)
