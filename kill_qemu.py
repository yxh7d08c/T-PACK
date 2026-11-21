#!/usr/bin/env python3
import os
from pathlib import Path
import signal
import time
target = str(Path(__file__).resolve().parent/"qemu-tdp/build/qemu-system-x86_64")
def matching():
    result = []
    for p in Path("/proc").iterdir():
        if not p.name.isdigit(): continue
        try:
            argv = (p/"cmdline").read_bytes().split(b"\0")
            if argv and argv[0].decode(errors="replace")==target: result.append(int(p.name))
        except (OSError,ValueError): pass
    return result
pids=matching()
for pid in pids:
    try: os.kill(pid,signal.SIGTERM)
    except ProcessLookupError: pass
time.sleep(2)
for pid in matching():
    if pid in pids:
        try: os.kill(pid,signal.SIGKILL)
        except ProcessLookupError: pass
print("Stopped repository QEMU processes:",pids)
