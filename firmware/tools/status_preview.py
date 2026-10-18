#!/usr/bin/env python3
"""Status-screen design preview: renders the layout exactly as the panel can show it (1-bit glyphs, the six
panel colours) with a chosen typeface, so the design can be approved before it goes into firmware.

Usage: tools/status_preview.py <fonts-dir> <out-dir>
"""
import sys
from PIL import Image, ImageDraw, ImageFont

FONTS, OUT = sys.argv[1], sys.argv[2]
# Panel colours as the e-paper actually shows them (same values as PALETTE in www/index.html)
WHITE, BLACK, YELLOW, RED, BLUE, GREEN = (232, 232, 232), (25, 30, 33), (239, 222, 68), (178, 19, 24), (33, 87, 186), (18, 95, 32)

FAMILIES = {
    # role -> variation (named instance or axis values); "features" are OpenType features for every role
    "Inter": {"file": "Inter.ttf", "time": "Light", "date": "Regular", "value": "Medium", "label": "SemiBold"},
    "Fraunces": {"file": "Fraunces.ttf", "time": [144, 300, 50, 0], "date": [48, 400, 50, 0],
                 "value": [72, 500, 50, 0], "label": [9, 700, 0, 0]},
    "DM Serif Display": {"file": "DMSerifDisplay.ttf"},
    # Old-style figures dip below the line; lining figures keep the clock level.
    "EB Garamond": {"file": "EBGaramond.ttf", "time": "Regular", "date": "Regular", "value": "Medium",
                    "label": "SemiBold", "features": ["lnum"]},
    "Manrope": {"file": "Manrope.ttf", "time": "Light", "date": "Medium", "value": "SemiBold", "label": "Bold"},
    # Candidates to replace Manrope (preview only)
    "Outfit": {"file": "Outfit.ttf", "time": "Light", "date": "Regular", "value": "Medium"},
    "Sora": {"file": "Sora.ttf", "time": "Light", "date": "Regular", "value": "SemiBold"},
    "DM Sans": {"file": "DMSans.ttf", "time": "Light", "date": "Regular", "value": "Medium"},
    "Figtree": {"file": "Figtree.ttf", "time": "Light", "date": "Regular", "value": "Medium"},
    "Plus Jakarta Sans": {"file": "PlusJakartaSans.ttf", "time": "Light", "date": "Regular", "value": "SemiBold"},
    "Work Sans": {"file": "WorkSans.ttf", "time": "Light", "date": "Regular", "value": "Medium"},
    "Nunito Sans": {"file": "NunitoSans.ttf", "time": "Light", "date": "Regular", "value": "SemiBold"},
    "Lexend": {"file": "Lexend.ttf", "time": "Light", "date": "Regular", "value": "Medium"},
    "Bricolage Grotesque": {"file": "BricolageGrotesque.ttf", "time": "Light", "date": "Regular", "value": "Medium"},
    "JetBrains Mono": {"file": "JetBrainsMono.ttf", "time": "Light", "date": "Regular", "value": "Medium",
                       "label": "Bold"},
}
STRESS_SSIDS = ["HomeNet", "MySpectrumWiFi7c-5G", "FBI Surveillance Van #3", "DIRECT-4B-HP OfficeJet Pro 9010",
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"]


class Font:
    """A sized face plus its OpenType features (Pillow needs them passed on every call)."""

    def __init__(self, fam, role, size):
        spec = FAMILIES[fam]
        self.size, self.fam, self.role = size, fam, role
        self.f = ImageFont.truetype(f"{FONTS}/{spec['file']}", size, layout_engine=ImageFont.Layout.RAQM)
        v = spec.get(role)
        if isinstance(v, str): self.f.set_variation_by_name(v)
        elif v: self.f.set_variation_by_axes(v)
        self.feat = spec.get("features")

    def length(self, s): return self.f.getlength(s, features=self.feat)
    def bbox(self, s): return self.f.getbbox(s, features=self.feat)
    def resized(self, size): return Font(self.fam, self.role, size)


class Panel:
    """Draws 1-bit text: render in greyscale, then threshold, which is what the firmware's glyph tables hold."""

    def __init__(self, w, h):
        self.img = Image.new("RGB", (w, h), WHITE)

    def text(self, xy, s, f, color, tracking=0):
        x, y = xy
        for ch in (s if tracking else [s]):
            mask = Image.new("L", self.img.size, 0)
            ImageDraw.Draw(mask).text((x, y), ch, font=f.f, fill=255, features=f.feat)
            self.img.paste(color, mask=mask.point(lambda v: 255 if v >= 128 else 0))
            x += f.length(ch) + tracking
        return x

    def rect(self, box, color, width=0):
        d = ImageDraw.Draw(self.img)
        d.rectangle(box, fill=None if width else color, outline=color if width else None, width=width or 1)

    def poly(self, pts, fill, outline):
        ImageDraw.Draw(self.img).polygon(pts, fill=fill, outline=outline, width=2)


def width(s, f, tracking=0):
    return sum(f.length(c) + tracking for c in s) - tracking if tracking else f.length(s)


def battery(p, x, y, pct, charging):
    """48x24 battery filled to the charge; yellow bolt with a black outline when charging."""
    p.rect((x, y, x + 44, y + 23), BLACK, width=3)
    p.rect((x + 45, y + 7, x + 48, y + 16), BLACK)
    fill = round(36 * min(pct, 100) / 100)
    if fill: p.rect((x + 4, y + 4, x + 3 + fill, y + 19), RED if pct <= 15 else GREEN)
    if charging:
        cx = x + 22
        p.poly([(cx + 3, y - 6), (cx - 8, y + 14), (cx - 1, y + 14), (cx - 4, y + 30), (cx + 9, y + 8), (cx + 1, y + 8)],
               YELLOW, BLACK)


def bars(p, x, y, n):  # bottom-aligned; blue when filled, red when the signal is weak
    color = RED if n <= 2 else BLUE
    for i in range(5):
        h = 6 + i * 5
        box = (x + i * 9, y - h, x + i * 9 + 6, y)
        p.rect(box, color) if i < n else p.rect(box, BLACK, width=1)


def comfort(t_c, rh):
    """How the air feels, from temperature and dew point (dew point tracks perceived mugginess far better
    than relative humidity does). Returns (word, colour)."""
    import math
    g = math.log(rh / 100) + 17.62 * t_c / (243.12 + t_c)
    dew = 243.12 * g / (17.62 - g)
    if t_c < 17: return "Chilly", BLUE
    if t_c > 28: return "Hot", RED
    if dew < 7: return "Dry", BLUE
    if dew < 16: return ("Warm", RED) if t_c > 25 else ("Comfortable", GREEN)
    return ("Humid", RED) if dew < 19 else ("Muggy", RED)


def feels_like_f(t_f, rh):
    """NWS heat index. Below ~80 F the simple Steadman form; the full Rothfusz regression above it."""
    hi = 0.5 * (t_f + 61.0 + (t_f - 68.0) * 1.2 + rh * 0.094)
    if (hi + t_f) / 2 < 80:
        return hi
    return (-42.379 + 2.04901523 * t_f + 10.14333127 * rh - 0.22475541 * t_f * rh - 6.83783e-3 * t_f * t_f
            - 5.481717e-2 * rh * rh + 1.22874e-3 * t_f * t_f * rh + 8.5282e-4 * t_f * rh * rh - 1.99e-6 * t_f * t_f * rh * rh)


def humidity_level(rh):
    """EPA: keep indoor RH 30-50%, and below 60% to avoid mould. Returns (tag, text colour, tag fill)."""
    if rh < 30: return "DRY", WHITE, BLUE
    if rh <= 50: return "NORMAL", WHITE, GREEN
    if rh <= 60: return "ELEVATED", BLACK, YELLOW
    return "HIGH \u00b7 MOLD RISK", WHITE, RED


def tag(p, x, y, text, f, fg, bg):
    """Small filled pill-ish tag; returns the x after it."""
    w = width(text, f, 1.5)
    p.rect((x, y, x + w + 12, y + f.size + 6), bg)
    p.text((x + 6, y + 2 - f.bbox("H")[1] + 3), text, f, fg, tracking=1.5)
    return x + w + 12


def usage_bar(p, x, y, w, frac):
    """Card fill under the storage numbers; red when nearly full. At least a sliver shows when not empty."""
    p.rect((x, y, x + w, y + 12), BLACK, width=2)
    fill = max(2, round((w - 4) * frac)) if frac > 0 else 0
    if fill: p.rect((x + 2, y + 2, x + 2 + fill, y + 10), RED if frac >= 0.9 else BLUE)


def moon(p, cx, cy, r, phase):
    """phase 0..1 (0 new, .5 full). Lit part yellow, dark part black, black outline."""
    import math
    for y in range(-r, r + 1):
        for x in range(-r, r + 1):
            if x * x + y * y > r * r: continue
            # terminator: an ellipse whose x-radius follows cos(phase angle)
            k = math.cos(2 * math.pi * phase)
            half = math.sqrt(r * r - y * y)
            edge = k * half
            lit = (x > edge) if phase < 0.5 else (x < -edge)
            p.img.putpixel((cx + x, cy + y), YELLOW if lit else BLACK)
    ImageDraw.Draw(p.img).ellipse((cx - r, cy - r, cx + r, cy + r), outline=BLACK, width=2)


def sun(p, x, y, color=RED):
    """Setting sun: half disc on a horizon line with short rays."""
    import math
    d = ImageDraw.Draw(p.img)
    d.pieslice((x, y, x + 26, y + 26), 180, 360, fill=YELLOW, outline=BLACK, width=2)
    for a in (200, 240, 270, 300, 340):
        ang = math.radians(a)
        d.line((x + 13 + 17 * math.cos(ang), y + 13 + 17 * math.sin(ang), x + 13 + 23 * math.cos(ang),
                y + 13 + 23 * math.sin(ang)), fill=BLACK, width=2)
    d.line((x - 4, y + 13, x + 30, y + 13), fill=BLACK, width=2)


def fit_name(name, f, w, min_size=22):
    """Network names can be 32 characters, often without spaces. In order of preference, at each size
    from large to small: one line; two lines split at a space, - or _; two lines split mid-word. At the
    smallest size, the second line ends with an ellipsis if it still doesn't fit."""
    def split(f, natural):
        best = None
        for n in range(1, len(name)):
            if natural and name[n - 1] not in " -_":
                continue
            first, rest = name[:n].rstrip(), name[n:].lstrip()
            if width(first, f) <= w and width(rest, f) <= w:
                best = (first, rest)  # keep the latest split that fits: balances toward a full first line
        return best

    sizes = list(range(f.size, min_size - 1, -2))
    for size in sizes:  # 1) one line, shrinking no further than a size that still reads as a value
        if size >= min_size + 8 and width(name, f.resized(size)) <= w:
            return [name], f.resized(size)
    for natural in (True,):  # 2) two lines at a word break, largest size first
        for size in sizes:
            if size > min_size + 8:
                continue
            lines = split(f.resized(size), natural)
            if lines:
                return list(lines), f.resized(size)
    # 2b) no word break lets both lines fit: keep the latest word break that fits line 1, trim line 2.
    f = f.resized(min_size)
    for n in range(len(name) - 1, 0, -1):
        if name[n - 1] in " -_" and width(name[:n].rstrip(), f) <= w:
            rest = name[n:].lstrip()
            while width(rest, f) > w:
                rest = rest[:-2] + "…"
            return [name[:n].rstrip(), rest], f
    # 3) no usable word break at all (e.g. one 32-character token): mid-word, then trim.
    for size in sizes:
        if size <= min_size + 8:
            lines = split(f.resized(size), False)
            if lines:
                return list(lines), f.resized(size)
    n = len(name)
    while n > 1 and width(name[:n], f) > w:
        n -= 1
    first, rest = name[:n], name[n:]
    while width(rest, f) > w:
        rest = rest[:-2] + "…"
    return [first, rest], f


FOURTH = {
    "wifi": ("WI-FI", "HomeNet", [("good signal", BLACK)]),
    "sunset": ("SUNSET", "6:38", [("sunrise 7:24 AM", BLACK)]),
    "moon": ("MOON", "Gibbous", [("waxing, 78% lit", BLACK)]),
    "daylight": ("DAYLIGHT", "11h 14m", [("2 min shorter", BLUE)]),
    "next": ("NEXT PHOTO", "4:00", [("then every hour", BLACK)]),
}


CARDS = {
    "indoor": None, "battery": ("BATTERY", "64%", [("charging", BLACK)]),
    "photos": ("PHOTOS", "660", [("1.0 of 15.6 GB used", BLACK)]), **FOURTH}
SETS = {"Home": ["indoor", "battery", "photos", "sunset"], "Nature": ["indoor", "sunset", "moon", "daylight"],
        "Technical": ["battery", "photos", "wifi", "next"]}


def render(fam, landscape=True, ssid="HomeNet", fourth="wifi", temp_f=72, rh=41, cards=None):
    W, H = (800, 480) if landscape else (480, 800)
    p, m = Panel(W, H), 40 if landscape else 32
    fampm = Font(fam, "date", 40)
    # Largest clock that still leaves room for the meridiem within the margins (worst case "12:55").
    size = 210 if landscape else 190
    while width("12:55", Font(fam, "time", size)) + 14 + width("PM", fampm) > W - 2 * m + 4:
        size -= 6
    ftime = Font(fam, "time", size)
    fdate = Font(fam, "date", 40 if landscape else 34)
    fval = Font(fam, "value", 46)
    # Small caps labels are UI chrome: one sturdy sans for every typeface (thin serifs break up at 17 px in 1-bit).
    flab = Font("Inter", "label", 17)
    fsub = Font(fam, "date", 21)

    # Time, with the meridiem set small beside it; vertical positions use the cap height so the
    # numerals sit on a predictable baseline whatever the font's internal leading.
    asc, base = ftime.bbox("0")[1], ftime.bbox("0")[3]
    top = 18 if landscape else 105
    x = p.text((m - 4, top - asc), "3:05", ftime, BLACK)
    p.text((x + 14, top - asc + base - fampm.bbox("P")[3]), "PM", fampm, BLACK)
    y = top + (base - asc) + 34
    date = "Sunday, October 18" if landscape else "Sunday, Oct 18"
    p.text((m, y - fdate.bbox("S")[1]), date, fdate, BLUE)
    y += fdate.bbox("S")[3] - fdate.bbox("S")[1] + 34

    lab4, val4, sub4 = FOURTH[fourth]
    if fourth == "wifi": val4 = ssid
    feels = round(feels_like_f(temp_f, rh))
    cells = [("INDOOR", f"{temp_f}°", [(f"{rh}% humidity", BLACK)]), ("BATTERY", "64%", [("charging", BLACK)]),
             ("PHOTOS", "660", [("1.0 of 15.6 GB used", BLACK)]), (lab4, val4, sub4)]
    if cards:
        cells = [cells[0] if k == "indoor" else (CARDS[k][0], ssid if k == "wifi" else CARDS[k][1], CARDS[k][2]) for k in cards]
    plain = lambda sub: "".join(t for t, _ in sub)
    # Columns: sized to their content (typefaces differ a lot in width); Wi-Fi gets whatever is left,
    # since network names are the longest values.
    if landscape:
        # Size every column to its content, shrinking captions until all four fit, then share out the spare.
        avail = W - 2 * m
        while True:
            nat = [max(width(lab, flab, 2.5) + (44 if lab in ("WI-FI", "SUNSET", "MOON") else 0),
                       width(val, fval) + (62 if lab == "BATTERY" else 0) + (40 if lab in ("SUNSET", "NEXT PHOTO") else 0)
                       + (8 + width(f"feels {feels}°", fsub) if lab == "INDOOR" and abs(feels - temp_f) >= 1 else 0),
                       width(plain(sub), fsub)) + 22 for lab, val, sub in cells]
            nat = [min(n, 200) if c[0] == "WI-FI" else n for n, c in zip(nat, cells)]  # long names wrap instead
            if sum(nat) <= avail or fsub.size <= 17: break
            fsub = fsub.resized(fsub.size - 1)
        spare = (avail - sum(nat)) / 4
        widths, y = [int(n + spare) for n in nat], max(y + 30, H - 40 - 140)
        p.rect((m, y - 30, W - m, y - 29), BLACK)
        pos = [(m + sum(widths[:i]), y) for i in range(4)]
    else:
        p.rect((m, y, W - m, y + 1), BLACK)
        y += 50
        half = (W - 2 * m) // 2
        widths, pos = [half] * 4, [(m, y), (m + half, y), (m, y + 175), (m + half, y + 175)]

    for (lab, val, sub), (cx, cy), cw in zip(cells, pos, widths):
        x = p.text((cx, cy), lab, flab, BLACK, tracking=2.5)
        vy, vx = cy + 32, cx
        if lab == "BATTERY":
            battery(p, cx, vy + 8, 64, True)
            vx = cx + 62
        if lab == "SUNSET": sun(p, int(x) + 12, cy - 6)
        if lab == "MOON": moon(p, int(x) + 24, cy + 9, 11, 0.36)
        if lab == "WI-FI":
            bars(p, int(x) + 12, cy + 17, 4)
            lines, f = fit_name(val, fval, cw - 12)
            ly = vy - f.bbox("H")[1] + (0 if len(lines) == 1 else -6)
            for ln in lines:
                p.text((cx, ly), ln, f, BLACK)
                ly += round(f.size * 1.08)
        elif lab in ("SUNSET", "NEXT PHOTO", "MOON", "DAYLIGHT"):
            f = fval
            while width(val, f) > cw - 12 and f.size > 28: f = f.resized(f.size - 2)
            x2 = p.text((cx, vy - f.bbox("0")[1]), val, f, BLACK)
            if lab in ("SUNSET", "NEXT PHOTO"):  # times get their meridiem, small
                p.text((x2 + 6, vy - f.bbox("0")[1] + f.bbox("0")[3] - fsub.bbox("P")[3]), "PM", fsub, BLACK)
        else:
            x2 = p.text((vx, vy - fval.bbox("0")[1]), val, fval, BLACK)
            if lab == "INDOOR" and abs(feels - temp_f) >= 1:  # only worth saying when it differs
                p.text((x2 + 8, vy - fval.bbox("0")[1] + fval.bbox("0")[3] - fsub.bbox("f")[3]), f"feels {feels}°", fsub, BLACK)
        sx = cx
        for t, color in sub:
            sx = p.text((sx, vy + 58), t, fsub, color)
        if lab == "PHOTOS":  # bar spans the width of the numbers above it
            usage_bar(p, cx, vy + 92, int(width(plain(sub), fsub)), 1.0 / 15.6)
        if lab == "INDOOR":  # humidity level on its own line, as a coloured tag
            t, fg, bg = humidity_level(rh)
            tag(p, cx, vy + 88, t, flab, fg, bg)
    return p.img


if __name__ == "__main__":
    for fam in FAMILIES:
        for land in (True, False):
            render(fam, land, fourth="sunset").save(f"{OUT}/status_{fam.replace(' ', '')}_{'land' if land else 'portrait'}.png")
    for i, name in enumerate(STRESS_SSIDS):
        for land in (True, False):
            render("Inter", land, name).save(f"{OUT}/ssid{i}_{'land' if land else 'portrait'}.png")
    for k in FOURTH:
        render("Inter", True, fourth=k).save(f"{OUT}/fourth_{k}.png")
    for name, cards in SETS.items():
        render("Inter", True, cards=cards).save(f"{OUT}/set_{name}.png")
    for t, h in ((72, 24), (72, 41), (74, 56), (76, 68), (88, 62)):
        render("Inter", True, fourth="sunset", temp_f=t, rh=h).save(f"{OUT}/humid_{h}.png")
