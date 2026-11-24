#!/usr/bin/env python3
"""Одноразовая диагностика AHCI: трейс команд ИМЕННО ядра.

Путь: varstore чистый -> boot manager -> бурст Shift (как сценарий shift)
-> kernel console (storage_init выполняется при старте ядра, ввод не нужен).
Ждём [ahci]/rd lba=0 маркеры в serial, снимаем трейс.
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
    tmp = tempfile.mkdtemp(prefix="fobos-ahci5-")
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
        "-d", "trace:handle_cmd_*,trace:ahci_populate_sglist*,"
              "trace:ahci_start_dma,trace:ahci_dma_prepare_buf,"
              "trace:ahci_pio_transfer,trace:ahci_cmd_done,"
              "trace:ide_bus_exec_cmd,trace:ide_dma_cb",
        "-D", trace,
    ]
    env = dict(os.environ)
    lib = "/home/z/tools/root/usr/lib/x86_64-linux-gnu"
    if os.path.isdir(lib):
        env["LD_LIBRARY_PATH"] = lib + ":" + env.get("LD_LIBRARY_PATH", "")

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, env=env)
    mon = None
    try:
        mon = Monitor(mon_path)
        mon.connect()
        # Бурст Shift в окне boot manager -> FBPI_BOOTF_CONSOLE -> kernel console.
        deadline = time.time() + 60
        while time.time() < deadline:
            mon.raw("sendkey shift 250")
            txt = ""
            try:
                txt = open(log, errors="replace").read()
            except OSError:
                pass
            if "boot manager: shift detected" in txt or "kernel entry" in txt:
                break
            time.sleep(0.15)
        # Ждём диагностические строки ядра по AHCI (до 180 с).
        deadline = time.time() + 180
        while time.time() < deadline:
            txt = ""
            try:
                txt = open(log, errors="replace").read()
            except OSError:
                pass
            if ("rd lba=0" in txt and ("FAT32-партиция" in txt
                                       or "FAT32: том" in txt)):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        time.sleep(2)
    finally:
        try:
            if mon is not None:
                mon.cmd("quit")
                mon.close()
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
    print("==== serial ([fs]/[ahci]) ====")
    for line in txt.splitlines():
        if "[fs]" in line or "[ahci]" in line:
            print("  " + line)
    out = ""
    try:
        out = open(trace, errors="replace").read()
    except OSError:
        pass
    open("/tmp/ahci_kernel_trace5.txt", "w").write(out)
    print(f"==== trace {len(out.splitlines())} lines ====")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
