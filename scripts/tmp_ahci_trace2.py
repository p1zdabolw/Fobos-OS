#!/usr/bin/env python3
"""Одноразовая отладка: ручной QEMU с -d trace:ahci_*, ловим обработку слотов."""
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from qemu_selftest import Monitor, find_qemu, find_ovmf, ROM_DIR_CANDIDATES  # noqa: E402


def main():
    qemu = find_qemu()
    ovmf_code, ovmf_vars = find_ovmf()
    tmp = tempfile.mkdtemp(prefix="fobos-tr3-")
    vars_fd = os.path.join(tmp, "vars.fd")
    shutil.copyfile(ovmf_vars, vars_fd)

    cmd = [qemu, "-machine", "q35", "-m", "512", "-smp", "1"]
    for rom_dir in ROM_DIR_CANDIDATES:
        if os.path.isdir(rom_dir):
            cmd += ["-L", rom_dir]
    cmd += [
        "-drive", "if=pflash,format=raw,readonly=on,file=" + ovmf_code,
        "-drive", "if=pflash,format=raw,file=" + vars_fd,
        "-drive", "file=build/esp.img,format=raw,if=ide",
        "-display", "none", "-net", "none",
        "-chardev", f"socket,id=mon0,path={tmp}/mon.sock,server=on,wait=off",
        "-mon", "chardev=mon0,mode=readline",
        "-serial", f"file:{tmp}/serial.log",
        "-no-reboot",
        "-d", "trace:ahci_*",
    ]
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = ("/home/z/tools/root/usr/lib/x86_64-linux-gnu:"
                              + env.get("LD_LIBRARY_PATH", ""))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=env)

    time.sleep(3)
    mon = None
    try:
        mon = Monitor(f"{tmp}/mon.sock")
        mon.connect()
    except Exception as e:
        print("monitor fail:", e)

    end = time.time() + 75
    while time.time() < end:
        try:
            txt = open(f"{tmp}/serial.log", errors="replace").read()
        except OSError:
            txt = ""
        if "storage недоступно" in txt or "FAT32:" in txt or "FAT32-партиция" in txt:
            break
        if mon is not None:
            try:
                mon.raw("sendkey shift 250")
            except Exception:
                pass
        time.sleep(0.2)

    time.sleep(2)
    if mon is not None:
        try:
            mon.cmd("quit")
        except Exception:
            pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    out = proc.stderr.read().decode(errors="replace")
    open("/tmp/ahci_trace.txt", "w").write(out)
    lines = out.splitlines()
    print(f"trace lines: {len(lines)} (saved /tmp/ahci_trace.txt)")
    print("=== serial ===")
    try:
        for line in open(f"{tmp}/serial.log", errors="replace"):
            if "[fs]" in line or "[ahci]" in line:
                print(line.rstrip())
    except OSError:
        pass


if __name__ == "__main__":
    main()
