#!/usr/bin/env python3
"""Хост-диагностика: содержимое \\fobos\\fobos.cfg в build/esp.img.

Разбирает MBR -> раздел FAT32 -> BPB -> каталог \fobos -> цепочку кластеров
FOBOS.CFG и печатает содержимое. Цель: проверить, что реально на диске
после записи из ядра (`fos == gui --lock`).
"""
import struct
import sys

IMG = sys.argv[1] if len(sys.argv) > 1 else "build/esp.img"
SECT = 512

with open(IMG, "rb") as f:
    img = f.read()


def rd(lba, n=1):
    return img[lba * SECT:(lba + n) * SECT]


# --- MBR: первый раздел с типом 0x0B/0x0C/0xEF ---
mbr = rd(0)
part_off = None
for i in range(4):
    e = mbr[446 + 16 * i: 446 + 16 * (i + 1)]
    ptype = e[4]
    lba, cnt = struct.unpack("<II", e[8:16])
    print(f"MBR part{i}: type=0x{ptype:02x} lba={lba} sectors={cnt}")
    if ptype in (0x0B, 0x0C) and part_off is None:
        part_off = lba

if part_off is None:
    print("FAT32-раздел не найден")
    sys.exit(1)

# --- BPB ---
bpb = rd(part_off)
bytes_per_sect = struct.unpack("<H", bpb[11:13])[0]
sect_per_clus = bpb[13]
rsvd = struct.unpack("<H", bpb[14:16])[0]
nfats = bpb[16]
fatsz = struct.unpack("<I", bpb[36:40])[0]
root_clus = struct.unpack("<I", bpb[44:48])[0]
assert bytes_per_sect == SECT, bytes_per_sect
fat_lba = part_off + rsvd
data_lba = part_off + rsvd + nfats * fatsz
print(f"BPB: spc={sect_per_clus} rsvd={rsvd} nfats={nfats} fatsz={fatsz} "
      f"rootclus={root_clus} fat_lba={fat_lba} data_lba={data_lba}")


def fat_entry(clus):
    off = fat_lba * SECT + clus * 4
    return struct.unpack("<I", img[off:off + 4])[0] & 0x0FFFFFFF


def clus_lba(clus):
    return data_lba + (clus - 2) * sect_per_clus


# --- каталог \fobos ---
clus = root_clus
target = None
while 2 <= clus < 0x0FFFFFF8 and target is None:
    sec = rd(clus_lba(clus), sect_per_clus)
    for i in range(0, len(sec), 32):
        e = sec[i:i + 32]
        if e[0] == 0x00:
            break
        if e[0] == 0xE5:
            continue
        attr = e[11]
        if attr == 0x0F:
            continue  # LFN
        name = e[0:8].decode("ascii", "replace").strip() + \
            ("." + e[8:11].decode("ascii", "replace").strip()
             if e[8:11].strip() else "")
        c = (e[20] << 16) | struct.unpack("<H", e[26:28])[0]
        size = struct.unpack("<I", e[28:32])[0]
        if name.upper() == "FOBOS":
            print(f"каталог /FOBOS: clus={c}")
            sub = c
            while 2 <= sub < 0x0FFFFFF8 and target is None:
                s2 = rd(clus_lba(sub), sect_per_clus)
                for j in range(0, len(s2), 32):
                    e2 = s2[j:j + 32]
                    if e2[0] == 0x00:
                        break
                    if e2[0] == 0xE5 or e2[11] == 0x0F:
                        continue
                    n2 = e2[0:8].decode("ascii", "replace").strip() + \
                        ("." + e2[8:11].decode("ascii", "replace").strip()
                         if e2[8:11].strip() else "")
                    c2 = (e2[20] << 16) | struct.unpack("<H", e2[26:28])[0]
                    s2v = struct.unpack("<I", e2[28:32])[0]
                    if n2.upper() == "FOBOS.CFG":
                        target = (c2, s2v)
                        break
                sub = fat_entry(sub)
        elif name.upper() == "FOBOS.CFG":
            target = (c, size)
if target is None:
    print("FOBOS.CFG не найден")
    sys.exit(1)

c2, size = target
print(f"FOBOS.CFG: first_clus={c2} size={size}")
data = b""
ch = c2
n = 0
while 2 <= ch < 0x0FFFFFF8 and len(data) < size + 512:
    data += rd(clus_lba(ch), sect_per_clus)
    ch = fat_entry(ch)
    n += 1
    if n > 32:
        break
print(f"цепочка кластеров: {n}, первые байты:")
print(repr(data[:size if size < 256 else 256]))
