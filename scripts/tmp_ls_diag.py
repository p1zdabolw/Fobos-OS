#!/usr/bin/env python3
"""Одноразовая диагностика: ls в kernel console (shift-путь)."""
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
    tmp = tempfile.mkdtemp(prefix="fobos-ls-")
    vfd = os.path.join(tmp, "v.fd")
    shutil.copyfile(vars_, vfd)
    g = Guest(qemu, "build/esp.img", vfd, os.path.join(tmp, "s.log"),
              tmp, "ls")
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
    # ждём shell в консольном режиме
    if not wait_marker(g.log_path, "shell: interactive shell ready", 240):
        print("shell не поднялся")
        return
    time.sleep(1.0)
    for cmd in ("ls /", "ls /fobos", "storage", "cat /fobos/NOTES.TXT"):
        mon.sendkey(cmd)
        wait_marker(g.log_path, f"[shell] exec: {cmd.split()[0]}", 60)
        time.sleep(2.0)
    time.sleep(2)
    txt = open(g.log_path, errors="replace").read()
    for line in txt.splitlines():
        if any(k in line for k in ("[shell]", "[fat32]", "[ahci]",
                                   "[fs]", "fat32/", "[ovl]")):
            print(line)
    try:
        mon.cmd("quit")
        mon.close()
    except Exception:
        pass
    g.proc.wait(timeout=10)
    shutil.rmtree(tmp, ignore_errors=True)


def wait_marker(path, marker, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if marker in open(path, errors="replace").read():
            return True
        time.sleep(0.5)
    return False


if __name__ == "__main__":
    main()
