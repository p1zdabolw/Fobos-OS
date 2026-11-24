#!/usr/bin/env python3
"""
Fobos OS — генератор загрузочного образа ESP (FAT32 + MBR) для QEMU/OVMF.

Структура:
  MBR (LBA 0), одна партиция type 0x0C (FAT32 LBA), старт LBA 2048;
  FAT32: сектор 512 Б, кластер 512 Б, reserved 32, 2 копии FAT;
  дерево каталогов:
    /EFI/BOOT/BOOTX64.EFI
    /FOBOS/KERNEL.ELF
    /FOBOS/NOTES.TXT (тестовые данные для ls/cat/explorer/viewer)
    /FOBOS/HELLO.ELF, MTEST.ELF, KECHO.ELF (user-space, FND-3a)

Имена — строгие 8.3 (LFN не требуется загрузчику; LFN появится в kernel
FAT32 в FND-4). Размер 256 МиБ => >65525 кластеров (требование FAT32).

Порядок аллокации кластеров (важно):
  2=root, 3=EFI, 4=EFI/BOOT, 5=FOBOS, 6..=файлы. Все ссылки на кластеры
  проставляются ПОСЛЕ аллокации, файлы пишутся в свои цепочки в конце.
"""
import argparse
import struct
import sys

SECTOR = 512
PART_LBA = 2048
TOTAL_SECTORS = 256 * 1024 * 1024 // SECTOR          # 524288
RESERVED = 32
NUM_FATS = 2
ROOT_CLUSTER = 2

EOC = 0x0FFFFFFF
ATTR_DIR = 0x10
ATTR_ARCH = 0x20

# фиксированная метка 2025-01-01 00:00:00 — детерминизм сборки
FTIME = 0x0000
FDATE = ((2025 - 1980) << 9) | (1 << 5) | 1


def fat_size_sectors(data_sectors: int) -> int:
    fatsz = 4096
    for _ in range(16):
        data = data_sectors - RESERVED - NUM_FATS * fatsz
        clusters = data                      # 1 кластер = 1 сектор
        need = (clusters * 4 + SECTOR - 1) // SECTOR + 1
        if need == fatsz:
            break
        fatsz = need
    return fatsz


class Fat32Image:
    def __init__(self, path: str):
        self.f = open(path, "wb+")
        self.f.truncate(TOTAL_SECTORS * SECTOR)
        self.fat_off = PART_LBA + RESERVED
        self.fatsz = fat_size_sectors(TOTAL_SECTORS - PART_LBA)
        self.data_off = self.fat_off + NUM_FATS * self.fatsz
        self.next_free = 2
        self.total_clus = TOTAL_SECTORS - PART_LBA - RESERVED - NUM_FATS * self.fatsz
        self.free_count = self.total_clus

    def cluster_sector(self, clus: int) -> int:
        return self.data_off + (clus - 2)

    def alloc_chain(self, nbytes: int) -> int:
        """Выделяет цепочку под nbytes; возвращает первый кластер."""
        n = max(1, (nbytes + SECTOR - 1) // SECTOR)
        first = self.next_free
        prev = None
        for _ in range(n):
            cur = self.next_free
            if cur > self.total_clus + 1:
                raise RuntimeError("образ переполнен")
            self.next_free += 1
            self.free_count -= 1
            if prev is not None:
                self.fat_set(prev, cur)
            prev = cur
        self.fat_set(prev, EOC)
        return first

    def fat_set(self, idx: int, value: int) -> None:
        self.f.seek((self.fat_off) * SECTOR + idx * 4)
        self.f.write(struct.pack("<I", value))
        self.f.seek((self.fat_off + self.fatsz) * SECTOR + idx * 4)
        self.f.write(struct.pack("<I", value))

    def fat_get(self, idx: int) -> int:
        self.f.seek(self.fat_off * SECTOR + idx * 4)
        return struct.unpack("<I", self.f.read(4))[0]

    def write_clus(self, clus: int, data: bytes) -> None:
        assert len(data) == SECTOR, f"кластер {clus}: {len(data)} != {SECTOR}"
        self.f.seek(self.cluster_sector(clus) * SECTOR)
        self.f.write(data)

    def write_chain(self, first: int, data: bytes) -> None:
        clus = first
        off = 0
        while off < len(data):
            chunk = data[off:off + SECTOR]
            if len(chunk) < SECTOR:
                chunk = chunk + b"\x00" * (SECTOR - len(chunk))
            self.write_clus(clus, chunk)
            off += SECTOR
            nxt = self.fat_get(clus)
            clus = nxt
        assert clus == EOC

    def dir_entry(self, name83: bytes, attr: int, clus: int, size: int) -> bytes:
        assert len(name83) == 11, name83
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        struct.pack_into("<H", e, 14, FTIME)          # crtTime
        struct.pack_into("<H", e, 16, FDATE)          # crtDate
        struct.pack_into("<H", e, 18, FDATE)          # lastAccDate
        struct.pack_into("<H", e, 20, (clus >> 16) & 0xFFFF)  # fstClusHI
        struct.pack_into("<H", e, 22, FTIME)          # wrtTime
        struct.pack_into("<H", e, 24, FDATE)          # wrtDate
        struct.pack_into("<H", e, 26, clus & 0xFFFF)  # fstClusLO
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def dot_entries(self, self_clus: int, parent_clus: int) -> bytes:
        return (self.dir_entry(b".          ", ATTR_DIR, self_clus, 0) +
                self.dir_entry(b"..         ", ATTR_DIR, parent_clus, 0))

    def pad_sector(self, data: bytes) -> bytes:
        r = len(data) % SECTOR
        if r != 0:
            data += b"\x00" * (SECTOR - r)
        return data

    def finish(self) -> None:
        self.write_mbr()
        self.write_bpb()
        self.write_fsinfo()
        self.fat_set(0, 0x0FFFFFF8)
        self.fat_set(1, EOC)

    def write_mbr(self) -> None:
        b = bytearray(SECTOR)
        part = bytearray(16)
        struct.pack_into("<B", part, 0, 0x00)
        part[1:4] = b"\xFE\xFF\xFF"
        struct.pack_into("<B", part, 4, 0x0C)         # FAT32 LBA
        part[5:8] = b"\xFE\xFF\xFF"
        struct.pack_into("<I", part, 8, PART_LBA)
        struct.pack_into("<I", part, 12, TOTAL_SECTORS - PART_LBA)
        b[0x1BE:0x1CE] = part
        b[510:512] = b"\x55\xAA"
        self.f.seek(0)
        self.f.write(b)

    def write_bpb(self) -> None:
        b = bytearray(SECTOR)
        b[0:3] = b"\xEB\x3C\x90"
        b[3:11] = b"FOBOSOS "
        struct.pack_into("<H", b, 11, SECTOR)
        b[13] = 1                                      # секторов на кластер
        struct.pack_into("<H", b, 14, RESERVED)
        b[16] = NUM_FATS
        struct.pack_into("<H", b, 17, 0)               # root entries (FAT32: 0)
        struct.pack_into("<H", b, 19, 0)               # total16
        b[21] = 0xF8                                   # media
        struct.pack_into("<H", b, 22, 0)               # fatsz16
        struct.pack_into("<H", b, 24, 63)              # spt
        struct.pack_into("<H", b, 26, 255)             # heads
        struct.pack_into("<I", b, 28, 0)               # hidden
        struct.pack_into("<I", b, 32, TOTAL_SECTORS - PART_LBA)
        struct.pack_into("<I", b, 36, self.fatsz)
        struct.pack_into("<H", b, 40, 0)               # ext flags
        struct.pack_into("<H", b, 42, 0)               # fs version
        struct.pack_into("<I", b, 44, ROOT_CLUSTER)
        struct.pack_into("<H", b, 48, 1)               # fsinfo sector
        struct.pack_into("<H", b, 50, 6)               # backup boot sector
        b[76:82] = b"FAT32   "
        b[82] = 0x80                                   # drive num
        b[71:82] = b"FOBOSDISK  "                      # метка тома (11 байт)
        for dst in (0, 6):                             # основная и резервная копии
            self.f.seek((PART_LBA + dst) * SECTOR)
            self.f.write(b)
            self.f.seek((PART_LBA + dst) * SECTOR + 510)
            self.f.write(b"\x55\xAA")

    def write_fsinfo(self) -> None:
        b = bytearray(SECTOR)
        struct.pack_into("<I", b, 0, 0x41615252)
        struct.pack_into("<I", b, 484, 0x61417272)
        struct.pack_into("<I", b, 488, self.free_count)
        struct.pack_into("<I", b, 492, self.next_free)
        struct.pack_into("<I", b, 508, 0xAA550000)
        self.f.seek((PART_LBA + 1) * SECTOR)
        self.f.write(b)


def build(esp_path: str, boot_path: str, kernel_path: str,
          user_bins: list) -> None:
    img = Fat32Image(esp_path)

    # Порядок кластеров детерминирован: root, EFI, BOOT, FOBOS, файлы.
    root_clus = img.alloc_chain(SECTOR)          # == 2
    efi_clus = img.alloc_chain(SECTOR)           # == 3
    bootdir_clus = img.alloc_chain(SECTOR)       # == 4
    fobos_clus = img.alloc_chain(SECTOR)         # == 5
    assert root_clus == ROOT_CLUSTER

    with open(boot_path, "rb") as f:
        boot_bin = f.read()
    with open(kernel_path, "rb") as f:
        kernel_bin = f.read()

    notes_bin = (
        b"Fobos OS - session notes\r\n"
        b"------------------------\r\n"
        b"This file lives on the FAT32 volume of the boot disk.\r\n"
        b"Use kshell 'ls /fobos' and 'cat /fobos/NOTES.TXT'\r\n"
        b"or the Explorer window to read it.\r\n"
        b"\r\n"
        b"Try: fos == gui --lock   - GUI desktop mode, saved to fobos.cfg\r\n"
        b"     fos == tui          - back to the text console\r\n"
        b"     amnezia on          - writes go to RAM until reboot\r\n"
        b"     storage             - AHCI/FAT32/amnesia status\r\n"
        b"     run hello           - start a user-space program (FND-3a)\r\n")
    notes_clus = img.alloc_chain(len(notes_bin))
    boot_clus = img.alloc_chain(len(boot_bin))
    kernel_clus = img.alloc_chain(len(kernel_bin))

    # user-программы: (name83, data); имена — строгие 8.3.
    user_entries = []
    for path, name83 in user_bins:
        with open(path, "rb") as f:
            data = f.read()
        assert len(name83) == 11, name83
        clus = img.alloc_chain(len(data))
        user_entries.append((name83, clus, len(data), data))

    # /EFI/BOOT/BOOTX64.EFI
    img.write_chain(bootdir_clus, img.pad_sector(
        img.dot_entries(bootdir_clus, efi_clus) +
        img.dir_entry(b"BOOTX64 EFI", ATTR_ARCH, boot_clus, len(boot_bin))))
    # /EFI
    img.write_chain(efi_clus, img.pad_sector(
        img.dot_entries(efi_clus, ROOT_CLUSTER) +
        img.dir_entry(b"BOOT       ", ATTR_DIR, bootdir_clus, 0)))
    # /FOBOS: KERNEL.ELF + NOTES.TXT + user-программы
    fobos_content = (img.dot_entries(fobos_clus, ROOT_CLUSTER) +
                     img.dir_entry(b"KERNEL  ELF", ATTR_ARCH, kernel_clus,
                                   len(kernel_bin)) +
                     img.dir_entry(b"NOTES   TXT", ATTR_ARCH, notes_clus,
                                   len(notes_bin)))
    for name83, clus, size, _ in user_entries:
        fobos_content += img.dir_entry(name83, ATTR_ARCH, clus, size)
    img.write_chain(fobos_clus, img.pad_sector(fobos_content))
    # корень
    img.write_chain(ROOT_CLUSTER, img.pad_sector(
        img.dir_entry(b"EFI        ", ATTR_DIR, efi_clus, 0) +
        img.dir_entry(b"FOBOS      ", ATTR_DIR, fobos_clus, 0)))
    # данные файлов
    img.write_chain(notes_clus, notes_bin)
    img.write_chain(boot_clus, boot_bin)
    img.write_chain(kernel_clus, kernel_bin)
    for _, clus, _, data in user_entries:
        img.write_chain(clus, data)

    img.finish()
    users_str = " ".join(n.decode("ascii") for n, _, _, _ in user_entries) or "-"
    print(f"mkimg: {esp_path}: FAT32 ok; BOOTX64.EFI={len(boot_bin)} Б, "
          f"kernel.elf={len(kernel_bin)} Б, user: {users_str}; "
          f"кластеров {img.total_clus}, свободно {img.free_count}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--boot", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--user", action="append", default=[],
                    metavar="PATH=NAME.EXT",
                    help="user-программа: путь к ELF и 8.3-имя в \\FOBOS")
    a = ap.parse_args()

    user_bins = []
    for spec in a.user:
        path, name = spec.rsplit("=", 1)
        # 8.3-паддинг: "HELLO.ELF" -> b"HELLO   ELF" (имя до точки,
        # расширение после; усечение до 8/3).
        if "." in name:
            base, ext = name.split(".", 1)
        else:
            base, ext = name, ""
        name83 = (base[:8].ljust(8) + ext[:3].ljust(3)).encode("ascii")
        user_bins.append((path, name83))
    build(a.out, a.boot, a.kernel, user_bins)


if __name__ == "__main__":
    main()
