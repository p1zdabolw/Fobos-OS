#!/usr/bin/env python3
"""Верификация FND-2f (баг-репорт пользователя №14):
  1. курсор: квадрат-артефакт на спавне (кадры spawn/moved);
  2. иконки: новый набор (кадр explorer/start);
  3. кнопка питания в Start — полный символ (кадр start);
  4. перезапуск консоли без раздвоения (кадр cmd-reopen);
  6. notepad save -> файл на ESP проверяется НА ХОСТЕ после выхода QEMU.
Скриншоты: download/f2f-*.png. Лог: /tmp/f2f/serial.log.
"""
import os
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
from qemu_selftest import (find_qemu, find_ovmf, ROM_DIR_CANDIDATES,
                           Monitor, wait_for, M, drive_oobe, do_login)
from screenshot import ppm_to_png

OUT = "/home/z/my-project/download"
TMP = "/tmp/f2f"
ESP = "/home/z/my-project/fobos-os/build/esp.img"
LOG = os.path.join(TMP, "serial.log")

RESET_ESC = "[loader] setup: reset requested (ESC)"


def boot_and_login(mon):
    wait_for(LOG, M["bootmgr"], 180)
    deadline = time.time() + 90
    while time.time() < deadline:
        mon.cmd("sendkey esc")
        if RESET_ESC in open(LOG, errors="replace").read():
            break
        time.sleep(0.3)
    wait_for(LOG, M["oobe_welcome"], 180)
    drive_oobe(mon, "admin", "1234")
    wait_for(LOG, M["login_wait"], 180)
    do_login(mon, "1234")
    wait_for(LOG, M["done"], 180)
    time.sleep(0.5)


def main():
    qemu = find_qemu()
    ovmf_code, ovmf_vars = find_ovmf()
    os.makedirs(TMP, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    # Чистый ESP и varstore: детерминизм.
    subprocess.run(["make", "build/esp.img"], cwd="/home/z/my-project/fobos-os",
                   check=True, capture_output=True)
    vars_fd = os.path.join(TMP, "vars.fd")
    shutil.copyfile(ovmf_vars, vars_fd)
    open(LOG, "wb").close()

    mon_path = os.path.join(TMP, "mon.sock")
    cmd = [qemu, "-machine", "q35", "-m", "512", "-smp", "1"]
    for d in ROM_DIR_CANDIDATES:
        if os.path.isdir(d):
            cmd += ["-L", d]
    cmd += [
        "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
        "-drive", f"if=pflash,format=raw,file={vars_fd}",
        "-drive", f"file={ESP},format=raw,if=ide",
        "-display", "none", "-net", "none",
        "-chardev", f"socket,id=mon0,path={mon_path},server=on,wait=off",
        "-mon", "chardev=mon0,mode=readline",
        "-serial", f"file:{LOG}",
        "-global", "isa-debugcon.iobase=0x402",
        "-no-reboot",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
    ]
    env = dict(os.environ)
    lib = "/home/z/tools/root/usr/lib/x86_64-linux-gnu"
    if os.path.isdir(lib):
        env["LD_LIBRARY_PATH"] = lib
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, env=env)
    mon = Monitor(mon_path)
    mon.connect()

    def shot(name):
        ppm = os.path.join(TMP, f"{name}.ppm")
        mon.cmd("screendump " + ppm)
        time.sleep(0.4)
        png = os.path.join(OUT, f"f2f-{name}.png")
        ppm_to_png(ppm, png)
        print(f"shot: {png}")

    def move(x0, y0, x1, y1, click=False):
        mon.cmd(f"mouse_move {x1 - x0} {y1 - y0}")
        time.sleep(0.35)
        if click:
            mon.cmd("mouse_button 1")
            time.sleep(0.25)
            mon.cmd("mouse_button 0")
            time.sleep(0.45)
        return x1, y1

    try:
        boot_and_login(mon)
        # TUI-десктоп: курсор в центре (640,400).
        mon.sendkey("fos == gui")
        wait_for(LOG, "[gfx/desktop] desktop: gui mode active", 60)
        time.sleep(1.2)
        pos = (640, 400)
        shot("gui-spawn")            # баг 1: кадр сразу после старта GUI

        pos = move(*pos, 900, 300)   # уводим курсор со спавна
        pos = move(*pos, 400, 500)
        pos = move(*pos, 900, 300)
        shot("gui-moved")            # баг 1: спавн-зона чистая?

        # баг 3: Start-меню с символом питания (правый нижний угол).
        mon.sendkey("start")
        wait_for(LOG, "[shell] start: toggled", 60)
        time.sleep(1.2)
        shot("start")
        mon.raw("sendkey esc")
        time.sleep(0.6)

        # баг 4: перезапуск консоли — закрыть крестом, открыть с панели.
        pos = move(*pos, 646, 59, click=True)     # X окна cmd (96,48,560x360)
        pos = move(*pos, 588, 782, click=True)    # пин cmd на панели
        time.sleep(1.0)
        mon.sendkey("echo console-restarted")
        wait_for(LOG, "[shell] echo: console-restarted", 60)
        time.sleep(0.6)
        shot("cmd-reopen")

        # баг 2/5: проводник (клик по пину на панели; shell-команды
        # explorer нет) с новым набором иконок, каталог \fobos.
        pos = move(*pos, 622, 782, click=True)    # пин explorer на панели
        time.sleep(1.0)
        shot("explorer")

        # баг 6: notepad -> текст -> Save -> маркер -> файл на ESP.
        mon.sendkey("notepad")
        wait_for(LOG, "[shell] notepad: opened from shell", 60)
        time.sleep(0.8)
        mon.sendkey("Hello Fobos")
        time.sleep(0.6)
        pos = move(*pos, 461, 205, click=True)    # кнопка Save
        wait_for(LOG, "notepad: saved", 60)
        time.sleep(0.8)
        shot("notepad-saved")
    finally:
        try:
            mon.cmd("quit")
        except Exception:
            pass
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()

    # --- Host-проверка: UNTITLED.TXT на ESP (\fobos) ---
    ok = False
    with open(ESP, "rb") as f:
        f.seek(2048 * 512)
        bpb = f.read(512)
        reserved = struct.unpack_from("<H", bpb, 14)[0]
        nfats = bpb[16]
        fatsz = struct.unpack_from("<I", bpb, 36)[0]
        data = reserved + nfats * fatsz

        def rd(lba, n=1):
            f.seek((2048 + lba) * 512)
            return f.read(n * 512)

        root = rd(data)
        fobos_clus = None
        for i in range(16):
            e = root[i * 32:(i + 1) * 32]
            if e[0:5] == b"FOBOS":
                fobos_clus = struct.unpack_from("<H", e, 20)[0] << 16 | \
                             struct.unpack_from("<H", e, 26)[0]
                break
        d = rd(data + fobos_clus - 2)
        for i in range(16):
            e = d[i * 32:(i + 1) * 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            name = (e[0:8].decode("ascii", "replace").rstrip() + "." +
                    e[8:11].decode("ascii", "replace").rstrip()).rstrip(".")
            if name == "UNTITLED.TXT":
                clus = struct.unpack_from("<H", e, 20)[0] << 16 | \
                       struct.unpack_from("<H", e, 26)[0]
                size = struct.unpack_from("<I", e, 28)[0]
                content = rd(data + clus - 2)[:size]
                print(f"HOST CHECK: UNTITLED.TXT clus={clus} size={size} "
                      f"content={content!r}")
                ok = (content.rstrip(b"\n\r") == b"Hello Fobos")
    print(f"HOST CHECK: {'PASS' if ok else 'FAIL'} — файл Notepad на ESP")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
