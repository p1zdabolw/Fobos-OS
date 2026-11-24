#!/usr/bin/env python3
"""Одноразовая диагностика AHCI: трейс команд ИМЕННО ядра.

Ждём в serial маркер ahci_init ядра ("HBA: CAP="), даём ядру дойти до
FAT32-партиция/mbr-дампа, затем снимаем хвост трейса (последние команды).
"""
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
    tmp = tempfile.mkdtemp(prefix="fobos-ahci3-")
    log = os.path.join(tmp, "serial.log")
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
    try:
        # 1) ждём старт ahci_init ядра (до 300 с — TCG с трейсом медленный)
        deadline = time.time() + 300
        while time.time() < deadline:
            txt = ""
            try:
                txt = open(log, errors="replace").read()
            except OSError:
                pass
            if "HBA: CAP=" in txt:
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        # 2) даём ядру прогнать IDENTIFY + READ + падение FAT32-mount
        deadline = time.time() + 120
        while time.time() < deadline:
            txt = ""
            try:
                txt = open(log, errors="replace").read()
            except OSError:
                pass
            if ("FAT32-партиция" in txt or "fat_mount" in txt
                    or "storage недоступно" in txt or "FAT32: том" in txt):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        time.sleep(2)
    finally:
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

    print("==== serial ([fs]/[ahci] строки) ====")
    try:
        for line in open(log, errors="replace"):
            if "[fs]" in line or "[ahci]" in line or "[boot] storage" in line:
                print(line.rstrip())
    except OSError:
        pass

    out = ""
    try:
        out = open(trace, errors="replace").read()
    except OSError:
        pass
    open("/tmp/ahci_kernel_trace3.txt", "w").write(out)
    print(f"==== trace {len(out.splitlines())} lines ====")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
