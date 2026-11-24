#!/usr/bin/env python3
"""
Fobos OS — HOST CHECK установленного диска (FND-3b).

Разбирает целевой образ со стороны хоста и проверяет результат установки
побайтово:
  1. MBR: раздел 1 = 0xEF (ESP), раздел 2 = 0x0C (FAT32), сигнатура.
  2. ESP (FAT32): \\EFI\\BOOT\\BOOTX64.EFI == build/BOOTX64.efi,
     \\fobos\\KERNEL.ELF == build/kernel.elf, \\fobos\\FOBOS.CFG присутствует,
     \\fobos\\SETUP.CFG присутствует.
  3. Том данных (FAT32): \\fobos\\HELLO.ELF, \\fobos\\NOTES.TXT, \\fobos\\FOBOS.CFG.

Только чтение; структура образов не модифицируется.
"""
import argparse
import struct
import sys

SECTOR = 512


def rd16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def rd32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Fat32:
    """Минимальный читатель FAT32 (только чтение, кластеры по цепочке)."""

    def __init__(self, data, part_lba):
        self.d = data
        self.base = part_lba * SECTOR
        vbr = self._sec(0)
        if vbr[510:512] != b"\x55\xAA":
            raise ValueError("VBR: нет сигнатуры 0x55AA")
        if rd16(vbr, 11) != SECTOR:
            raise ValueError("VBR: bytes/sector != 512")
        self.spc = vbr[13]
        self.reserved = rd16(vbr, 14)
        self.nfats = vbr[16]
        self.fatsz = rd32(vbr, 36)
        self.root_clus = rd32(vbr, 44)
        self.data_lba = self.reserved + self.nfats * self.fatsz

    def _sec(self, rel):
        off = self.base + rel * SECTOR
        return self.d[off:off + SECTOR]

    def _clus(self, c):
        lba = self.data_lba + (c - 2) * self.spc
        out = b""
        for s in range(self.spc):
            out += self._sec(lba + s)
        return out

    def _fat(self, c):
        fat_rel = self.reserved + (c // (SECTOR // 4))
        sec = self._sec(fat_rel)
        return rd32(sec, (c % (SECTOR // 4)) * 4) & 0x0FFFFFFF

    def _chain(self, first):
        out = b""
        c = first
        guard = 0
        while 2 <= c < 0x0FFFFFF0 and guard < 1_000_000:
            out += self._clus(c)
            c = self._fat(c)
            guard += 1
        return out

    def list_dir(self, cluster=None):
        """[(name, attr, size, first_cluster)] корня или каталога."""
        entries = []
        raw = self._chain(self.root_clus if cluster is None else cluster)
        lfn = ""
        for i in range(0, len(raw) - 31, 32):
            e = raw[i:i + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                lfn = ""
                continue
            if e[11] == 0x0F:
                # LFN-запись: для проверки установленных файлов достаточно
                # 8.3-имён — LFN-серию пропускаем.
                continue
            name83 = e[0:11].decode("ascii", "replace")
            name = name83[0:8].rstrip() + ("." + name83[8:11].rstrip()
                                           if name83[8:11].strip() else "")
            attr = e[11]
            first = (rd16(e, 20) << 16) | rd16(e, 26)
            size = rd32(e, 28)
            entries.append((lfn or name, attr, size, first))
            lfn = ""
        return entries

    def read_file(self, path):
        """Читает файл по пути вида \\DIR\\NAME.EXT (8.3, регистр любой)."""
        comps = [c for c in path.replace("/", "\\").split("\\") if c]
        cluster = None
        entries = self.list_dir()
        for i, comp in enumerate(comps):
            hit = None
            for (name, attr, size, first) in entries:
                if name.upper() == comp.upper():
                    hit = (name, attr, size, first)
                    break
            if hit is None:
                raise FileNotFoundError(path)
            last = (i == len(comps) - 1)
            if last:
                _, _, size, first = hit
                return self._chain(first)[:size]
            cluster = hit[3]
            entries = self.list_dir(cluster)
        raise FileNotFoundError(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--boot", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--media", default="build/esp.img",
                    help="установочный носитель: с него установщик переносит "
                         "fobos.cfg; нет файла на носителе — дефолт mode=gui")
    a = ap.parse_args()

    with open(a.target, "rb") as f:
        img = f.read()

    failures = []
    ok_n = 0

    def check(name, cond):
        nonlocal ok_n
        if cond:
            ok_n += 1
            print(f"HOST CHECK PASS: {name}")
        else:
            failures.append(name)
            print(f"HOST CHECK FAIL: {name}")

    # 1. MBR.
    mbr = img[0:SECTOR]
    check("mbr signature", mbr[510:512] == b"\x55\xAA")
    types = []
    for i in range(4):
        e = mbr[0x1BE + i * 16: 0x1BE + (i + 1) * 16]
        if e[4] != 0:
            types.append((e[4], rd32(e, 8), rd32(e, 12)))
    check("mbr two partitions", len(types) == 2)
    check("partition 1 is EFI (0xEF)",
          len(types) >= 1 and types[0][0] == 0xEF)
    check("partition 2 is FAT32 (0x0C)",
          len(types) >= 2 and types[1][0] == 0x0C)
    check("partition 1 starts at LBA 2048 (1 MiB)",
          len(types) >= 1 and types[0][1] == 2048)
    check("partition 2 after partition 1",
          len(types) >= 2 and types[1][1] >= types[0][1] + types[0][2])

    # 2. ESP.
    esp = Fat32(img, types[0][1])
    esp_entries = esp.list_dir()
    names = [n.upper() for (n, _, _, _) in esp_entries]
    check("esp root has EFI and FOBOS",
          "EFI" in names and "FOBOS" in names)
    boot = esp.read_file("\\EFI\\BOOT\\BOOTX64.EFI")
    with open(a.boot, "rb") as f:
        want = f.read()
    check("esp BOOTX64.EFI matches build artifact", boot == want)
    kernel = esp.read_file("\\FOBOS\\KERNEL.ELF")
    with open(a.kernel, "rb") as f:
        want = f.read()
    check("esp KERNEL.ELF matches build artifact", kernel == want)
    cfg = esp.read_file("\\FOBOS\\FOBOS.CFG")
    # Ожидаемый режим = режим fobos.cfg НОСИТЕЛЯ (установщик переносит
    # конфиг как есть); на дистрибутивном носителе fobos.cfg нет —
    # установщик пишет дефолт (mode=gui).
    expected_mode = "gui"
    try:
        with open(a.media, "rb") as f:
            media_img = f.read()
        media_types = []
        for i in range(4):
            e = media_img[0x1BE + i * 16: 0x1BE + (i + 1) * 16]
            if e[4] != 0:
                media_types.append((e[4], rd32(e, 8), rd32(e, 12)))
        if media_types:
            media_fat = Fat32(media_img, media_types[0][1])
            mcfg = media_fat.read_file("\\FOBOS\\FOBOS.CFG")
            for line in mcfg.decode("ascii", "replace").splitlines():
                if line.strip().startswith("mode="):
                    expected_mode = line.strip()[5:].strip() or "gui"
                    break
    except (FileNotFoundError, OSError):
        pass   # конфига на носителе нет — дефолт mode=gui
    check(f"esp FOBOS.CFG present (mode={expected_mode})",
          b"mode=" + expected_mode.encode("ascii") in cfg)
    scfg = esp.read_file("\\FOBOS\\SETUP.CFG")
    check("esp SETUP.CFG present (FBS2 blob)",
          scfg[0:4] == b"FBS2")

    # 3. Том данных.
    data = Fat32(img, types[1][1])
    dentries = data.list_dir()
    dnames = [n.upper() for (n, _, _, _) in dentries]
    check("data root has FOBOS dir",
          any(n.upper() == "FOBOS" and (a2 & 0x10)
              for (n, a2, _, _) in dentries))
    hello = data.read_file("\\FOBOS\\HELLO.ELF")
    check("data HELLO.ELF is ELF64", hello[0:4] == b"\x7fELF"
          and hello[4] == 2)
    notes = data.read_file("\\FOBOS\\NOTES.TXT")
    check("data NOTES.TXT non-empty", len(notes) > 16)
    dcfg = data.read_file("\\FOBOS\\FOBOS.CFG")
    check("data FOBOS.CFG == esp FOBOS.CFG", dcfg == cfg)

    total = ok_n + len(failures)
    print(f"HOST CHECK: {ok_n}/{total} проверок пройдено")
    if failures:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
