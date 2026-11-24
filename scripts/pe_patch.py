#!/usr/bin/env python3
"""
Fobos OS — пост-обработка PE32+ образа UEFI-загрузчика.

GNU ld (эмуляция i386pep) не умеет выставлять Subsystem=EFI_APPLICATION,
поэтому после линковки патчим заголовок напрямую и печатаем сводку образа.
Также проверяем наличие .reloc: без базовых релокаций прошивка обязана
загрузить образ строго по ImageBase — ошибка загрузки будет видна сразу.
"""
import struct
import sys

EFI_APPLICATION = 10


def fail(msg: str) -> None:
    print(f"pe_patch: FATAL: {msg}")
    sys.exit(1)


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <BOOTX64.efi>")
        sys.exit(2)

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())

    if len(data) < 0x40:
        fail("файл слишком мал для PE")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        fail("нет PE-сигнатуры")

    coff = e_lfanew + 4
    num_sections = struct.unpack_from("<H", data, coff + 2)[0]
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    if opt_size < 112:
        fail("OptionalHeader меньше PE32+")
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic != 0x20B:
        fail("ожидается PE32+ (magic 0x20B)")

    entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    subsystem_off = opt + 68
    subsystem_old = struct.unpack_from("<H", data, subsystem_off)[0]

    struct.pack_into("<H", data, subsystem_off, EFI_APPLICATION)
    with open(path, "wb") as f:
        f.write(data)

    sect = opt + opt_size
    names = []
    has_reloc = False
    for i in range(num_sections):
        off = sect + i * 40
        name = data[off:off + 8].rstrip(b"\x00").decode("ascii", "replace")
        names.append(name)
        if name == ".reloc":
            has_reloc = True

    print("pe_patch: BOOTX64.efi сводка:")
    print(f"  entry RVA     : 0x{entry_rva:08X}")
    print(f"  ImageBase     : 0x{image_base:016X}")
    print(f"  Subsystem     : {subsystem_old} -> {EFI_APPLICATION} (EFI_APPLICATION)")
    print(f"  sections      : {', '.join(names)}")
    if not has_reloc:
        print("  ПРЕДУПРЕЖДЕНИЕ: .reloc отсутствует — образ не перемещаемый;"
              " OVMF обязан загрузить его строго по ImageBase")

    with open(path, "rb") as f:
        check = f.read()
    if struct.unpack_from("<H", check, subsystem_off)[0] != EFI_APPLICATION:
        fail("Subsystem не применился")


if __name__ == "__main__":
    main()
