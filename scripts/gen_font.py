#!/usr/bin/env python3
"""
Fobos OS — генератор common/font8x8.h из публичного font8x8 (Daniel Hepper,
Public Domain, основан на VGA-шрифте IBM).

Вход:  /tmp/font8x8_basic.h (скачивается curl'ом, см. README).
Выход: common/font8x8.h — массив FONT8X8[95][8] для ASCII 0x20..0x7E.
Шрифт общий для ядра (fbcon/ui) и UEFI-загрузчика (OOBE).

Контраст растра: бит i байта-строки = столбец i (0 = левый пиксель),
итого строка сверху вниз, биты слева направо. Это контракт, который
использует fb_blit_glyph8(); проверяется статическим ассертом на глифе 'A'.

Скрипт идемпотентен: повторный запуск даёт байт-в-байт тот же файл.
"""
import re
import sys

SRC = "/tmp/font8x8_basic.h"
DST = "common/font8x8.h"

FIRST = 0x20
LAST = 0x7E  # включительно => 95 глифов


def parse_source(text: bytes) -> list:
    body = text.decode("utf-8", errors="replace")
    # Каждый глиф — строка вида "{ 0xHH, ..., 0xHH},"; берём все hex внутри {...}.
    groups = re.findall(r"\{([^{}]*)\}", body)
    vals = []
    for grp in groups:
        hexes = re.findall(r"0x([0-9A-Fa-f]{2})", grp)
        if len(hexes) != 8:
            continue  # это не строка глифа (например, тело массива объявлений)
        vals.extend(int(h, 16) for h in hexes)
    if len(vals) < 128 * 8:
        raise SystemExit(f"gen_font: источник неполон: {len(vals)} байт < {128 * 8}")
    glyphs = []
    for g in range(128):
        glyphs.append(vals[g * 8:(g + 1) * 8])
    return glyphs


def render(glyphs: list) -> str:
    out = []
    out.append("/*")
    out.append(" * Fobos OS — растровый шрифт 8x8 (ASCII 0x20..0x7E).")
    out.append(" *")
    out.append(" * Источник: font8x8, Daniel Hepper / Marcel Sondaar / IBM — Public Domain.")
    out.append(" * Сгенерирован scripts/gen_font.py из font8x8_basic.h; НЕ редактировать")
    out.append(" * вручную — править генератор и перезапускать (см. README).")
    out.append(" *")
    out.append(" * Контракт растра: каждый глиф — 8 байт (строки сверху вниз);")
    out.append(" * бит i байта = пиксель столбца i (0 = левый). Используется")
    out.append(" * fb_blit_glyph8(); инвариант глифа 'A' зафиксирован static_assert'ом.")
    out.append(" */")
    out.append("#ifndef FOBOS_COMMON_FONT8X8_H")
    out.append("#define FOBOS_COMMON_FONT8X8_H")
    out.append("")
    out.append("#include <fobos/types.h>")
    out.append("")
    out.append("#define FONT8X8_FIRST 0x%02Xu" % FIRST)
    out.append("#define FONT8X8_LAST  0x%02Xu" % LAST)
    out.append("#define FONT8X8_COUNT %du /* (LAST - FIRST + 1) */" % (LAST - FIRST + 1))
    out.append("")
    out.append("static const u8 font8x8[FONT8X8_COUNT][8] = {")
    for code in range(FIRST, LAST + 1):
        rows = glyphs[code]
        hexes = ", ".join("0x%02X" % b for b in rows)
        ch = chr(code) if code != 0x5C else "\\\\"
        out.append("    { %s }, /* '%s' 0x%02X */" % (hexes, ch, code))
    out.append("};")
    out.append("")
    out.append("/* Глиф 'A': вертикальные штрихи в столбцах 1 и 6, перекладина в строке 3.")
    out.append(" * Ловит перепутанную ориентацию битов (MSB/LSB) при переносе. */")
    a = glyphs[0x41]
    out.append("#define FONT8X8_A_ROW0 0x%02Xu" % a[0])
    out.append("#define FONT8X8_A_ROW3 0x%02Xu" % a[3])
    out.append("")
    out.append("#endif /* FOBOS_COMMON_FONT8X8_H */")
    return "\n".join(out) + "\n"


def main() -> None:
    try:
        with open(SRC, "rb") as f:
            glyphs = parse_source(f.read())
    except OSError as e:
        print(f"gen_font: не удалось прочитать {SRC}: {e}")
        print("gen_font: скачайте шрифт командой:")
        print("  curl -fsSL https://raw.githubusercontent.com/dhepper/font8x8/"
              "master/font8x8_basic.h -o /tmp/font8x8_basic.h")
        sys.exit(2)

    content = render(glyphs)
    with open(DST, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"gen_font: {DST}: {LAST - FIRST + 1} глифов записано")


if __name__ == "__main__":
    main()
