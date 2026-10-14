#!/usr/bin/env python3
import sys

if len(sys.argv) != 3:
    print("Kullanim: python tools/bdf_to_c.py input.bdf output.h")
    raise SystemExit(1)

source, destination = sys.argv[1], sys.argv[2]

glyphs = []
glyph = None
in_bitmap = False
font_ascent = 12

with open(source, "r", encoding="ascii", errors="replace") as f:
    lines = iter(f)

    for raw in lines:
        line = raw.strip()

        if line.startswith("FONT_ASCENT "):
            font_ascent = int(line.split()[1])

        elif line.startswith("STARTCHAR "):
            glyph = {
                "encoding": -1,
                "bbx": (0, 0, 0, 0),
                "bitmap": [],
            }
            in_bitmap = False

        elif glyph is not None and line.startswith("ENCODING "):
            glyph["encoding"] = int(line.split()[1])

        elif glyph is not None and line.startswith("BBX "):
            glyph["bbx"] = tuple(map(int, line.split()[1:]))

        elif glyph is not None and line == "BITMAP":
            in_bitmap = True

        elif glyph is not None and line == "ENDCHAR":
            cp = glyph["encoding"]
            width, height, xoff, yoff = glyph["bbx"]

            if cp >= 0 and width <= 8 and height <= 16:
                rows = [0] * 16
                top = font_ascent - (yoff + height)

                for sy, hexrow in enumerate(glyph["bitmap"][:height]):
                    try:
                        bits = bytes.fromhex(hexrow)
                    except ValueError:
                        continue

                    for sx in range(width):
                        bx = xoff + sx
                        by = top + sy

                        if 0 <= bx < 8 and 0 <= by < 16:
                            byte_index = sx // 8
                            bit_index = 7 - (sx % 8)

                            if byte_index < len(bits):
                                if bits[byte_index] & (1 << bit_index):
                                    rows[by] |= 1 << (7 - bx)

                glyphs.append((cp, rows))

            glyph = None
            in_bitmap = False

        elif glyph is not None and in_bitmap and line:
            glyph["bitmap"].append(line)

# Yinelenen kod noktalarını kaldır, sonra sıralı tut.
unique = {}
for cp, rows in glyphs:
    unique.setdefault(cp, rows)

glyphs = sorted(unique.items())

with open(destination, "w", encoding="utf-8", newline="\n") as out:
    out.write("/* Generated from Spleen 8x16 BDF. */\n")
    out.write("/* Spleen is licensed under BSD-2-Clause. */\n")
    out.write("#ifndef MYOS_FONT8X16_H\n#define MYOS_FONT8X16_H\n\n")
    out.write("#include <stdint.h>\n")
    out.write("#include <stddef.h>\n\n")
    out.write("struct font8x16_glyph {\n")
    out.write("    uint32_t codepoint;\n")
    out.write("    uint8_t rows[16];\n")
    out.write("};\n\n")
    out.write("static const struct font8x16_glyph font8x16_glyphs[] = {\n")

    for cp, rows in glyphs:
        values = ", ".join(f"0x{value:02X}" for value in rows)
        out.write(f"    {{ 0x{cp:X}, {{ {values} }} }},\n")

    out.write("};\n\n")
    out.write("static inline const uint8_t *font8x16_get(uint32_t cp) {\n")
    out.write("    size_t lo = 0;\n")
    out.write("    size_t hi = sizeof(font8x16_glyphs) / sizeof(font8x16_glyphs[0]);\n")
    out.write("    while (lo < hi) {\n")
    out.write("        size_t mid = lo + (hi - lo) / 2;\n")
    out.write("        if (font8x16_glyphs[mid].codepoint < cp) lo = mid + 1;\n")
    out.write("        else hi = mid;\n")
    out.write("    }\n")
    out.write("    if (lo < sizeof(font8x16_glyphs) / sizeof(font8x16_glyphs[0]) &&\n")
    out.write("        font8x16_glyphs[lo].codepoint == cp)\n")
    out.write("        return font8x16_glyphs[lo].rows;\n")
    out.write("    return (const uint8_t *)0;\n")
    out.write("}\n\n#endif\n")

print(f"{len(glyphs)} karakter yazildi: {destination}")
