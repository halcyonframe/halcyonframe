#!/usr/bin/env python3
"""Generates main/typefaces.c: 1-bit glyph tables for the status screen's typefaces, rasterised exactly like
tools/status_preview.py (greyscale render, threshold at 50%), so the frame matches the approved previews.

Usage: tools/ttf2c.py <fonts-dir> > main/typefaces.c        (status-screen typefaces)
       tools/ttf2c.py <fonts-dir> --ui > main/uifonts.c     (name, title and instruction fonts of the setup screens)
The second output has its own bitmap array, so adding a font never changes the first file's bytes.
Fonts are OFL-licensed and come from github.com/google/fonts (see tools/fonts/README.md).
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONTS = sys.argv[1]
UI = "--ui" in sys.argv
TEXT = [chr(c) for c in range(32, 127)] + ["°", "·", "…"]  # ASCII + degree, middle dot, ellipsis
CLOCK = list("0123456789:")
AMPM = list("AMP")

# (C name, style shown in the web UI, file, roles). Roles are named instances or variation-axis values.
FACES = [
    ("inter", "Clean", "Inter", "Inter.ttf", {"time": "Light", "date": "Regular", "value": "Medium"}),
    ("fraunces", "Warm", "Fraunces", "Fraunces.ttf",
     {"time": [144, 300, 50, 0], "date": [48, 400, 50, 0], "value": [72, 500, 50, 0]}),
    ("dmserif", "Dramatic", "DM Serif Display", "DMSerifDisplay.ttf", {}),
    # Old-style figures dip below the line; lining figures keep the clock level.
    ("garamond", "Classic", "EB Garamond", "EBGaramond.ttf",
     {"time": "Regular", "date": "Regular", "value": "Medium", "features": ["lnum"]}),
    ("outfit", "Friendly", "Outfit", "Outfit.ttf", {"time": "Light", "date": "Regular", "value": "Medium"}),
    ("figtree", "Modern", "Figtree", "Figtree.ttf", {"time": "Light", "date": "Regular", "value": "Medium"}),
    ("jetbrains", "Technical", "JetBrains Mono", "JetBrainsMono.ttf",
     {"time": "Light", "date": "Regular", "value": "Medium"}),
]
VALUE_SIZES = [46, 36, 28, 22]  # big figures; long values (network names) step down
SUB_SIZES = [21, 19, 17]        # captions; step down when four columns don't fit
UI_BODY_SIZES = [26, 22]        # setup-screen instruction lines, largest first (the layout steps down to fit)
UI_BIG_SIZES = [52, 40]         # names and addresses to join or type


def load(file, roles, role, size):
    f = ImageFont.truetype(f"{FONTS}/{file}", size, layout_engine=ImageFont.Layout.RAQM)
    v = roles.get(role)
    if isinstance(v, str): f.set_variation_by_name(v)
    elif v: f.set_variation_by_axes(v)
    return f


def clock_size(file, roles, width, margin):
    """Largest clock where "12:55", a gap and "PM" fit between the margins (same rule as the preview)."""
    feat, ampm = roles.get("features"), load(file, roles, "date", 40)
    size = 210 if width == 800 else 190
    while (load(file, roles, "time", size).getlength("12:55", features=feat) + 14
           + ampm.getlength("PM", features=feat) > width - 2 * margin + 4):
        size -= 6
    return size


out_bits, out_lines = [], []
emit = out_lines.append  # glyph tables are printed after the bitmap array they index into


def emit_font(name, font, feat, chars):
    """Glyph = bounding box of its set pixels, offsets relative to pen position and baseline."""
    asc, desc = font.getmetrics()
    glyphs = []
    for ch in chars:
        pad = font.size
        img = Image.new("L", (font.size * 3, font.size * 3), 0)
        ImageDraw.Draw(img).text((pad, pad + asc), ch, font=font, fill=255, anchor="ls", features=feat)
        img = img.point(lambda v: 255 if v >= 128 else 0)
        box = img.getbbox()
        adv = round(font.getlength(ch, features=feat))
        if not box:
            glyphs.append((ord(ch), 0, 0, 0, 0, adv, len(out_bits)))
            continue
        x0, y0, x1, y1 = box
        w, h = x1 - x0, y1 - y0
        off = len(out_bits)
        for y in range(y0, y1):  # rows padded to whole bytes, MSB = leftmost pixel
            for bx in range(0, w, 8):
                b = 0
                for i in range(8):
                    if bx + i < w and img.getpixel((x0 + bx + i, y)):
                        b |= 0x80 >> i
                out_bits.append(b)
        glyphs.append((ord(ch), w, h, x0 - pad, y0 - (pad + asc), adv, off))
    emit(f"static const pglyph_t {name}_g[] = {{")
    for cp, w, h, x, y, adv, off in glyphs:
        emit(f"    {{0x{cp:04x}, {w}, {h}, {x}, {y}, {adv}, {off}}},")
    emit("};")
    own = ", UI_GLYPH_BITS" if UI else ""  # the UI fonts' bitmaps live in their own array
    emit(f"static const pfont_t {name} = {{{font.size}, {asc}, {desc}, {len(glyphs)}, {name}_g{own}}};")


def ui_main():
    """Name and title as the website sets them (Fraunces: the name like its header, weight 400 with the optical
    size following the point size; titles like its headings, softened), instructions in Inter, which stays
    legible at this size in 1-bit."""
    fr = {"name": [40, 400, 0, 0], "title": [34, 400, 50, 0]}
    emit_font("brand_name", load("Fraunces.ttf", fr, "name", 40), None, TEXT)
    emit_font("brand_title", load("Fraunces.ttf", fr, "title", 34), None, TEXT)
    for s in UI_BODY_SIZES:
        emit_font(f"ui_body{s}", load("Inter.ttf", {"b": "Medium"}, "b", s), None, TEXT)
    for s in UI_BIG_SIZES:
        emit_font(f"ui_big{s}", load("Inter.ttf", {"b": "Medium"}, "b", s), None, TEXT)
    emit("const pfont_t *const BRAND_NAME = &brand_name;")
    emit("const pfont_t *const BRAND_TITLE = &brand_title;")
    emit("const pfont_t *const UI_BODY[UI_BODY_SIZES_N] = {" + ", ".join(f"&ui_body{s}" for s in UI_BODY_SIZES) + "};")
    emit("const pfont_t *const UI_BIG[UI_BIG_SIZES_N] = {" + ", ".join(f"&ui_big{s}" for s in UI_BIG_SIZES) + "};")
    print("// Generated by tools/ttf2c.py --ui. Do not edit. Typefaces: SIL Open Font License 1.1 (tools/fonts/).")
    print('#include "typefaces.h"\n')
    print(f"static const uint8_t UI_GLYPH_BITS[] = {{  // {len(out_bits)} bytes")
    for i in range(0, len(out_bits), 24):
        print("    " + ",".join(f"0x{b:02x}" for b in out_bits[i:i + 24]) + ",")
    print("};\n")
    print("\n".join(out_lines))
    print(f"generated {len(out_bits)} bytes of glyph bitmaps", file=sys.stderr)


def main():
    if UI:
        return ui_main()
    faces = []
    for cname, style, label, file, roles in FACES:
        feat = roles.get("features")
        tl, tp = clock_size(file, roles, 800, 40), clock_size(file, roles, 480, 32)
        emit_font(f"{cname}_time_l", load(file, roles, "time", tl), feat, CLOCK)
        emit_font(f"{cname}_time_p", load(file, roles, "time", tp), feat, CLOCK)
        emit_font(f"{cname}_ampm", load(file, roles, "date", 40), feat, AMPM)
        emit_font(f"{cname}_date_l", load(file, roles, "date", 40), feat, TEXT)
        emit_font(f"{cname}_date_p", load(file, roles, "date", 34), feat, TEXT)
        for s in VALUE_SIZES:
            emit_font(f"{cname}_value{s}", load(file, roles, "value", s), feat, TEXT)
        for s in SUB_SIZES:
            emit_font(f"{cname}_sub{s}", load(file, roles, "date", s), feat, TEXT)
        faces.append((cname, style, label))
    # Labels and tags: one sturdy sans for every typeface (thin serifs break up at this size in 1-bit).
    emit_font("label", load("Inter.ttf", {"label": "SemiBold"}, "label", 17), None, TEXT)
    emit("\nconst typeface_t TYPEFACES[TYPEFACE_COUNT] = {")
    for cname, style, label in faces:
        vals = ", ".join(f"&{cname}_value{s}" for s in VALUE_SIZES)
        subs = ", ".join(f"&{cname}_sub{s}" for s in SUB_SIZES)
        emit(f'    {{"{style}", "{label}", &{cname}_time_l, &{cname}_time_p, &{cname}_ampm, &{cname}_date_l, '
             f"&{cname}_date_p, {{{vals}}}, {{{subs}}}}},")
    emit("};")
    emit("const pfont_t *const LABEL_FONT = &label;")

    print("// Generated by tools/ttf2c.py. Do not edit. Typefaces: SIL Open Font License 1.1 (tools/fonts/).")
    print('#include "typefaces.h"\n')
    print(f"const uint8_t GLYPH_BITS[] = {{  // {len(out_bits)} bytes")
    for i in range(0, len(out_bits), 24):
        print("    " + ",".join(f"0x{b:02x}" for b in out_bits[i:i + 24]) + ",")
    print("};\n")
    print("\n".join(out_lines))
    print(f"generated {len(out_bits)} bytes of glyph bitmaps", file=sys.stderr)


main()
