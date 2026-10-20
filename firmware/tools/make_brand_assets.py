#!/usr/bin/env python3
"""Generates the web page's brand files in main/www/brand/ from docs/mark.svg's geometry and Fraunces:
icons for browsers and phone home screens, and a tiny Fraunces subset for the "Halcyon Frame" wordmark.

Usage: tools/make_brand_assets.py <fonts-dir>     (needs Pillow, fonttools and brotli)
"""
import io
import sys
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from PIL import Image, ImageDraw

FONTS = Path(sys.argv[1])
OUT = Path(__file__).resolve().parent.parent / "main" / "www" / "brand"
OUT.mkdir(parents=True, exist_ok=True)
INK, SUN, BLUE, PAPER = (25, 30, 33), (239, 222, 68), (33, 87, 186), (246, 246, 244)


def mark(size, ss=8):
    """The mark (docs/mark.svg, 100x100 units) on a transparent background, anti-aliased by supersampling."""
    S = size * ss
    u = S / 100
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.pieslice([28 * u, 36 * u, 72 * u, 80 * u], 180, 360, fill=SUN)
    # SVG strokes are centred on the path and Pillow draws them inward from the box, so the boxes are the outer edges
    d.rounded_rectangle([2.5 * u, 2.5 * u, 97.5 * u, 97.5 * u], radius=17.5 * u, outline=INK, width=round(7 * u))
    d.arc([26.5 * u, 34.5 * u, 73.5 * u, 81.5 * u], 180, 360, fill=INK, width=round(3 * u))
    d.line([28 * u, 58 * u, 72 * u, 58 * u], fill=INK, width=round(3 * u))
    for y, a, b in ((62, 18, 82), (72, 28, 72), (82, 38, 62)):
        d.line([a * u, y * u, b * u, y * u], fill=BLUE, width=round(6 * u))
        for x in (a, b):
            d.ellipse([(x - 3) * u, (y - 3) * u, (x + 3) * u, (y + 3) * u], fill=BLUE)
    return im.resize((size, size), Image.LANCZOS)


def tile(size, mark_frac, radius_frac):
    """The mark centred on a paper-coloured tile (square corners when radius_frac is 0)."""
    ss = 4
    S = size * ss
    bg = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(bg).rounded_rectangle([0, 0, S - 1, S - 1], radius=radius_frac * S, fill=PAPER)
    m = mark(round(size * mark_frac), 8).resize((round(S * mark_frac),) * 2, Image.LANCZOS)
    bg.alpha_composite(m, ((S - m.width) // 2, (S - m.height) // 2))
    return bg.resize((size, size), Image.LANCZOS)


# Opaque where the system draws its own mask (iOS rounds the touch icon, maskable icons are cropped to any shape),
# a rounded tile for the plain icons so the dark outline reads on a dark launcher too.
tile(180, 0.70, 0).convert("RGB").save(OUT / "apple-touch-icon.png", optimize=True)
tile(192, 0.72, 0.22).save(OUT / "icon-192.png", optimize=True)
tile(512, 0.72, 0.22).save(OUT / "icon-512.png", optimize=True)
tile(512, 0.56, 0).convert("RGB").save(OUT / "icon-maskable-512.png", optimize=True)  # mark inside the 80% safe zone
mark(48).save(OUT / "favicon.png", optimize=True)  # served at /favicon.ico

# The wordmark: Fraunces as the website's header sets it (weight 400, optical size following the size, no softening),
# reduced to the letters of "Halcyon Frame".
vf = TTFont(FONTS / "Fraunces.ttf")
static = instancer.instantiateVariableFont(vf, {"opsz": 24, "wght": 400, "SOFT": 0, "WONK": 0})
opts = subset.Options()
opts.flavor = "woff2"
opts.layout_features = ["kern", "liga"]
opts.name_IDs = []
opts.notdef_outline = True
ss = subset.Subsetter(opts)
ss.populate(text="Halcyon Frame")
ss.subset(static)
buf = io.BytesIO()
static.flavor = "woff2"
static.save(buf)
(OUT / "fraunces-brand.woff2").write_bytes(buf.getvalue())

(OUT / "icon.svg").write_text(
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"><title>Halcyon Frame</title>'
    '<rect x="6" y="6" width="88" height="88" rx="14" fill="none" stroke="#191e21" stroke-width="7"/>'
    '<path d="M28 58a22 22 0 0 1 44 0z" fill="#efde44" stroke="#191e21" stroke-width="3"/>'
    '<path d="M18 62h64M28 72h44M38 82h24" stroke="#2157ba" stroke-width="6" stroke-linecap="round"/></svg>\n')
for f in sorted(OUT.iterdir()):
    print(f"{f.name:24s} {f.stat().st_size:6d} bytes")
