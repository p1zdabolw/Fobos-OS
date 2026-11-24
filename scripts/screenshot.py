#!/usr/bin/env python3
"""
Fobos OS — визуальная проверка UI: скриншот экрана гостя через QEMU monitor.

Запускает гостя headless, ждёт готовности kshell, делает screendump в PPM,
конвертирует в PNG (Pillow или чистый PNG-энкодер). Позволяет убедиться,
что рабочий стол/консоль отрисованы, без открытого окна QEMU.
"""
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_selftest import find_qemu, find_ovmf, Monitor, ROM_DIR_CANDIDATES  # noqa: E402


def ppm_to_png(ppm_path: str, png_path: str) -> None:
    with open(ppm_path, "rb") as f:
        data = f.read()
    # P6 PPM: P6\n<комментарии?>\n<w> <h>\n<max>\n<rgb>
    m = re.match(rb"P6\n(?:#[^\n]*\n)*(\d+) (\d+)\n(\d+)\n", data)
    if m is None:
        raise RuntimeError("не P6 PPM")
    w, h = int(m.group(1)), int(m.group(2))
    offset = m.end()
    raw = data[offset:offset + w * h * 3]
    if len(raw) < w * h * 3:
        raise RuntimeError("PPM обрезан")

    def chunk(tag: bytes, payload: bytes) -> bytes:
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    scan = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
           chunk(b"IDAT", zlib.compress(scan, 6)) + chunk(b"IEND", b""))
    with open(png_path, "wb") as f:
        f.write(png)


def main() -> None:
    out_png = sys.argv[1] if len(sys.argv) > 1 else "build/screen.png"
    esp = sys.argv[2] if len(sys.argv) > 2 else "build/esp.img"
    serial = "build/serial_shot.log"

    qemu = find_qemu()
    ovmf_code, ovmf_vars = find_ovmf()
    tmp = tempfile.mkdtemp(prefix="fobos-shot-")
    vars_copy = os.path.join(tmp, "vars.fd")
    shutil.copyfile(ovmf_vars, vars_copy)
    monitor_path = os.path.join(tmp, "mon.sock")

    cmd = [qemu, "-machine", "q35", "-m", "512", "-smp", "1"]
    for rom_dir in ROM_DIR_CANDIDATES:
        if os.path.isdir(rom_dir):
            cmd += ["-L", rom_dir]
    cmd += [
        "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
        "-drive", f"if=pflash,format=raw,file={vars_copy}",
        "-drive", f"file={esp},format=raw,if=ide",
        "-display", "none", "-net", "none",
        "-chardev", f"socket,id=mon0,path={monitor_path},server=on,wait=off",
        "-mon", "chardev=mon0,mode=readline",
        "-serial", f"file:{serial}",
        "-debugcon", f"file:{serial}.fw.log",
        "-global", "isa-debugcon.iobase=0x402",
        "-no-reboot",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]

    env = dict(os.environ)
    tool_lib = "/home/z/tools/root/usr/lib/x86_64-linux-gnu"
    if os.path.isdir(tool_lib):
        env["LD_LIBRARY_PATH"] = tool_lib + ":" + env.get("LD_LIBRARY_PATH", "")

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=env)
    try:
        deadline = time.time() + 240
        ok = False
        while time.time() < deadline:
            try:
                with open(serial, "r", errors="replace") as f:
                    if "interactive shell ready" in f.read():
                        ok = True
                        break
            except OSError:
                pass
            time.sleep(0.5)
        if not ok:
            print("screenshot: kshell не поднялся за 240 с")
            sys.exit(1)

        time.sleep(2.0)  # дать главному циклу дорисовать статус-бар
        mon = Monitor(monitor_path)
        mon.connect()
        ppm = os.path.join(tmp, "shot.ppm")
        mon.cmd(f"screendump {ppm}")
        time.sleep(1.5)
        # Двинем мышь, чтобы курсор был виден в кадре не в стартовой точке
        mon.cmd("mouse_move 160 100")
        time.sleep(1.0)
        mon.cmd(f"screendump {ppm}.2")
        time.sleep(1.5)
        mon.cmd("quit")
        mon.close()
        proc.wait(timeout=30)

        ppm_to_png(ppm, out_png)
        ppm_to_png(ppm + ".2", out_png.replace(".png", ".mouse.png"))
        print(f"screenshot: {out_png} и {out_png.replace('.png', '.mouse.png')}")
    finally:
        try:
            mon = Monitor(monitor_path)
            mon.connect()
            mon.cmd("quit")
            mon.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
