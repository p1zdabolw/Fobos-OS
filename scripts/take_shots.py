#!/usr/bin/env python3
"""Скриншоты FND-2e: splash, OOBE (вкл. save), login, TUI-десктоп,
GUI-десктоп (Start-меню + окна notepad/explorer поверх), kernel console."""
import os, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(__file__))
from qemu_selftest import (find_qemu, find_ovmf, ROM_DIR_CANDIDATES,
                           Monitor, wait_for, M, drive_oobe, do_login)
from screenshot import ppm_to_png

OUT = "/home/z/my-project/download"
TMP = "/tmp/fobos-shots"

RESET_ESC = "[loader] setup: reset requested (ESC)"


def reset_setup(mon, log):
    """ESC-сброс настроек в окне boot manager: на ESP может остаться
    setup.cfg от прошлых прогонов (CI/скриншоты) — без сброса OOBE не
    покажется, а пароль будет чужим. Детерминизм = сброс + свой OOBE."""
    deadline = time.time() + 90
    while time.time() < deadline:
        mon.cmd("sendkey esc")
        if RESET_ESC in open(log, errors="replace").read():
            return True
        time.sleep(0.3)
    return False


def boot_with_shot(shot_name, phase, vars_src=None):
    """Запуск QEMU до нужной фазы + screendump -> OUT/fnd2b-<shot>.png."""
    qemu = find_qemu()
    ovmf_code, ovmf_vars = find_ovmf()
    os.makedirs(TMP, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    vars_fd = os.path.join(TMP, f"vars-{shot_name}.fd")
    if vars_src and os.path.isfile(vars_src):
        shutil.copyfile(vars_src, vars_fd)
    else:
        shutil.copyfile(ovmf_vars, vars_fd)
    log = os.path.join(TMP, f"serial-{shot_name}.log")
    mon_path = os.path.join(TMP, f"mon-{shot_name}.sock")

    cmd = [qemu, "-machine", "q35", "-m", "512", "-smp", "1"]
    for d in ROM_DIR_CANDIDATES:
        if os.path.isdir(d):
            cmd += ["-L", d]
    cmd += [
        "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf_code}",
        "-drive", f"if=pflash,format=raw,file={vars_fd}",
        "-drive", "file=build/esp.img,format=raw,if=ide",
        "-display", "none", "-net", "none",
        "-chardev", f"socket,id=mon0,path={mon_path},server=on,wait=off",
        "-mon", "chardev=mon0,mode=readline",
        "-serial", f"file:{log}",
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
    ppm = os.path.join(TMP, f"{shot_name}.ppm")
    try:
        if phase == "splash":
            # Сброс + OOBE, потом ловим splash ядра по маркеру (selftest'ы
            # перед splash идут долго — sleep от entry промахивался мимо
            # 3-секундного окна и снимал ещё экран загрузчика).
            wait_for(log, M["bootmgr"], 180)
            reset_setup(mon, log)
            wait_for(log, M["oobe_welcome"], 180)
            drive_oobe(mon, "admin", "1234")
            wait_for(log, M["splash"], 240)
            time.sleep(0.6)  # окно splash 3 c — кадр внутри
            mon.cmd("screendump " + ppm)
        elif phase == "oobe":
            wait_for(log, M["bootmgr"], 180)
            reset_setup(mon, log)
            wait_for(log, M["oobe_welcome"], 180)
            time.sleep(0.5)
            mon.sendkey("")      # welcome -> hostname
            time.sleep(0.5)
            mon.cmd("screendump " + ppm)
        elif phase == "save":
            # Экран OOBE "Saving setup...": длинный подзаголовок обязан
            # переноситься внутри бокса (баг FND-2c: текст выходил за рамку).
            wait_for(log, M["bootmgr"], 180)
            reset_setup(mon, log)
            wait_for(log, M["oobe_welcome"], 180)
            drive_oobe(mon, "admin", "1234")
            time.sleep(1.0)
            mon.cmd("screendump " + ppm)
        elif phase == "login":
            wait_for(log, M["bootmgr"], 180)
            reset_setup(mon, log)
            wait_for(log, M["oobe_welcome"], 180)
            drive_oobe(mon, "admin", "1234")
            wait_for(log, M["login_wait"], 180)
            time.sleep(0.5)
            mon.cmd("screendump " + ppm)
        elif phase == "desktop":
            wait_for(log, M["bootmgr"], 180)
            reset_setup(mon, log)
            wait_for(log, M["oobe_welcome"], 180)
            drive_oobe(mon, "admin", "1234")
            wait_for(log, M["login_wait"], 180)
            do_login(mon, "1234")
            wait_for(log, M["done"], 180)
            time.sleep(0.5)
            mon.sendkey("help")
            time.sleep(0.5)
            mon.cmd("screendump " + ppm)
        elif phase == "gui":
            # FND-2e: логин -> fos == gui -> help в cmd -> клик по иконке
            # explorer -> notepad командой (окно поверх) -> Start-меню.
            # На кадре: обои bloom, иконки, окна, панель с иконками, Start.
            wait_for(log, M["bootmgr"], 180)
            reset_setup(mon, log)
            wait_for(log, M["oobe_welcome"], 180)
            drive_oobe(mon, "admin", "1234")
            wait_for(log, M["login_wait"], 180)
            do_login(mon, "1234")
            wait_for(log, M["done"], 180)
            time.sleep(0.5)
            mon.sendkey("fos == gui")
            wait_for(log, "[gfx/desktop] desktop: gui mode active", 60)
            time.sleep(1.0)
            mon.sendkey("help")
            time.sleep(0.5)
            # Иконка explorer: (16..64, 100..148), центр (40,124);
            # курсор в центре экрана (640,400).
            mon.cmd("mouse_move -600 -276")
            time.sleep(0.3)
            mon.cmd("mouse_button 1")
            time.sleep(0.2)
            mon.cmd("mouse_button 0")
            time.sleep(0.5)
            # notepad командой: cmd ещё в фокусе (клик по иконке только
            # выделяет её).
            mon.sendkey("notepad")
            time.sleep(1.5)
            # Возврат фокуса: кнопка cmd на панели (588,782).
            # dy: HMP плюс = вниз по экрану (40,124) -> (588,782): +658.
            mon.cmd("mouse_move 548 658")
            time.sleep(0.3)
            mon.cmd("mouse_button 1")
            time.sleep(0.2)
            mon.cmd("mouse_button 0")
            time.sleep(0.5)
            # Start-меню с поиском и плитками: ждём маркер исполнения.
            mon.sendkey("start")
            wait_for(log, "[shell] start: toggled", 60)
            time.sleep(2.0)
            mon.cmd("screendump " + ppm)
        elif phase == "console":
            wait_for(log, M["bootmgr"], 180)
            for _ in range(12):
                mon.raw("sendkey shift 250")
                if M["bm_shift"] in open(log, errors="replace").read():
                    break
                time.sleep(0.15)
            wait_for(log, M["done"], 180)
            time.sleep(0.5)
            mon.sendkey("help")
            time.sleep(0.5)
            mon.cmd("screendump " + ppm)
    finally:
        try:
            mon.cmd("quit")
        except Exception:
            pass
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()

    png = os.path.join(OUT, f"fnd2e-{shot_name}.png")
    try:
        ppm_to_png(ppm, png)
        ok = os.path.isfile(png)
    except Exception as e:
        print(f"{shot_name}: ppm_to_png error: {e}")
        ok = False
    print(f"{shot_name}: {'OK ' + png if ok else 'FAILED'}")
    return vars_fd


def main():
    # Аргумент — имя одного скриншота (частичный прогон в песочнице);
    # без аргумента снимаются все.
    only = sys.argv[1] if len(sys.argv) > 1 else None
    shots = [
        ("splash", "splash"),    # 1. splash на чистом varstore
        ("oobe", "oobe"),        # 2. OOBE-мастер
        ("save", "save"),        # 3. OOBE save: перенос подзаголовка
        ("login", "login"),      # 4. экран входа
        ("desktop", "desktop"),  # 5. TUI-десктоп
        ("gui", "gui"),          # 6. GUI-десктоп: Start + окна (Shell 2.0)
        ("console", "console"),  # 7. kernel console по Shift
    ]
    for name, phase in shots:
        if only is None or only == name:
            boot_with_shot(name, phase)


if __name__ == "__main__":
    main()
