#!/usr/bin/env python3
"""
Fobos OS — QEMU-харнесс e2e-проверок FND-2b (сценарный).

Поток загрузки теперь: loader boot manager (ESC=Setup) -> [OOBE при первом
запуске] -> ядро (splash 3 c, Shift -> kernel console) -> login -> desktop.

Сценарии (в одном прогоне `make selftest`, последовательные запуски QEMU):
  1. oobe       — чистый varstore: OOBE-мастер проходим через sendkey
                  (hostname/username/password/confirm/more=none), логин,
                  десктоп, интерактив (мышь, selftest#2, fbtest), reboot.
  2. persist    — ЧИСТЫЙ_ varstore из сценария 1: переменная FobosSetup
                  подхватывается загрузчиком (без OOBE), логин паролем,
                  десктоп, Ctrl+Alt+Del -> reset.
  3. esc-reset  — varstore из сценария 1: ESC в окне boot manager ->
                  OOBE заново -> логин -> desktop -> poweroff (выход QEMU).
  4. shift      — varstore из сценария 1: во время splash удерживаем Shift
                  (sendkey shift 6000) -> режим kernel console, whoami,
                  poweroff.
  5. desktop    — FND-2c: storage/AHCI/FAT32, ls/cat по FAT32, амнезия
                  on/off, fos == gui/tui (+--lock), reboot -> залоченный
                  gui при следующей загрузке, возврат к tui --lock.

Любой отсутствующий маркер = FAIL с дампом serial-лога.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

RE_SUMMARY = re.compile(r"kselftest: summary: (\d+) passed, (\d+) failed")
RE_MOUSE = re.compile(
    r"\[shell\] mouse: x=(\d+) y=(\d+) buttons=(\d+) ok=(\d+) bad=(\d+)")

M = {
    "bootmgr":      "[loader] fobos boot manager",
    "entry":        "FOBOS: kernel entry",
    "esc_window":   "[loader] boot manager: esc window",
    "oobe_welcome": "[loader] oobe: step=welcome",
    "var_found":    "[loader] setup: variable found (users=",
    "reset_esc":    "[loader] setup: reset requested (ESC)",
    "oobe_saved":   "[loader] oobe: saved users=",
    "splash":       "[boot] splash:",
    "bm_shift":     "[loader] boot manager: shift detected",
    "shift_console":"[boot] console:",
    "login_wait":   "[boot] login: waiting for user",
    "session":      "[session] opened user=",
    "done":         "FOBOS: boot complete",
    "panic":        "PANIC",
    "login_fail":   "[login] wrong password user=",
    # FND-2c: файл setup.cfg — первичный носитель, UEFI-переменная — фолбэк.
    "setup_found":  "[loader] setup: file found (users=",
    # FND-2c: дисковая подсистема, файлы, амнезия, режимы десктопа.
    "storage_ok":   "[shell] storage: ahci=1 fat=1 amnesia=0",
    "ls_fobos_pre": "[shell] ls: path=/fobos count=",
    "cat_notes":    "[shell] cat: path=/fobos/NOTES.TXT bytes=",
    # FND-2d: запись/копирование/удаление + защита системных имён.
    "write_note2":  "[shell] write: path=/fobos/NOTE2.TXT bytes=",
    "cat_note2":    "[shell] cat: path=/fobos/NOTE2.TXT bytes=",
    "cp_note2":     "[shell] cp: src=/fobos/NOTE2.TXT dst=/fobos/COPY2.TXT bytes=",
    "rm_copy2":     "[shell] rm: path=/fobos/COPY2.TXT",
    "rm_protected": "[shell] rm: refused protected path=/fobos/FOBOS.CFG",
    "amn_on":       "[shell] amnezia: on",
    "amn_off":      "[shell] amnezia: off",
    "fos_gui_0":    "[shell] fos: mode=gui locked=0",
    "fos_gui_1":    "[shell] fos: mode=gui locked=1",
    "fos_tui_1":    "[shell] fos: mode=tui locked=1",
    "dsk_gui":      "[gfx/desktop] desktop: gui mode active",
    "dsk_tui":      "[gfx/desktop] desktop: tui mode active",
    "bootcfg_gui":  "[loader] bootcfg: mode=gui",
    "bootcfg_tui":  "[loader] bootcfg: mode=tui",
    "lock_gui":     "[boot] fobos.cfg: mode=gui (locked)",
    # FND-2e: shell-команды GUI-приложений.
    "apps_listed":  "[shell] apps: listed",
    "pins_listed":  "[shell] pins: listed",
    "notepad_open": "[shell] notepad: opened from shell",
    "calc_open":    "[shell] calc: opened from shell",
    "set_open":     "[shell] settings: opened from shell",
    "start_toggle": "[shell] start: toggled",
    # FND-2f: LFN-запись (длинные имена > 8.3) + чтение по алиасу.
    "write_lfn":    "[shell] write: path=/fobos/longfilename.txt bytes=",
    "cat_lfn":      "[shell] cat: path=/fobos/longfilename.txt bytes=",
    "cat_lfn_alias": "[shell] cat: path=/fobos/LONGFI~1.TXT bytes=",
    "rm_lfn":       "[shell] rm: path=/fobos/longfilename.txt",
    # FND-3a: user-space процессы и syscall ABI v0.
    "sched_up":     "[sys/task] планировщик: пул",
    "abi_up":       "[sys/syscall] syscall ABI v0",
    "spawn_ok":     "[shell] run: spawned name=",
    "user_ok":      "[user] USEROK",
    "mem_ok":       "[user] MEMOK",
    "task_exit":    "[sys/task] task: exit pid=",
    "task_reap":    "[sys/task] task: reap pid=",
    "mem_cmd":      "[shell] mem: total_mib=",
    "ps_cmd":       "[shell] ps: tasks=",
    "run_neg":      "[shell] run: failed path=/fobos/nosuch.elf rc=-9",
    # FND-3b: системный установщик.
    "bm_install":   "[loader] boot manager: install requested (I)",
    "setup_fresh":  "[loader] setup: fresh media + target disk -> installer",
    "setup_welcome":"[setup] step=welcome",
    "setup_license":"[setup] step=license",
    "setup_disks":  "[setup] step=disks",
    "setup_drives": "[setup] step=drives",
    "setup_esp":    "[setup] phase=create ok",       # создание ESP ('e')
    "setup_confirm":"[setup] step=confirm",
    "setup_fmt_esp":"[setup] phase=format esp ok",
    "setup_fmt_dat":"[setup] phase=format data ok",
    "setup_copy":   "[setup] copy: data bytes=",
    "setup_copyesp":"[setup] copy: esp bytes=",
    "setup_cfg":    "[setup] cfg: fobos.cfg written (esp)",
    "setup_done":   "[setup] install complete",
    "setup_target": "[setup] target: setup.cfg written bytes=",
    "setup_restart":"[loader] setup: restart requested (cold reset)",
}

OVMF_CANDIDATES = [
    "/home/z/tools/root/usr/share/OVMF",
    "/home/z/my-project/tools/root/usr/share/OVMF",
    "/tmp/pkg/local-root/usr/share/OVMF",
    "/usr/share/OVMF",
    "/usr/share/ovmf",
    "/usr/share/edk2-ovmf",
]

ROM_DIR_CANDIDATES = [
    "/home/z/tools/root/usr/share/qemu",
    "/home/z/my-project/tools/root/usr/share/qemu",
    "/home/z/my-project/tools/root/usr/share/seabios",
    "/tmp/pkg/local-root/usr/share/qemu",
    "/tmp/pkg/local-root/usr/share/seabios",
    "/usr/share/qemu",
    "/home/z/tools/root/usr/share/seabios",
    "/usr/share/seabios",
    "/home/z/tools/root/usr/share/vgabios",
    "/usr/share/vgabios",
]

QEMU_CANDIDATES = [
    "/home/z/tools/root/usr/bin/qemu-system-x86_64",
    "/home/z/my-project/tools/root/usr/bin/qemu-system-x86_64",
    "/tmp/pkg/local-root/usr/bin/qemu-system-x86_64",
]


def find_qemu() -> str:
    env = os.environ.get("FOBOS_QEMU")
    if env and os.path.isfile(env):
        return env
    for c in QEMU_CANDIDATES:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    q = shutil.which("qemu-system-x86_64")
    if q:
        return q
    print("qemu_selftest: qemu-system-x86_64 не найден (см. FOBOS_QEMU)")
    sys.exit(2)


def find_ovmf() -> tuple:
    env = os.environ.get("FOBOS_OVMF_DIR")
    dirs = [env] if env else OVMF_CANDIDATES
    for d in dirs:
        code = os.path.join(d, "OVMF_CODE_4M.fd")
        vars_ = os.path.join(d, "OVMF_VARS_4M.fd")
        if os.path.isfile(code) and os.path.isfile(vars_):
            return code, vars_
        code = os.path.join(d, "OVMF_CODE.fd")
        vars_ = os.path.join(d, "OVMF_VARS.fd")
        if os.path.isfile(code) and os.path.isfile(vars_):
            return code, vars_
    print("qemu_selftest: OVMF не найден (см. FOBOS_OVMF_DIR)")
    sys.exit(2)


class Monitor:
    """HMP-клиент QEMU через unix-сокет."""

    def __init__(self, path: str):
        self.path = path
        self.sock = None

    def connect(self) -> None:
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + 30
        while time.time() < deadline:
            try:
                self.sock.connect(self.path)
                break
            except (ConnectionRefusedError, FileNotFoundError,
                    ConnectionResetError):
                # Reset бывает транзиентным при перезапуске гостя: старый
                # сокет-файл ещё существует, новый QEMU ещё не сделал bind.
                time.sleep(0.2)
        else:
            raise RuntimeError("monitor-сокет не открылся за 30 с")
        self.sock.settimeout(2.0)
        self.drain()

    def drain(self) -> str:
        out = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                out += chunk
            except socket.timeout:
                break
        return out.decode(errors="replace")

    def drain_fast(self) -> None:
        """Быстрый дренаж эха монитора: 0.25 с на пустом сокете вместо 2 с."""
        self.sock.settimeout(0.25)
        try:
            while True:
                if not self.sock.recv(4096):
                    break
        except socket.timeout:
            pass
        finally:
            self.sock.settimeout(2.0)

    def cmd(self, line: str) -> str:
        self.sock.sendall((line + "\n").encode())
        time.sleep(0.02)
        return self.drain()

    def raw(self, line: str) -> None:
        """Fire-and-forget: шлёт команду без ожидания ответа (минимальный лаг)."""
        self.sock.sendall((line + "\n").encode())
        time.sleep(0.02)

    def sendkey(self, text: str, hold_ms: int = None) -> None:
        """Печатает текст в гостя через PS/2 (HMP sendkey).
        FND-2d: raw() вместо cmd() — cmd ждал тайм-аут сокета (~2 с) на
        КАЖДЫЙ символ, из-за чего длинные команды печатались десятки секунд
        и сценарий desktop не влезал в бюджет. Эхо дренажится пачками."""
        keymap = {c: c for c in "abcdefghijklmnopqrstuvwxyz"}
        keymap.update({c: c for c in "0123456789"})
        keymap.update({" ": "spc", "\n": "ret", "-": "minus",
                       ".": "dot", "/": "slash", "=": "equal",
                       "\\": "backslash", ";": "semicolon",
                       "'": "apostrophe", "[": "bracket_left",
                       "]": "bracket_right", "`": "grave_accent",
                       ",": "comma", "~": "shift-grave_accent",
                       "@": "shift-2", "^": "shift-6", "&": "shift-7",
                       "*": "shift-8", "(": "shift-9", ")": "shift-0",
                       "_": "shift-minus", "+": "shift-equal",
                       "?": "shift-slash", "!": "shift-1",
                       "#": "shift-3", "$": "shift-4", "%": "shift-5"})
        if hold_ms is not None:
            key = keymap.get(text)
            if key is None:
                raise RuntimeError(f"нет маппинга для {text!r}")
            self.cmd(f"sendkey {key} {hold_ms}")
            return
        n = 0
        for ch in text:
            if ch.isupper() and ch.lower() in keymap:
                self.raw(f"sendkey shift-{keymap[ch.lower()]}")
            else:
                key = keymap.get(ch)
                if key is None:
                    raise RuntimeError(f"нет маппинга для символа {ch!r}")
                self.raw(f"sendkey {key}")
            n += 1
            if n % 16 == 0:
                self.drain_fast()
            time.sleep(0.03)
        if not text.endswith("\n"):
            self.raw("sendkey ret")
        self.drain_fast()

    def close(self) -> None:
        if self.sock is not None:
            try:
                self.sock.close()
            except OSError:
                pass


import socket  # noqa: E402


def log_text(path: str) -> str:
    try:
        with open(path, "r", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(log_path: str, marker: str, timeout: float) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if marker in log_text(log_path):
            return True
        time.sleep(0.2)
    return False


def count_of(log_path: str, marker: str) -> int:
    return log_text(log_path).count(marker)


def wait_ci(log_path: str, marker: str, timeout: float) -> bool:
    """Ожидание маркера без учёта регистра: под нагрузкой TCG клавиатурный
    ввод может прийти с залипшим Shift (make без break в очереди 8042/кольце
    kbd), а маркеры с путями повторяют регистр набранного текста."""
    m = marker.lower()
    deadline = time.time() + timeout
    while time.time() < deadline:
        if m in log_text(log_path).lower():
            return True
        time.sleep(0.2)
    return False


def console_drain(mon) -> None:
    """Чистка консоли после тяжёлой фазы загрузки: принудительный make+break
    Shift (две пары подряд — снимает залипший модификатор, если break первой
    пары потерялся) и пустая строка (исполняет возможный обрывок ввода и
    возвращает приглашение). Вызывается ПЕРЕД первой клавиатурной командой."""
    time.sleep(2.0)
    mon.raw("sendkey shift")
    time.sleep(0.3)
    mon.raw("sendkey shift")
    time.sleep(0.3)
    mon.raw("sendkey ret")
    time.sleep(0.8)


def esc_reset_retry(g, log_path: str, failures: list,
                    attempts: int = 3) -> tuple:
    """ESC-сброс настроек через окно boot manager с повторными попытками.

    ESC давим с ПЕРВОГО маркера загрузчика (M["bootmgr"]): ConIn прошивки
    буферизует нажатия, сделанные до открытия ~1-с окна, поэтому раннее
    нажатие доезжает до окна — под нагрузкой TCG это снимает гонку
    «успеть за 1 с». Не попали (гость ушёл в загрузку по умолчанию) —
    перезапускаем гостя и пробуем снова. Bounded: attempts.
    0 = настройки сброшены (маркер reset_esc).

    Перезапуск гостя — НОВЫМ процессом QEMU (g.stop() + g.start()), а не
    system_reset через монитор: QEMU запущен с -no-reboot, любое событие
    reset завершает процесс, монитор-сокет умирает (BrokenPipe). Дополнительно
    g.start() усекает лог-файл, поэтому маркеры прошлой попытки не будут
    ложно засчитаны в следующей. Возвращает (ok, mon) — монитор актуального
    гостя (пересозданный, если был перезапуск)."""
    mon = None
    last_err = ""
    for attempt in range(attempts):
        try:
            if mon is not None:
                # Фикс утечки: соединение монитора прошлой попытки держало
                # chardev-сокет, и повторный коннект к занятому chardev
                # давал гонку/ECONNRESET при перезапуске гостя.
                mon.close()
                mon = None
            if attempt > 0:
                g.stop()
                g.start()
            mon = g.monitor()
        except (OSError, RuntimeError) as e:
            # Гость умер до входа (connect/refused/reset) — диагностика и
            # следующая попытка (перезапуск нового процесса QEMU).
            last_err = (f"попытка {attempt}: гость не поднялся: {e!r} "
                        f"(qemu poll={g.proc.poll() if g.proc else '?'})")
            print(f"qemu_selftest: {last_err}")
            g.dump_err()
            time.sleep(1.0)
            continue
        # С bootmgr-маркера (не с 'esc window'): ConIn буферизует ранние
        # нажатия. Bounded: 60 с/попытку, выход — reset_esc или kernel entry.
        if boot_to(mon, log_path, M["bootmgr"], [], 120):
            deadline = time.time() + 60
            while time.time() < deadline:
                mon.raw("sendkey esc")
                if M["reset_esc"] in log_text(log_path):
                    return True, mon
                if "[boot] FOBOS: kernel entry" in log_text(log_path):
                    break   # окно пропущено — на перезапуск
                time.sleep(0.15)
        time.sleep(1.0)
    failures.append("ESC-сброс настроек не сработал "
                    f"({last_err or 'нет маркера reset_esc'}, "
                    f"попыток: {attempts})")
    return False, mon


class Guest:
    """Один запуск QEMU с monitor-сокетом и serial-логом."""

    # Реестр живых гостей: фазовые госты (g_b в сценариях 5/6/7) создаются
    # ВНУТРИ сценарной функции, и finally в main() останавливает только
    # внешний g — при провале внутри фазы B гость g_b утекал и продолжал
    # жечь CPU (наблюдалось: зомби-QEMU 88% CPU душит все последующие
    # прогоны в песочнице с 2 CPU). start() регистрирует, stop() снимает,
    # stop_all() — гарантированная зачистка в main().
    _active = []

    def __init__(self, qemu, esp, vars_fd, log_path, tmp, name,
                 extra_drives=None):
        self.qemu = qemu
        self.esp = esp
        self.vars_fd = vars_fd
        self.log_path = log_path
        self.tmp = tmp
        self.name = name
        self.extra_drives = extra_drives or []
        self.proc = None
        self.mon_path = os.path.join(tmp, f"mon-{name}.sock")
        self.fw_log = log_path + ".fw.log"
        # stderr QEMU — в файл: при смерти процесса причина остаётся
        # доступной для диагностики (pipe без читателя — молчаливая потеря).
        self.err_path = log_path + ".qemu.err"

    def start(self):
        with open(self.log_path, "wb"):
            pass
        cmd = [
            self.qemu,
            "-machine", "q35",
            "-m", "512",
            "-smp", "1",
        ]
        for rom_dir in ROM_DIR_CANDIDATES:
            if os.path.isdir(rom_dir):
                cmd += ["-L", rom_dir]
        cmd += [
            "-drive", "if=pflash,format=raw,readonly=on,file=" + self.ovmf_code,
            "-drive", "if=pflash,format=raw,file=" + self.vars_fd,
            "-drive", f"file={self.esp},format=raw,if=ide",
        ]
        for extra in self.extra_drives:
            cmd += ["-drive", f"file={extra},format=raw,if=ide"]
        cmd += [
            "-display", "none",
            "-net", "none",
            "-chardev", f"socket,id=mon0,path={self.mon_path},server=on,wait=off",
            "-mon", "chardev=mon0,mode=readline",
            "-serial", f"file:{self.log_path}",
            "-debugcon", f"file:{self.fw_log}",
            "-global", "isa-debugcon.iobase=0x402",
            "-no-reboot",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        ]
        env = dict(os.environ)
        tool_lib = "/home/z/tools/root/usr/lib/x86_64-linux-gnu"
        if os.path.isdir(tool_lib):
            env["LD_LIBRARY_PATH"] = tool_lib + ":" + env.get("LD_LIBRARY_PATH", "")
        pkg_lib = "/tmp/pkg/local-root/usr/lib/x86_64-linux-gnu"
        if os.path.isdir(pkg_lib):
            env["LD_LIBRARY_PATH"] = pkg_lib + ":" + env.get("LD_LIBRARY_PATH", "")
        proj_lib = "/home/z/my-project/tools/root/usr/lib/x86_64-linux-gnu"
        if os.path.isdir(proj_lib):
            env["LD_LIBRARY_PATH"] = proj_lib + ":" + env.get("LD_LIBRARY_PATH", "")
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                     stderr=open(self.err_path, "wb"), env=env)
        Guest._active.append(self)

    @classmethod
    def stop_all(cls) -> int:
        """Останавливает всех незавершённых гостей. Возвращает число убитых
        по таймауту (для диагностики). Вызывается из main() в finally."""
        killed = 0
        for guest in list(cls._active):
            if guest.proc is not None and guest.proc.poll() is None:
                try:
                    mon = guest.monitor()
                    mon.cmd("quit")
                    mon.close()
                except Exception:
                    pass
                try:
                    guest.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    guest.proc.kill()
                    killed += 1
            if guest in cls._active:
                cls._active.remove(guest)
        return killed

    def dump_err(self, tail: int = 15) -> None:
        """Печатает stderr погибшего QEMU (если он что-то успел написать)."""
        try:
            with open(self.err_path, "rb") as f:
                data = f.read()
        except OSError:
            return
        if data:
            print(f"qemu_selftest: qemu stderr ({self.name}):")
            for line in data.decode(errors="replace").splitlines()[-tail:]:
                print("  " + line)

    def monitor(self) -> Monitor:
        mon = Monitor(self.mon_path)
        mon.connect()
        return mon

    def wait_exit(self, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return True
            time.sleep(0.3)
        return False

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                mon = self.monitor()
                mon.cmd("quit")
                mon.close()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self in Guest._active:
            Guest._active.remove(self)


def boot_to(mon: Monitor, log_path: str, marker: str, failures: list,
            timeout: float = 120) -> bool:
    """Ждёт маркер, возвращает True/False (добавляет failure)."""
    if wait_for(log_path, marker, timeout):
        return True
    failures.append(f"{marker!r} не появился за {timeout:.0f} с")
    return False


def boot_to_count(mon: Monitor, log_path: str, marker: str, min_count: int,
                  failures: list, timeout: float = 120) -> bool:
    """Ждёт, пока маркер не появится в логе min_count раз (нужен, когда
    один и тот же маркер печатается на каждый однотипный шаг)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if count_of(log_path, marker) >= min_count:
            return True
        time.sleep(0.2)
    failures.append(f"{marker!r} не появился {min_count}x за {timeout:.0f} с")
    return False


def drive_oobe(mon: Monitor, username, password):
    """Проходит OOBE-мастер клавиатурой (см. шаги в oobe.c).
    hostname: пустой ввод -> умолчание fobos-pc (поле предзаполнено)."""
    def line(text):
        mon.sendkey(text)  # sendkey сам добавляет ret

    # welcome: Enter
    time.sleep(0.5)
    line("")
    # hostname: Enter -> default fobos-pc
    time.sleep(0.5)
    line("")
    # username
    time.sleep(0.5)
    line(username)
    # password
    time.sleep(0.5)
    line(password)
    # confirm
    time.sleep(0.5)
    line(password)
    # more users: n
    time.sleep(0.5)
    line("n")
    # save...
    time.sleep(0.5)


def do_login(mon: Monitor, password):
    """Логин: имя предзаполнено (единственный пользователь), вводим пароль."""
    time.sleep(0.3)
    mon.sendkey(password)


def scenario_1_oobe(g: Guest, failures: list) -> None:
    mon = g.monitor()

    # Загрузка до boot manager и OOBE (свежий varstore -> мастер обязателен).
    if not boot_to(mon, g.log_path, M["bootmgr"], failures):
        return
    if not boot_to(mon, g.log_path, M["oobe_welcome"], failures, 120):
        return
    drive_oobe(mon, "admin", "1234")
    if not boot_to(mon, g.log_path, M["oobe_saved"], failures, 240):
        return
    # Логин-экран
    if not boot_to(mon, g.log_path, M["login_wait"], failures, 180):
        return
    # Не ждём splash полностью — логин уже ждёт; вводим пароль.
    do_login(mon, "1234")
    if not boot_to(mon, g.log_path, M["session"], failures, 180):
        return
    if not boot_to(mon, g.log_path, M["done"], failures, 60):
        return

    # --- интерактив как в FND-2a ---
    def need_mouse(timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = last_mouse(g.log_path)
            if m is not None:
                return m
            time.sleep(0.3)
        return None

    mon.sendkey("mouse")
    m0 = need_mouse(60)
    if m0 is None:
        failures.append("нет ответа команды mouse")
        return
    mon.cmd("mouse_move 100 60")
    time.sleep(0.5)
    mon.sendkey("mouse")
    m1 = need_mouse(60)
    if m1 is None:
        failures.append("нет второго ответа команды mouse")
        return
    dx = m1[0] - m0[0]
    dy = m1[1] - m0[1]
    print(f"qemu_selftest: mouse dx={dx} dy={dy}")
    if not (60 <= dx <= 140):
        failures.append(f"курсор не сместился на ~+100 px (dx={dx})")
    if not (30 <= abs(dy) <= 120):
        failures.append(f"курсор не сместился по Y (dy={dy})")

    before = count_of(g.log_path, "kselftest: summary:")
    mon.sendkey("selftest")
    deadline = time.time() + 300
    while time.time() < deadline:
        if count_of(g.log_path, "kselftest: summary:") > before:
            break
        time.sleep(0.5)
    else:
        failures.append("повторный kselftest не завершился")
        return
    txt = log_text(g.log_path)
    sums = RE_SUMMARY.findall(txt)
    if int(sums[-1][1]) != 0:
        failures.append(f"повторный kselftest упал: {sums[-1]}")
        return

    mon.sendkey("fbtest")
    time.sleep(1.0)
    mon.cmd("sendkey z")
    if not wait_for(g.log_path, "[shell] fbtest: done", 60):
        failures.append("fbtest не завершился по нажатию клавиши")

    # logout -> снова экран входа
    mon.sendkey("logout")
    if not boot_to(mon, g.log_path, "[session] closed", failures, 60):
        return
    if not boot_to(mon, g.log_path, M["login_wait"], failures, 60):
        return
    print("qemu_selftest: logout -> login OK")

    # повторный вход и reboot
    do_login(mon, "1234")
    if not boot_to(mon, g.log_path, M["session"], failures, 120):
        return
    mon.sendkey("reboot")
    if not g.wait_exit(60):
        failures.append("reboot не привёл к завершению QEMU за 60 с")

    mon.close()


def scenario_2_persist(g: Guest, failures: list) -> None:
    mon = g.monitor()
    if not boot_to(mon, g.log_path, M["bootmgr"], failures, 120):
        return
    # Полная последовательность: настройка уже записана сценарием 1
    # (файл \\fobos\\setup.cfg — первичный носитель, UEFI-переменная — фолбэк).
    # Standalone-запуск: носителей нет — проходим OOBE в этом же прогоне
    # (проверка файловой персистентности — в полной последовательности).
    if (not wait_for(g.log_path, M["setup_found"], 15) and
            not wait_for(g.log_path, M["var_found"], 5)):
        if not boot_to(mon, g.log_path, M["oobe_welcome"], failures, 60):
            return
        drive_oobe(mon, "admin", "1234")
        if not boot_to(mon, g.log_path, M["oobe_saved"], failures, 240):
            return
        print("qemu_selftest: (standalone) носителей нет - OOBE проезден локально")
    if not boot_to(mon, g.log_path, M["login_wait"], failures, 180):
        return
    # Неверный пароль -> сообщение об ошибке -> верный.
    mon.sendkey("9999")
    if not boot_to(mon, g.log_path, M["login_fail"], failures, 60):
        return
    time.sleep(1.0)
    mon.sendkey("1234")
    if not boot_to(mon, g.log_path, M["session"], failures, 180):
        return
    print("qemu_selftest: persisted login OK (wrong->right password)")

    # Ctrl+Alt+Del -> перезагрузка -> QEMU выходит (-no-reboot).
    time.sleep(1.0)
    mon.cmd("sendkey ctrl-alt-delete")
    if not g.wait_exit(60):
        failures.append("Ctrl+Alt+Del не перезагрузил машину за 60 с")
    mon.close()


def scenario_3_esc_reset(g: Guest, failures: list) -> None:
    # ESC-сброс через окно boot manager; при промахе — перезапуск гостя
    # и повторная попытка (окно ~1 с, под нагрузкой бывает пропущено).
    ok, mon = esc_reset_retry(g, g.log_path, failures)
    if not ok:
        if mon is not None:
            mon.close()
        return
    if not boot_to(mon, g.log_path, M["oobe_welcome"], failures, 60):
        return
    drive_oobe(mon, "admin", "4321")
    if not boot_to(mon, g.log_path, M["oobe_saved"], failures, 240):
        return
    if not boot_to(mon, g.log_path, M["login_wait"], failures, 180):
        return
    do_login(mon, "4321")
    if not boot_to(mon, g.log_path, M["session"], failures, 180):
        return
    print("qemu_selftest: ESC-reset -> new OOBE -> login OK")
    time.sleep(1.0)
    mon.sendkey("poweroff")
    if not g.wait_exit(60):
        failures.append("poweroff не завершил QEMU за 60 с")
    mon.close()


def scenario_4_shift(g: Guest, failures: list) -> None:
    mon = g.monitor()
    if not boot_to(mon, g.log_path, M["bootmgr"], failures, 120):
        return
    # ДЕТЕРМИНИРОВАННЫЙ путь: Shift ловится в окне boot manager (1 c)
    # прошивочным ConIn/EX. CI давит Shift с повторами, как живой человек
    # (make-коды кладутся в очередь прошивки; одно из событий доходит в
    # окно). Загрузчик ставит FBPI_BOOTF_CONSOLE, ядро грузится в консоль
    # без splash-гонки и без OOBE.
    deadline = time.time() + 30
    # Бурст коротких нажатий без ожидания ответа монитора: каждые ~0.4 c
    # один make/break попадает в 1-секундное окно boot manager. Сработка
    # возможна двумя путями (оба — легитимные механизмы продукта):
    #   a) окно загрузчика (EX/8042) -> FBPI_BOOTF_CONSOLE;
    #   б) окно splash ядра (3 c): make-коды из очереди QEMU доходят до
    #      ядра и включают Shift-модификатор.
    # Сценарий проверяет ИСХОД: система загрузилась в kernel console.
    while time.time() < deadline:
        mon.raw("sendkey shift 250")
        # Выход по ЛЮБОМУ из двух механизмов: иначе, если Shift поймал
        # splash ядра (путь «б»), цикл давит всю отведённую секунду/30 с и
        # очередь монитора (~75 команд по 250 мс при шаге 0.4 с) задерживает
        # следующие нажатия на десятки секунд.
        if (M["bm_shift"] in log_text(g.log_path)
                or M["shift_console"] in log_text(g.log_path)):
            break
        # 0.4 с, а не 0.1: шаг 0.1 при удержании 250 мс заливал очередь
        # монитора/8042 перекрывающимися make+break (в гостевой kbd-ring
        # попадали сотни shift-событий перед полезным вводом — при TCG-
        # starvation шелл обрабатывал их десятки секунд). 0.4 с > удержания
        # — очередь между нажатиями успевает опустеть, окно 1 с (в стен-)
        # времени под нагрузкой растягивается) по-прежнему ловит 1-2 нажатия.
        time.sleep(0.4)
    if not boot_to(mon, g.log_path, M["shift_console"], failures, 150):
        return
    if not boot_to(mon, g.log_path, M["done"], failures, 150):
        return
    # В консоли ядра: whoami == maintenance.
    time.sleep(1.0)
    mon.sendkey("whoami")
    if not wait_for(g.log_path, "[shell] whoami=maintenance", 150):
        failures.append("whoami в kernel console не вернул maintenance")
        return
    mon.sendkey("poweroff")
    if not g.wait_exit(120):
        failures.append("poweroff из kernel console не завершил QEMU")
    mon.close()


def scenario_5_desktop(g: Guest, failures: list) -> None:
    """FND-2c: файлы, амнезия, режимы десктопа и их персистентность.

    Детерминизм: ESC-сброс настроек (файл setup.cfg + переменная
    очищаются) -> свежий OOBE с известным паролем.
    Фаза A (гость 1): OOBE -> логин -> storage/ls/cat -> амнезия on/off ->
    fos == gui / fos == tui -> fos == gui --lock -> reboot.
    Фаза B (гость 2, тот же ESP): загрузчик читает fobos.cfg ->
    ядро стартует в gui -> fos == tui --lock (возврат к умолчанию,
    чтобы общий ESP не ломал сценарии 1-4 при повторных прогонах).
    """
    mon = g.monitor()
    if not boot_to(mon, g.log_path, M["bootmgr"], failures, 120):
        return
    mon.close()
    # Детерминированный сброс настроек (как сценарий esc): окно boot
    # manager + ретраи с перезапуском гостя (см. esc_reset_retry).
    ok, mon = esc_reset_retry(g, g.log_path, failures)
    if not ok:
        if mon is not None:
            mon.close()
        return
    if not boot_to(mon, g.log_path, M["oobe_welcome"], failures, 60):
        return
    drive_oobe(mon, "admin", "4321")
    if not boot_to(mon, g.log_path, M["oobe_saved"], failures, 240):
        return
    if not boot_to(mon, g.log_path, M["login_wait"], failures, 180):
        return
    do_login(mon, "4321")
    if not boot_to(mon, g.log_path, M["session"], failures, 180):
        return

    # --- Дисковая подсистема: AHCI -> MBR -> FAT32 ---
    mon.sendkey("storage")
    if not boot_to(mon, g.log_path, M["storage_ok"], failures, 60):
        return
    # --- Файлы FAT32 (в /fobos >= 2 записей: ядро + notes; setup.cfg
    # появляется после OOBE) ---
    mon.sendkey("ls /fobos")
    if not wait_ci(g.log_path, M["ls_fobos_pre"], 60):
        failures.append("'ls /fobos' не ответил за 60 с")
        return
    m = re.search(r"ls: path=/fobos count=(\d+)",
                  log_text(g.log_path).lower())
    if m is None or int(m.group(1)) < 2:
        failures.append(f"ls /fobos: записей меньше двух ({m.group(1) if m else '?'})")
        return
    mon.sendkey("cat /fobos/NOTES.TXT")
    if not wait_ci(g.log_path, M["cat_notes"], 60):
        failures.append(f"{M['cat_notes']!r} не появился за 60 с")
        return
    # --- FND-2d: write/cat/cp/rm + защита системных имён ---
    mon.sendkey("write /fobos/NOTE2.TXT hello fnd2d")
    if not boot_to(mon, g.log_path, M["write_note2"], failures, 60):
        return
    mon.sendkey("cat /fobos/NOTE2.TXT")
    if not boot_to(mon, g.log_path, M["cat_note2"], failures, 60):
        return
    mon.sendkey("cp /fobos/NOTE2.TXT /fobos/COPY2.TXT")
    if not boot_to(mon, g.log_path, M["cp_note2"], failures, 60):
        return
    mon.sendkey("rm /fobos/COPY2.TXT")
    if not boot_to(mon, g.log_path, M["rm_copy2"], failures, 60):
        return
    mon.sendkey("rm /fobos/FOBOS.CFG")
    if not boot_to(mon, g.log_path, M["rm_protected"], failures, 60):
        return
    # --- FND-2f: LFN-запись — длинные имена (> 8.3) с чтением по имени
    # и по сгенерированному алиасу; удаление по длинному имени ---
    mon.sendkey("write /fobos/longfilename.txt data for lfn test")
    if not boot_to(mon, g.log_path, M["write_lfn"], failures, 60):
        return
    mon.sendkey("cat /fobos/longfilename.txt")
    if not boot_to(mon, g.log_path, M["cat_lfn"], failures, 60):
        return
    mon.sendkey("cat /fobos/LONGFI~1.TXT")
    if not boot_to(mon, g.log_path, M["cat_lfn_alias"], failures, 60):
        return
    mon.sendkey("rm /fobos/longfilename.txt")
    if not boot_to(mon, g.log_path, M["rm_lfn"], failures, 60):
        return
    # --- Амнезия: запись только в RAM ---
    mon.sendkey("amnezia on")
    if not boot_to(mon, g.log_path, M["amn_on"], failures, 60):
        return
    mon.sendkey("amnezia status")
    if not boot_to(mon, g.log_path, "[shell] amnezia: state=1", failures, 60):
        return
    mon.sendkey("amnezia off")
    if not boot_to(mon, g.log_path, M["amn_off"], failures, 60):
        return
    # --- Режимы десктопа: tui <-> gui, сессия жива ---
    mon.sendkey("fos == gui")
    if not boot_to(mon, g.log_path, M["fos_gui_0"], failures, 60):
        return
    if not boot_to(mon, g.log_path, M["dsk_gui"], failures, 30):
        return
    mon.sendkey("fos == tui")
    if not boot_to(mon, g.log_path, M["dsk_tui"], failures, 60):
        return
    # --- Фиксация gui в \fobos\fobos.cfg + перезагрузка ---
    mon.sendkey("fos == gui --lock")
    if not boot_to(mon, g.log_path, M["fos_gui_1"], failures, 60):
        return
    time.sleep(1.0)
    mon.sendkey("reboot")
    if not g.wait_exit(60):
        failures.append("reboot фазы A не завершил QEMU за 60 с")
    mon.close()

    # --- Фаза B: тот же ESP, залоченный gui при следующей загрузке.
    # QEMU фазы A уже вышел (-no-reboot); запускаем второй гость с теми
    # же ESP и varstore (отдельный мон-сокет, лог перезаписывается).
    g_b = Guest(g.qemu, g.esp, g.vars_fd, g.log_path, g.tmp, g.name + "-b")
    g_b.start()
    mon = g_b.monitor()
    if not boot_to(mon, g.log_path, M["bootcfg_gui"], failures, 120):
        return
    if not boot_to(mon, g.log_path, M["lock_gui"], failures, 120):
        return
    if not boot_to(mon, g.log_path, M["login_wait"], failures, 180):
        return
    do_login(mon, "4321")
    if not boot_to(mon, g.log_path, M["session"], failures, 180):
        return
    # Десктоп стартовал сразу в gui: маркер dsk_gui уже в логе.
    if not boot_to(mon, g.log_path, M["dsk_gui"], failures, 30):
        return
    print("qemu_selftest: locked gui mode restored across reboot OK")

    # --- FND-2e: shell-команды и GUI-приложения ------------------------
    # Экран 1280x800, панель задач 36 px, 5 закреплений -> центральная
    # группа 204 px: Пуск x=538, кнопка cmd x=572..604 (центр 588),
    # y=766..798 (центр 782). Курсор после загрузки — в центре (640,400).
    # HMP mouse_move: dy в ЭКРАННЫХ координатах (плюс = вниз; QEMU сам
    # переводит в PS/2, ядро инвертирует обратно).
    def click_here():
        # HMP mouse_button: 1 = нажать левую, 0 = отпустить.
        mon.cmd("mouse_button 1"); time.sleep(0.15); mon.cmd("mouse_button 0")
        time.sleep(0.3)

    def refocus_cmd():
        # Курсор обязан стоять на кнопке cmd (588,782). Уводим влево по
        # панели (100,782) — «пустой» клик, затем обратно: фокус -> cmd.
        mon.cmd("mouse_move -488 0")
        time.sleep(0.3)
        click_here()
        mon.cmd("mouse_move 488 0")
        time.sleep(0.3)
        click_here()

    # Центр (640,400) -> кнопка cmd (588,782): dx=-52, dy=-382.
    mon.cmd("mouse_move -52 382")
    time.sleep(0.5)

    mon.sendkey("apps")
    if not boot_to(mon, g.log_path, M["apps_listed"], failures, 60):
        return
    mon.sendkey("pins")
    if not boot_to(mon, g.log_path, M["pins_listed"], failures, 60):
        return

    mon.sendkey("notepad")
    if not boot_to(mon, g.log_path, M["notepad_open"], failures, 60):
        return
    refocus_cmd()
    mon.sendkey("calc")
    if not boot_to(mon, g.log_path, M["calc_open"], failures, 60):
        return
    refocus_cmd()
    mon.sendkey("settings")
    if not boot_to(mon, g.log_path, M["set_open"], failures, 60):
        return
    refocus_cmd()
    mon.sendkey("start")
    if not boot_to(mon, g.log_path, M["start_toggle"], failures, 60):
        return
    # Закрыть Start: sendkey() хелпер печатает БУКВЫ ("e","s","c" попадают
    # в поисковую строку меню) — настоящий Escape шлём сырой HMP-командой.
    mon.raw("sendkey esc")
    time.sleep(1.5)

    # --- Возврат к умолчанию (общий ESP не должен ломать сценарии 1-4).
    # ВНИМАНИЕ: ввод в GUI = полный кадр с процедурными обоями на каждый
    # символ (TCG ~1-2 с/кадр) — длинной команде нужен увеличенный лимит.
    mon.sendkey("fos == tui --lock")
    if not boot_to(mon, g.log_path, M["fos_tui_1"], failures, 150):
        return
    if not boot_to(mon, g.log_path, M["dsk_tui"], failures, 30):
        return
    time.sleep(1.0)
    mon.sendkey("poweroff")
    if not g_b.wait_exit(60):
        failures.append("poweroff фазы B не завершил QEMU за 60 с")
    mon.close()



def scenario_6_userspace(g: Guest, failures: list) -> None:
    """FND-3a: user-space процессы, syscall ABI v0, изоляция, утечки.

    Детерминированный путь сценария 4 (Shift в окне boot manager) ->
    kernel console. Далее: run hello (USEROK + exit + reap), run mtest
    (SYS_brk: рост/запись/чтение/возврат барьера), ps (пул вернулся к
    idle), mem до/после (нет утечки кадров), негатив run nosuch
    (честный K_ENOENT), повторный запуск (реюз слота).
    """
    mon = g.monitor()
    if not boot_to(mon, g.log_path, M["bootmgr"], failures, 120):
        return
    deadline = time.time() + 30
    while time.time() < deadline:
        mon.raw("sendkey shift 250")
        # Выход по ЛЮБОМУ из двух механизмов: иначе, если Shift поймал
        # splash ядра (путь «б»), цикл давит всю отведённую секунду/30 с и
        # очередь монитора (~75 команд по 250 мс при шаге 0.4 с) задерживает
        # следующие нажатия на десятки секунд.
        if (M["bm_shift"] in log_text(g.log_path)
                or M["shift_console"] in log_text(g.log_path)):
            break
        # 0.4 с, а не 0.1: шаг 0.1 при удержании 250 мс заливал очередь
        # монитора/8042 перекрывающимися make+break (в гостевой kbd-ring
        # попадали сотни shift-событий перед полезным вводом — при TCG-
        # starvation шелл обрабатывал их десятки секунд). 0.4 с > удержания
        # — очередь между нажатиями успевает опустеть, окно 1 с (в стен-)
        # времени под нагрузкой растягивается) по-прежнему ловит 1-2 нажатия.
        time.sleep(0.4)
    if not boot_to(mon, g.log_path, M["shift_console"], failures, 150):
        return
    if not boot_to(mon, g.log_path, M["done"], failures, 150):
        return
    # Подсистемы этапа подняты (маркеры уже в логе к моменту done).
    if not boot_to(mon, g.log_path, M["sched_up"], failures, 10):
        return
    if not boot_to(mon, g.log_path, M["abi_up"], failures, 10):
        return
    # Прогрев/чистка консоли: пустая строка сбрасывает возможный частичный
    # ввод, make+break Shift — залипший модификатор (см. console_drain).
    console_drain(mon)

    # mem ДО: точка отсчёта для проверки утечек.
    mon.sendkey("mem")
    if not boot_to(mon, g.log_path, M["mem_cmd"], failures, 30):
        return
    free_before = free_mib(log_text(g.log_path))

    # hello: полный жизненный цикл (spawn -> write -> exit -> reap).
    mon.sendkey("run hello")
    if not boot_to(mon, g.log_path, M["spawn_ok"], failures, 60):
        return
    if not boot_to(mon, g.log_path, M["user_ok"], failures, 60):
        return
    if not boot_to(mon, g.log_path, M["task_exit"], failures, 30):
        return
    if not boot_to(mon, g.log_path, M["task_reap"], failures, 30):
        return

    # mtest: SYS_brk и пользовательская память.
    mon.sendkey("run mtest")
    if not boot_to(mon, g.log_path, M["mem_ok"], failures, 60):
        return
    if not boot_to(mon, g.log_path, M["task_reap"], failures, 30):
        return

    # ps: все задачи завершены, остался idle.
    mon.sendkey("ps")
    if not boot_to(mon, g.log_path, M["ps_cmd"], failures, 30):
        return
    time.sleep(1.0)

    # mem ПОСЛЕ: кадры вернулись в пул (утечки дескрипторов/страниц нет).
    mon.sendkey("mem")
    if not boot_to(mon, g.log_path, M["mem_cmd"], failures, 30):
        return
    free_after = free_mib(log_text(g.log_path))
    if free_before is not None and free_after != free_before:
        failures.append(
            f"утечка кадров: free {free_before} -> {free_after} MiB")

    # Негатив: несуществующая программа — честный отказ без паники.
    mon.sendkey("run nosuch")
    if not boot_to(mon, g.log_path, M["run_neg"], failures, 30):
        return

    # Повторный запуск: реюз слота после reap.
    mon.sendkey("run hello")
    if not boot_to(mon, g.log_path, M["user_ok"], failures, 60):
        return
    if not boot_to(mon, g.log_path, M["task_reap"], failures, 30):
        return
    time.sleep(1.0)

    mon.sendkey("poweroff")
    if not g.wait_exit(120):
        failures.append("poweroff сценария userspace не завершил QEMU")
    mon.close()




def scenario_7_install(g: Guest, failures: list) -> None:
    """FND-3b: системный установщик + загрузка установленной системы.

    Фаза A: два диска (esp.img — установочный носитель, blank.img — цель).
    В окне boot manager жмём I -> мастер: Enter (welcome), y (лицензия),
    Enter (выбор целевого диска), e (создать ESP), c Enter (создать том
    данных на всё свободное место), Enter (установить сюда), y (подтвер-
    ждение), ожидание копирования, Enter (перезагрузка) -> QEMU выходит.
    HOST CHECK: целевой образ разбирается с хоста (scripts/verify_fnd3b.py).
    Фаза B: загрузка С УСТАНОВЛЕННОГО диска (target первым) со свежим
    varstore: setup.cfg с целевого ESP, Shift -> kernel console, storage
    (том данных смонтирован), ls/cat, run hello (user ELF с тома данных).
    """
    mon = g.monitor()

    # --- Фаза A: установка -------------------------------------------
    if not boot_to(mon, g.log_path, M["bootmgr"], failures, 120):
        return
    # I в окне boot manager (1 с) — как ESC-сценарий, с повторами.
    # Альтернативный легитимный путь: при ЧИСТОМ varstore и наличии целевого
    # диска загрузчик сам запускает установщик (fresh media) — маркер
    # setup_fresh; это делает сценарий самодостаточным при запуске
    # `--scenario install`/`--scenarios install` без сценария oobe.
    deadline = time.time() + 90
    pressed = False
    while time.time() < deadline:
        mon.cmd("sendkey i")
        if (M["bm_install"] in log_text(g.log_path)
                or M["setup_fresh"] in log_text(g.log_path)):
            pressed = True
            break
        time.sleep(0.3)
    if not pressed:
        failures.append("I в boot manager не сработал (нет маркера install/fresh)")
        return
    if not boot_to(mon, g.log_path, M["setup_welcome"], failures, 60):
        return
    time.sleep(0.5)
    mon.sendkey("")             # Enter: welcome -> license
    if not boot_to(mon, g.log_path, M["setup_license"], failures, 60):
        return
    time.sleep(0.5)
    mon.sendkey("y")            # лицензия принята
    if not boot_to(mon, g.log_path, M["setup_disks"], failures, 60):
        return
    time.sleep(0.5)
    mon.sendkey("")             # Enter: первый не-boot диск (цель)
    if not boot_to(mon, g.log_path, M["setup_drives"], failures, 60):
        return
    time.sleep(0.5)
    mon.sendkey("e")            # создать ESP (64 МиБ)
    if not boot_to(mon, g.log_path, M["setup_esp"], failures, 120):
        return
    time.sleep(0.5)
    mon.sendkey("c")            # создать том данных: экран размера
    time.sleep(1.0)
    mon.sendkey("")             # Enter: размер по умолчанию (всё место)
    # Том создаётся и сразу форматируется; маркер «phase=create ok»
    # печатается на КАЖДЫЙ созданный раздел (ESP, затем данные) —
    # ждём второе появление.
    if not boot_to_count(mon, g.log_path, M["setup_esp"], 2, failures, 180):
        return
    time.sleep(0.5)
    mon.sendkey("")             # Enter: выбранный том -> подтверждение
    if not boot_to(mon, g.log_path, M["setup_confirm"], failures, 60):
        return
    time.sleep(0.5)
    # «y» БЕЗ хвостового Enter (sendkey добавляет ret сам): буферизованный
    # Enter иначе сам ответит на экран done раньше сценария — гонка.
    mon.raw("sendkey y")        # установка!
    if not boot_to(mon, g.log_path, M["setup_fmt_esp"], failures, 300):
        return
    if not boot_to(mon, g.log_path, M["setup_fmt_dat"], failures, 300):
        return
    if not boot_to(mon, g.log_path, M["setup_cfg"], failures, 300):
        return
    if not boot_to(mon, g.log_path, M["setup_copyesp"], failures, 300):
        return
    if not boot_to(mon, g.log_path, M["setup_done"], failures, 300):
        return
    # setup.cfg обязан попасть на целевой ESP (путь have_cfg без OOBE).
    if not boot_to(mon, g.log_path, M["setup_target"], failures, 60):
        return
    print("qemu_selftest: installer completed; target written")
    time.sleep(0.5)
    mon.sendkey("")             # Enter: перезагрузка (cold reset)
    if not g.wait_exit(60):
        failures.append("перезагрузка после установки не завершила QEMU")
        return
    mon.close()

    # --- HOST CHECK: разбор целевого образа с хоста ------------------
    target = g.extra_drives[0]
    try:
        out = subprocess.run(
            ["python3", "scripts/verify_fnd3b.py", "--target", target,
             "--boot", "build/BOOTX64.efi", "--kernel", "build/kernel.elf",
             "--media", g.esp],
            capture_output=True, text=True, timeout=120)
        print(out.stdout.strip())
        if out.returncode != 0:
            failures.append("HOST CHECK установленного образа провалился: "
                            + (out.stderr or out.stdout)[-400:])
            return
    except Exception as e:  # noqa: BLE001
        failures.append(f"verify_fnd3b не запустился: {e}")
        return

    # --- Фаза B: загрузка с установленного диска ----------------------
    vars_b = os.path.join(g.tmp, "vars-install-b.fd")
    shutil.copyfile(Guest.ovmf_vars, vars_b)
    g_b = Guest(g.qemu, target, vars_b, g.log_path, g.tmp,
                g.name + "-b")
    g_b.start()
    mon = g_b.monitor()
    if not boot_to(mon, g.log_path, M["bootmgr"], failures, 120):
        return
    # Конфиг обязан читаться с ЦЕЛЕВОГО ESP (setup.cfg записан установщиком).
    if not boot_to(mon, g.log_path, M["setup_found"], failures, 60):
        return
    deadline = time.time() + 30
    while time.time() < deadline:
        mon.raw("sendkey shift 250")
        # Выход по ЛЮБОМУ из двух механизмов: иначе, если Shift поймал
        # splash ядра (путь «б»), цикл давит всю отведённую секунду/30 с и
        # очередь монитора (~75 команд по 250 мс при шаге 0.4 с) задерживает
        # следующие нажатия на десятки секунд.
        if (M["bm_shift"] in log_text(g.log_path)
                or M["shift_console"] in log_text(g.log_path)):
            break
        # 0.4 с, а не 0.1: шаг 0.1 при удержании 250 мс заливал очередь
        # монитора/8042 перекрывающимися make+break (в гостевой kbd-ring
        # попадали сотни shift-событий перед полезным вводом — при TCG-
        # starvation шелл обрабатывал их десятки секунд). 0.4 с > удержания
        # — очередь между нажатиями успевает опустеть, окно 1 с (в стен-)
        # времени под нагрузкой растягивается) по-прежнему ловит 1-2 нажатия.
        time.sleep(0.4)
    if not boot_to(mon, g.log_path, M["shift_console"], failures, 150):
        return
    if not boot_to(mon, g.log_path, M["done"], failures, 150):
        return
    # Чистка консоли: make+break Shift (снятие залипшего модификатора) и
    # пустая строка (сброс частичного ввода) перед первой командой.
    console_drain(mon)
    # Том данных (второй раздел MBR, 0x0C) смонтирован ядром.
    # Ретрай с дренажом: под нагрузкой TCG посылка может частично осесть
    # в очереди 8042 до readiness шелла; пустая строка между попытками
    # исполняет обрывок как unknown command и возвращает приглашение.
    storage_ok = False
    for _ in range(3):
        mon.sendkey("storage")
        # 45 с, а не 20: при TCG-starvation гость может замереть на ~60 с
        # и обработать очередь ввода одним бурстом (наблюдалось: три ответа
        # storage: ahci=1 fat=1 amnesia=0 в serial-логе через ~60 с после
        # отправки). 3×45 с — bounded и покрывает наблюденную задержку.
        if wait_ci(g.log_path, M["storage_ok"], 45):
            storage_ok = True
            break
        mon.raw("sendkey ret")
        time.sleep(1.0)
    if not storage_ok:
        failures.append(f"{M['storage_ok']!r} не появился за 135 с (3 попытки по 45 с)")
        return
    mon.sendkey("ls /fobos")
    if not wait_ci(g.log_path, M["ls_fobos_pre"], 60):
        failures.append(f"{M['ls_fobos_pre']!r} не появился за 60 с")
        return
    m = re.search(r"ls: path=/fobos count=(\d+)", log_text(g.log_path).lower())
    if m is None or int(m.group(1)) < 4:
        failures.append(f"ls /fobos на установленном томе: записей < 4 "
                        f"({m.group(1) if m else '?'})")
        return
    mon.sendkey("cat /fobos/NOTES.TXT")
    if not wait_ci(g.log_path, M["cat_notes"], 60):
        failures.append(f"{M['cat_notes']!r} не появился за 60 с")
        return
    # user-space программы лежат на томе данных установленной системы.
    mon.sendkey("run hello")
    if not boot_to(mon, g.log_path, M["user_ok"], failures, 60):
        return
    if not boot_to(mon, g.log_path, M["task_reap"], failures, 30):
        return
    print("qemu_selftest: boot from installed disk OK")
    time.sleep(1.0)
    mon.sendkey("poweroff")
    if not g_b.wait_exit(120):
        failures.append("poweroff фазы B не завершил QEMU")
    mon.close()

def free_mib(text: str):
    """Последнее значение free_mib из лога (mem-команда)."""
    import re
    m = re.findall(r"mem: total_mib=\d+ free_mib=(\d+)", text)
    return int(m[-1]) if m else None


def last_mouse(log_path: str):
    txt = log_text(log_path)
    matches = RE_MOUSE.findall(txt)
    if not matches:
        return None
    x, y, buttons, ok, bad = matches[-1]
    return (int(x), int(y), int(buttons), int(ok), int(bad))


def dump_log(path: str, tail: int = 60) -> None:
    lines = log_text(path).splitlines()
    print("---- serial.log (последние строки) ----")
    for line in lines[-tail:]:
        print(line)
    print("---------------------------------------")
    fw = path + ".fw.log"
    lines = log_text(fw).splitlines()
    print("---- firmware debugcon (последние строки) ----")
    for line in lines[-tail:]:
        print(line)
    print("-----------------------------------------------")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--esp", default="build/esp.img")
    ap.add_argument("--log", default="build/serial.log")
    ap.add_argument("--timeout", type=int, default=1500)
    ap.add_argument("--scenario", default=None,
                    help="запустить один сценарий: oobe|persist|esc|shift|desktop")
    ap.add_argument("--scenarios", default=None,
                    help="запустить несколько сценариев через запятую "
                         "(порядок канонический): oobe,persist,esc,...")
    a = ap.parse_args()

    qemu = find_qemu()
    ovmf_code, ovmf_vars = find_ovmf()

    os.makedirs(os.path.dirname(os.path.abspath(a.log)), exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="fobos-qemu-")

    # varstore сценария 1: свежий шаблон; 2-4 переиспользуют его
    # (в нём уже записана FobosSetup -> проверка NV-персистентности).
    vars_run1 = os.path.join(tmp, "vars-run1.fd")
    shutil.copyfile(ovmf_vars, vars_run1)

    Guest.ovmf_code = ovmf_code
    Guest.ovmf_vars = ovmf_vars

    order = ["oobe", "persist", "esc", "shift", "desktop", "userspace",
             "install"]
    if a.scenarios:
        req = [s.strip() for s in a.scenarios.split(",") if s.strip()]
        bad = [s for s in req if s not in order]
        if bad:
            print(f"qemu_selftest: неизвестные сценарии: {', '.join(bad)} "
                  f"(допустимо: {', '.join(order)})")
            sys.exit(2)
        # Канонический порядок сохраняем независимо от порядка в списке:
        # сценарии 2-4 переиспользуют varstore сценария 1.
        order = [n for n in order if n in req]
    elif a.scenario:
        order = [a.scenario]

    t_start = time.time()
    failures = []
    g = None
    for name in order:
        print(f"qemu_selftest: === сценарий {name} ===")
        log_path = f"{a.log}.{name}"
        extra = []
        if name == "install":
            # Чистый целевой диск 384 МиБ (без таблицы разделов).
            extra = [os.path.join(tmp, "install-target.img")]
            with open(extra[0], "wb") as f:
                f.truncate(384 * 1024 * 1024)
        g = Guest(qemu, a.esp, vars_run1, log_path, tmp, name,
                  extra_drives=extra)
        try:
            g.start()
            fn = {
                "oobe": scenario_1_oobe,
                "persist": scenario_2_persist,
                "esc": scenario_3_esc_reset,
                "shift": scenario_4_shift,
                "desktop": scenario_5_desktop,
                "userspace": scenario_6_userspace,
                "install": scenario_7_install,
            }[name]
            fn(g, failures)
        finally:
            g.stop()
            try:
                g.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                g.proc.kill()
        if failures:
            break

    # Гарантированная зачистка ВСЕХ гостей (включая фазовые g_b): без неё
    # провал внутри фазы B оставлял зомби-QEMU, душивший следующие прогоны.
    leftover = Guest.stop_all()
    if leftover:
        print(f"qemu_selftest: принудительно убито гостей: {leftover}")

    duration = time.time() - t_start
    if failures:
        for msg in failures:
            print(f"qemu_selftest: FAIL: {msg}")
        if g is not None:
            g.dump_err()
            dump_log(g.log_path)
        shutil.rmtree(tmp, ignore_errors=True)
        sys.exit(1)

    print(f"qemu_selftest: PASS ({duration:.1f} с, все сценарии)")
    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
