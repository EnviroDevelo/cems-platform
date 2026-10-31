#!/usr/bin/env python3
"""Render an EVE co-processor command list (as the UI sends it to the BT817) to a PNG,
so screens can be reviewed without the display. An approximation: DejaVu Sans in
place of the EVE ROM fonts, flat widgets.

    UI_DUMP=build/ui ./build/tests        # writes build/ui/<screen>.eve
    tools/eve_preview.py build/ui/*.eve   # writes <screen>.png next to each

Handles what platform/ui uses: CLEAR_COLOR_RGB, COLOR_RGB, COLOR_A, LINE_WIDTH,
BEGIN(RECTS/LINES)/VERTEX2F/END, CMD_TEXT, CMD_BUTTON, CMD_KEYS, CMD_NUMBER,
CMD_PROGRESS, CMD_FGCOLOR, CMD_BGCOLOR."""
import struct, sys, os
from PIL import Image, ImageDraw, ImageFont

W, H = 1024, 600
FONT_PX = {26: 15, 27: 18, 28: 21, 29: 24, 30: 30, 31: 42}       # rough cap heights of the ROM fonts
OPT_CENTERX, OPT_CENTERY, OPT_RIGHTX, OPT_FLAT = 512, 1024, 2048, 256
FONT_FILE = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
_fonts = {}
OWN = {}                                     # bitmap handle -> ROM font it stands in for (CMD_SETFONT2)
try:                                         # codes 1..31 of our own fonts (tools/mkfont.py)
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mkfont import EXTRA, SIZES
except Exception:
    EXTRA, SIZES = [], []


def font(n):
    px = FONT_PX.get(n, 18)
    if px not in _fonts:
        try:
            _fonts[px] = ImageFont.truetype(FONT_FILE, px)
        except OSError:
            _fonts[px] = ImageFont.load_default()
    return _fonts[px]


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def render(data):
    words = len(data) // 4
    img = Image.new("RGB", (W, H), (0, 0, 0))
    over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    st = {"color": (255, 255, 255), "alpha": 255, "lw": 16, "fg": (0, 56, 112), "bg": (0, 32, 64), "prim": 0, "verts": []}
    i = 0

    def s16(off):
        return struct.unpack_from("<h", data, off)[0]

    def u16(off):
        return struct.unpack_from("<H", data, off)[0]

    def string(off):
        end = data.index(b"\0", off)
        return data[off:end].decode("latin-1"), (end - off + 4) & ~3

    def draw_text(x, y, f, opt, s, col=None):
        if f in OWN:                                          # our font: map codes 1..31 to their symbols
            f = OWN[f]
            s = "".join(chr(EXTRA[ord(c) - 1]) if 1 <= ord(c) <= len(EXTRA) else c for c in s)
        fn = font(f)
        bb = d.textbbox((0, 0), s, font=fn)
        tw, th = bb[2] - bb[0], FONT_PX.get(f, 18)
        if opt & OPT_RIGHTX:
            x -= tw
        elif opt & OPT_CENTERX:
            x -= tw // 2
        if opt & OPT_CENTERY:
            y -= th // 2 + 2
        d.text((x, y - 2), s, font=fn, fill=col or st["color"])

    def flush_prim():
        v = st["verts"]
        if st["prim"] == 9:                                   # RECTS: pairs of corners, rounded by line width
            r = max(1, st["lw"] // 16)
            for a, b in zip(v[0::2], v[1::2]):
                box = (min(a[0], b[0]) - r + 1, min(a[1], b[1]) - r + 1, max(a[0], b[0]) + r, max(a[1], b[1]) + r)
                if st["alpha"] < 255:
                    ImageDraw.Draw(over).rounded_rectangle(box, radius=r, fill=st["color"] + (st["alpha"],))
                else:
                    d.rounded_rectangle(box, radius=r, fill=st["color"])
        elif st["prim"] == 3:                                 # LINES
            for a, b in zip(v[0::2], v[1::2]):
                d.line([a, b], fill=st["color"], width=max(1, st["lw"] // 8))
        elif st["prim"] == 4 and len(v) > 1:                  # LINE_STRIP
            d.line(v, fill=st["color"], width=max(1, st["lw"] // 8), joint="curve")
        st["verts"] = []

    while i < words * 4:
        w = struct.unpack_from("<I", data, i)[0]
        i += 4
        if w == 0xFFFFFF00 or w == 0xFFFFFF01:               # DLSTART, SWAP
            continue
        if w == 0xFFFFFF0C:                                   # TEXT x y font opt s
            x, y, f, o = s16(i), s16(i + 2), s16(i + 4), u16(i + 6)
            s, n = string(i + 8); i += 8 + n
            draw_text(x, y, f, o, s)
        elif w in (0xFFFFFF0D, 0xFFFFFF0E):                  # BUTTON / KEYS x y w h font opt s
            x, y, bw, bh, f, o = s16(i), s16(i + 2), s16(i + 4), s16(i + 6), s16(i + 8), u16(i + 10)
            s, n = string(i + 12); i += 12 + n
            if w == 0xFFFFFF0D:
                d.rounded_rectangle((x, y, x + bw - 1, y + bh - 1), radius=6, fill=st["fg"])
                draw_text(x + bw // 2, y + bh // 2, f, OPT_CENTERX | OPT_CENTERY, s)
            else:
                kw = bw / max(1, len(s))
                for k, ch in enumerate(s):
                    kx = int(x + k * kw)
                    fill = tuple(max(0, c - 60) for c in st["fg"]) if (o & 255) == ord(ch) else st["fg"]
                    d.rounded_rectangle((kx + 2, y, int(kx + kw) - 3, y + bh - 1), radius=6, fill=fill)
                    draw_text(int(kx + kw / 2), y + bh // 2, f, OPT_CENTERX | OPT_CENTERY, ch)
        elif w == 0xFFFFFF2E:                                 # NUMBER
            x, y, f, o = s16(i), s16(i + 2), s16(i + 4), u16(i + 6)
            n = struct.unpack_from("<i", data, i + 8)[0]; i += 12
            draw_text(x, y, f, o, str(n))
        elif w == 0xFFFFFF0F:                                 # PROGRESS
            x, y, bw, bh, o, val = s16(i), s16(i + 2), s16(i + 4), s16(i + 6), u16(i + 8), u16(i + 10)
            rng = u16(i + 12); i += 16
            d.rounded_rectangle((x, y, x + bw, y + bh), radius=bh // 2, fill=st["bg"])
            d.rounded_rectangle((x, y, x + int(bw * val / max(1, rng)), y + bh), radius=bh // 2, fill=st["fg"])
        elif w == 0xFFFFFF3B:                                 # SETFONT2 handle ptr first
            h = struct.unpack_from("<I", data, i)[0]
            if 1 <= h <= len(SIZES):
                OWN[h] = SIZES[h - 1][0]
            i += 12
        elif w == 0xFFFFFF16:                                 # SPINNER (not drawn)
            i += 8
        elif w == 0xFFFFFF0A:
            st["fg"] = rgb(struct.unpack_from("<I", data, i)[0]); i += 4
        elif w in (0xFFFFFF09, 0xFFFFFF34):
            if w == 0xFFFFFF09:
                st["bg"] = rgb(struct.unpack_from("<I", data, i)[0])
            i += 4
        elif (w >> 30) == 1:                                  # VERTEX2F
            x = (w >> 15) & 0x7FFF; y = w & 0x7FFF
            x = x - 0x8000 if x & 0x4000 else x
            y = y - 0x8000 if y & 0x4000 else y
            st["verts"].append((x, y))
        else:
            op = w >> 24
            if op == 2:                                       # CLEAR_COLOR_RGB
                d.rectangle((0, 0, W, H), fill=rgb(w))
            elif op == 4:
                st["color"] = rgb(w)
            elif op == 16:
                st["alpha"] = w & 255
            elif op == 14:
                st["lw"] = w & 4095
            elif op == 31:
                st["prim"] = w & 15; st["verts"] = []
            elif op == 33:
                flush_prim()
        if st["alpha"] == 255 and over.getbbox():
            img.paste(Image.alpha_composite(img.convert("RGBA"), over).convert("RGB"))
            over = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            d = ImageDraw.Draw(img)
    return img


def main():
    for path in sys.argv[1:]:
        img = render(open(path, "rb").read())
        out = os.path.splitext(path)[0] + ".png"
        img.save(out)
        print(out)


if __name__ == "__main__":
    main()
