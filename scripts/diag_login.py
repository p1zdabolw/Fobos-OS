#!/usr/bin/env python3
"""Диагностика связки «загрузчик читает fobos.cfg, записанный ядром».

1. Если на ESP нет настроек — прогон OOBE + `fos == gui --lock` + reboot
   (аналог фазы A сценария desktop).
2. Второй гость на том же ESP: проверка маркеров фазы B
   ([loader] bootcfg: mode=gui / [boot] fobos.cfg: mode=gui (locked))
   и логина (fast-ввод, как в сценарии).

Запуск: python3 scripts/diag_login.py [--esp build/esp.img]
Выход:  0 = фаза B отработала (session opened), 1 = нет.
"""
import argparse
import subprocess
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_selftest import (Guest, Monitor, find_qemu, find_ovmf, boot_to,  # noqa: E402
                           drive_oobe, M)


def provision(mon, g, failures):
    """OOBE + залочить gui, перезагрузка. True = успех."""
    if not boot_to(mon, g.log_path, M["oobe_welcome"], failures, 120):
        return False
    drive_oobe(mon, "admin", "4321")
    if not boot_to(mon, g.log_path, M["oobe_saved"], failures, 240):
        return False
    if not boot_to(mon, g.log_path, M["login_wait"], failures, 180):
        return False
    time.sleep(0.5)
    mon.sendkey("4321")
    if not boot_to(mon, g.log_path, M["session"], failures, 180):
        return False
    mon.sendkey("fos == gui --lock")
    if not boot_to(mon, g.log_path, M["fos_gui_1"], failures, 60):
        return False
    time.sleep(1.0)
    mon.sendkey("reboot")
    return g.wait_exit(60)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--esp", default="build/esp.img")
    ap.add_argument("--log", default="build/serial.log.diag")
    ap.add_argument("--skip-provision", action="store_true")
    a = ap.parse_args()

    qemu = find_qemu()
    ovmf_code, ovmf_vars = find_ovmf()
    Guest.ovmf_code = ovmf_code
    Guest.ovmf_vars = ovmf_vars
    import tempfile, shutil
    tmp = tempfile.mkdtemp(prefix="fobos-diag-")
    vars_fd = os.path.join(tmp, "vars.fd")
    shutil.copyfile(ovmf_vars, vars_fd)

    failures = []
    g = Guest(qemu, a.esp, vars_fd, a.log, tmp, "diag")
    g.start()
    time.sleep(3.0)
    if g.proc.poll() is not None:
        print("diag: QEMU умер, rc=", g.proc.poll())
        print(g.proc.stderr.read().decode(errors="replace"))
        return 2
    try:
        mon = g.monitor()
        provisioned = a.skip_provision
        if not provisioned:
            # Есть ли настройки? Маркер setup: file found в loader-логе.
            if boot_to(mon, g.log_path, M["setup_found"], [], 15):
                provisioned = True
                print("diag: настройки есть, пропуск OOBE")
            else:
                print("diag: настроек нет, прогон OOBE + gui --lock")
                if not provision(mon, g, failures):
                    print("diag: провижин не удался:", failures)
                    return 1
                print("diag: провижин OK, фаза B...")
        if not provisioned and False:
            pass
    finally:
        g.stop()
        try:
            g.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:  # noqa: F821
            g.proc.kill()

    # --- фаза B: второй гость на том же ESP и varstore ---
    g_b = Guest(qemu, a.esp, vars_fd, a.log, tmp, "diag-b")
    g_b.start()
    time.sleep(3.0)
    if g_b.proc.poll() is not None:
        print("diag: QEMU-B умер, rc=", g_b.proc.poll())
        print(g_b.proc.stderr.read().decode(errors="replace"))
        return 2
    try:
        mon = g_b.monitor()
        ok1 = boot_to(mon, g_b.log_path, M["bootcfg_gui"], [], 120)
        ok2 = boot_to(mon, g_b.log_path, M["lock_gui"], [], 120)
        if not ok1:
            print("diag: МАРКЕР [loader] bootcfg: mode=gui ОТСУТСТВУЕТ")
            txt = open(g_b.log_path, errors="replace").read()
            for line in txt.splitlines():
                if "bootcfg" in line or "hex:" in line:
                    print("  |", line)
        if not boot_to(mon, g_b.log_path, M["login_wait"], failures, 180):
            print("diag: логин-экран не достигнут:", failures)
            return 1
        time.sleep(0.3)
        mon.sendkey("4321")
        ok3 = boot_to(mon, g_b.log_path, M["session"], failures, 90)
        print("diag: bootcfg_gui=%s lock_gui=%s session=%s" %
              (ok1, ok2, ok3))
        return 0 if (ok1 and ok2 and ok3) else 1
    finally:
        g_b.stop()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
