#!/usr/bin/env python3
"""
Fobos OS — проверка раскладки ELF-образа ядра (FBPI v0).

Гарантирует, что ядро собрано ровно по контракту загрузчика:
  - e_entry == 0xFFFFFFFF80100000 (KBASE + KPHYS), _start — первый байт;
  - каждый PT_LOAD: vaddr - paddr == KBASE, p_memsz >= p_filesz;
  - весь образ внутри окна [1 МиБ, 4 ГиБ) (identity + higher-half 1 ГиБ).
Вызов выполняется автоматически после каждой линковки ядра.
"""
import struct
import sys

KBASE = 0xFFFFFFFF80000000
KPHYS = 0x200000
KPHYS_MAX = 0x40000000

PT_LOAD = 1


def fail(msg: str) -> None:
    print(f"check_kernel_elf: FATAL: {msg}")
    sys.exit(1)


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <kernel.elf>")
        sys.exit(2)

    with open(sys.argv[1], "rb") as f:
        data = f.read()

    if len(data) < 64 or data[:4] != b"\x7fELF":
        fail("не ELF")
    if data[4] != 2 or data[5] != 1:
        fail("ожидается ELF64 little-endian")
    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    e_entry = struct.unpack_from("<Q", data, 24)[0]
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 54)

    if e_type != 2:
        fail("e_type != ET_EXEC")
    if e_machine != 62:
        fail("e_machine != EM_X86_64")
    if e_entry != KBASE + KPHYS:
        fail(f"e_entry = 0x{e_entry:x}, ожидалось 0x{KBASE + KPHYS:x}")
    if e_phentsize != 56 or e_phnum == 0:
        fail("повреждённая таблица программных заголовков")

    loads = 0
    min_paddr = None
    max_end = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        p_vaddr = struct.unpack_from("<Q", data, off + 16)[0]
        p_paddr = struct.unpack_from("<Q", data, off + 24)[0]
        p_filesz = struct.unpack_from("<Q", data, off + 32)[0]
        p_memsz = struct.unpack_from("<Q", data, off + 40)[0]

        if p_type != PT_LOAD:
            continue
        loads += 1
        if p_vaddr - p_paddr != KBASE:
            fail(f"PT_LOAD #{i}: vaddr - paddr != KBASE")
        if p_memsz < p_filesz:
            fail(f"PT_LOAD #{i}: memsz < filesz")
        if min_paddr is None or p_paddr < min_paddr:
            min_paddr = p_paddr
        if p_paddr + p_memsz > max_end:
            max_end = p_paddr + p_memsz

    if loads == 0:
        fail("нет PT_LOAD-сегментов")
    if min_paddr != KPHYS:
        fail(f"физическая база ядра = 0x{min_paddr:x}, ожидалось 0x{KPHYS:x}")
    if max_end > KPHYS_MAX:
        fail(f"конец образа 0x{max_end:x} вне окна 4 ГиБ")

    print(f"check_kernel_elf: OK (entry=0x{e_entry:x}, span=0x{min_paddr:x}..0x{max_end:x},"
          f" {loads} PT_LOAD)")


if __name__ == "__main__":
    main()
