#!/usr/bin/env python3
"""Одноразовая проверка: грузится ли гость вообще с -d trace (120 с)."""
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
    tmp = tempfile.mkdtemp(prefix="fobos-ahci4-")
    log = os.path.join(tmp, "serial.log")
    fwlog = os.path.join(tmp, "fw.log")
    trace = os.path.join(tmp, "trace.log")
    mon_path = os.path.join(tmp, "mon.sock")
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
        "-chardev", f"socket,id=mon0,path={mon_path},server=on,wait=off",
        "-mon", "chardev=mon0,mode=readline",
        "-serial", f"file:{log}",
        "-debugcon", f"file:{fwlog}",
        "-global", "isa-debugcon.iobase=0x402",
        "-no-reboot",
        "-d", "trace:ahci_*,trace:ide_*",
        "-D", trace,
    ]
    env = dict(os.environ)
    lib = "/home/z/tools/root/usr/lib/x86_64-linux-gnu"
    if os.path.isdir(lib):
        env["LD_LIBRARY_PATH"] = lib + ":" + env.get("LD_LIBRARY_PATH", "")

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, env=env)
    deadline = time.time() + 120
    while time.time() < deadline and proc.poll() is None:
        time.sleep(2)
        txt = ""
        try:
            txt = open(log, errors="replace").read()
        except OSError:
            pass
        if "kernel entry" in txt or "FOBOS" in txt:
            print(f"[t+{120-(deadline-time.time()):.0f}s] guest serial жив")
            break
    else:
        print("120 с: маркеров ядра нет")
    try:
        mon = Monitor(mon_path)
        mon.connect()
        mon.cmd("quit")
    except Exception:
        pass
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()

    txt = ""
    try:
        txt = open(log, errors="replace").read()
    except OSError:
        pass
    lines = txt.splitlines()
    print(f"serial: {len(lines)} строк")
    for line in lines[-25:]:
        print("  " + line)
    fw = ""
    try:
        fw = open(fwlog, errors="replace").read()
    except OSError:
        pass
    fw_lines = fw.splitlines()
    print(f"fw debugcon: {len(fw_lines)} строк; последние 5:")
    for line in fw_lines[-5:]:
        print("  " + line)
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
