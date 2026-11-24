#!/usr/bin/env python3
"""Одноразовая диагностика fat32/write: shift-путь до storage-самотеста."""
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
os.environ.setdefault(
    "LD_LIBRARY_PATH", "/home/z/tools/root/usr/lib/x86_64-linux-gnu")
from qemu_selftest import Guest, Monitor, find_qemu, find_ovmf  # noqa: E402


def main():
    qemu = find_qemu()
    code, vars_ = find_ovmf()
    tmp = tempfile.mkdtemp(prefix="fobos-wr-")
    vfd = os.path.join(tmp, "v.fd")
    shutil.copyfile(vars_, vfd)
    g = Guest(qemu, "build/esp.img", vfd, os.path.join(tmp, "s.log"),
              tmp, "wr")
    Guest.ovmf_code = code
    g.start()
    mon = g.monitor()
    deadline = time.time() + 30
    while time.time() < deadline:
        mon.raw("sendkey shift 250")
        txt = open(g.log_path, errors="replace").read()
        if "kernel entry" in txt:
            break
        time.sleep(0.15)
    deadline = time.time() + 240
    while time.time() < deadline:
        txt = open(g.log_path, errors="replace").read()
        if "kselftest(storage): summary" in txt:
            break
        if g.proc.poll() is not None:
            break
        time.sleep(1)
    txt = open(g.log_path, errors="replace").read()
    for line in txt.splitlines():
        if any(k in line for k in ("[fs]", "[fat32]", "fat32/", "storage",
                                   "[ovl]")):
            print(line)
    try:
        mon.cmd("quit")
        mon.close()
    except Exception:
        pass
    g.proc.wait(timeout=10)
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
