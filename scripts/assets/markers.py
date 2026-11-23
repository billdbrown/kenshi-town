"""Overhead markers in Kenshi's own sign language, made from the installed game.

    python scripts/assets/markers.py <out dir> [preview.png]

Kenshi's shop signs (data/buildings/materials/signs2_diffuse.dds) are small
rusted metal plaques with a dark painted glyph: crossed blades for weapons, a
steaming bowl for food, a helmet for armour. The markers are built the same
way: a plaque cut from that texture, and a glyph painted on it. The hostile
glyph is Kenshi's own crossed blades, lifted from the same texture; the
tankard, coin pouch and town hall are drawn here to match.

Two styles, for Bill to compare in game (2026-11-23):
  A  "plaque": the glyph painted on a plaque. On hover the plaque stretches
     into a sign with the name painted on it (left part, a middle strip that
     stretches, a right cap).
  B  "engraved": the glyph bare, off-white with a shadow, like Kenshi's map
     icons; the name shows as Kenshi's own name tag.

Nothing from the game goes into git: this reads the installed texture and
writes PNGs straight into the install's mods folder (build.cmd install).
Needs PIL, so it runs without python -I.
"""
import os
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

KENSHI = os.environ.get("KENSHI_DIR", r"C:\Program Files (x86)\Steam\steamapps\common\Kenshi")
SIGNS = os.path.join(KENSHI, "data", "buildings", "materials", "signs2_diffuse.dds")

H = 48                 # marker height on screen, px (1080p); drawn at that size
SS = 8                 # supersampling for the drawn glyphs
PLAQUE = (300, 6, 694, 262)        # a blank plaque in signs2_diffuse (x0, y0, x1, y1)
BLADES = (322, 596, 478, 756)      # Kenshi's crossed-blades glyph (weapon shops)
LEFT_W = int(H * 0.80)             # plaque: the left part holds the glyph
RIGHT_W = H - LEFT_W               # ...and the right cap closes it; together a square
MID_LONG = 512                     # the sign body under the name; shown, not stretched
PAINT = (38, 34, 31)               # Kenshi's sign paint, sampled off the BAR sign
HOSTILE_PAINT = (128, 22, 14)      # dried-blood red for trouble
BONE = (222, 215, 206)             # Kenshi's name-tag off-white (0.865 0.842 0.810)


def signs():
    return Image.open(SIGNS).convert("RGB")  # the alpha channel is gloss, not coverage


# ---- glyph masks (white = paint), H x H --------------------------------------

def _canvas():
    n = H * SS
    return Image.new("L", (n, n), 0), n


def _finish(big):
    return big.resize((H, H), Image.LANCZOS)


def tankard():
    m, n = _canvas()
    d = ImageDraw.Draw(m)
    u = n / 48.0
    # body
    d.rounded_rectangle((13 * u, 15 * u, 31 * u, 38 * u), radius=3 * u, fill=255)
    # foam: three bumps over the rim
    for cx in (15.5, 22, 28.5):
        d.ellipse(((cx - 4.5) * u, 9.5 * u, (cx + 4.5) * u, 18 * u), fill=255)
    # handle: a ring on the right
    d.rounded_rectangle((28 * u, 19 * u, 38 * u, 33 * u), radius=5 * u, fill=255)
    d.rounded_rectangle((31 * u, 22.5 * u, 35 * u, 29.5 * u), radius=2 * u, fill=0)
    # staves: two cut lines down the body
    for x in (18.5, 25.5):
        d.rectangle(((x - 0.8) * u, 20 * u, (x + 0.8) * u, 34 * u), fill=0)
    return _finish(m)


def pouch():
    m, n = _canvas()
    d = ImageDraw.Draw(m)
    u = n / 48.0
    # body, bottom heavy
    d.ellipse((11 * u, 17 * u, 37 * u, 40 * u), fill=255)
    d.polygon([(17 * u, 20 * u), (31 * u, 20 * u), (27 * u, 15 * u), (21 * u, 15 * u)], fill=255)
    # neck tie
    d.rectangle((19 * u, 13.5 * u, 29 * u, 16 * u), fill=0)
    d.rounded_rectangle((18.5 * u, 14 * u, 29.5 * u, 16.5 * u), radius=1 * u, fill=255)
    d.line((19 * u, 16.8 * u, 29 * u, 16.8 * u), fill=0, width=int(1.1 * u))
    # flared top
    d.polygon([(20 * u, 14 * u), (16 * u, 8 * u), (21 * u, 10 * u), (24 * u, 7 * u), (27 * u, 10 * u),
               (32 * u, 8 * u), (28 * u, 14 * u)], fill=255)
    # a coin cut out of the body
    d.ellipse((19.5 * u, 24.5 * u, 28.5 * u, 33.5 * u), fill=0)
    d.ellipse((21.5 * u, 26.5 * u, 26.5 * u, 31.5 * u), fill=255)
    return _finish(m)


def hall():
    m, n = _canvas()
    d = ImageDraw.Draw(m)
    u = n / 48.0
    # banner pole and pennant
    d.rectangle((23 * u, 5 * u, 25 * u, 16 * u), fill=255)
    d.polygon([(25 * u, 5.5 * u), (34 * u, 8 * u), (25 * u, 11 * u)], fill=255)
    # roof
    d.polygon([(9 * u, 23 * u), (24 * u, 14.5 * u), (39 * u, 23 * u), (36 * u, 23 * u), (24 * u, 17 * u),
               (12 * u, 23 * u)], fill=255)
    d.polygon([(12 * u, 23.5 * u), (24 * u, 17.5 * u), (36 * u, 23.5 * u)], fill=255)
    # walls
    d.rectangle((13 * u, 23 * u, 35 * u, 38 * u), fill=255)
    # base step
    d.rectangle((10 * u, 37 * u, 38 * u, 40 * u), fill=255)
    # arched door
    d.rectangle((20.5 * u, 29 * u, 27.5 * u, 37 * u), fill=0)
    d.ellipse((20.5 * u, 25.5 * u, 27.5 * u, 32.5 * u), fill=0)
    # windows
    for x in (15.5, 30.5):
        d.rectangle((x * u, 27 * u, (x + 2.5) * u, 31 * u), fill=0)
    return _finish(m)


def blades(tex):
    """Kenshi's crossed mace and sword, lifted off the weapon-shop sign."""
    tile = tex.crop(BLADES)
    r, g, b = [c.point(lambda v: v) for c in tile.split()]
    # the paint is dark and grey; rust is dark but red, the plate light
    lum = Image.merge("RGB", (r, g, b)).convert("L")
    dark = lum.point(lambda v: 255 if v < 95 else 0)
    redness = ImageChops.subtract(r, b).point(lambda v: 255 if v > 28 else 0)
    mask = ImageChops.subtract(dark, redness).filter(ImageFilter.MedianFilter(5))
    mask = mask.filter(ImageFilter.MaxFilter(5))  # thicker: it is drawn at 34 px
    box = mask.getbbox()
    mask = mask.crop(box)
    # fit in the same frame as the drawn glyphs (about 30 of 48 px)
    side = max(mask.size)
    sq = Image.new("L", (side, side), 0)
    sq.paste(mask, ((side - mask.size[0]) // 2, (side - mask.size[1]) // 2))
    inner = int(H * 0.72)
    glyph = sq.resize((inner, inner), Image.LANCZOS)
    out = Image.new("L", (H, H), 0)
    out.paste(glyph, ((H - inner) // 2, (H - inner) // 2))
    return out


# ---- plaques -----------------------------------------------------------------

def plaque(tex):
    """The blank plaque squeezed to H x H, and its parts for the hover sign."""
    p = tex.crop(PLAQUE)
    sq = p.resize((H, H), Image.LANCZOS)
    tall = p.resize((int(H * p.size[0] / float(p.size[1])), H), Image.LANCZOS)
    w = tall.size[0]
    left = tall.crop((0, 0, LEFT_W, H))
    right = tall.crop((w - RIGHT_W, 0, w, H))
    body = tall.crop((LEFT_W, 0, w - RIGHT_W, H))
    flipped = body.transpose(Image.FLIP_LEFT_RIGHT)
    mid = Image.new("RGB", (MID_LONG, H))
    x, flip = 0, False
    while x < MID_LONG:
        mid.paste(flipped if flip else body, (x, 0))
        x += body.size[0]
        flip = not flip
    return sq, left, mid, right


def rounded_alpha(size, radius):
    a = Image.new("L", (size[0] * 4, size[1] * 4), 0)
    ImageDraw.Draw(a).rounded_rectangle((0, 0, size[0] * 4 - 1, size[1] * 4 - 1), radius=radius * 4, fill=255)
    return a.resize(size, Image.LANCZOS)


def paint_on(base, mask, colour):
    """Paint a glyph on a plate the way a sign is painted: the grime shows through."""
    base = base.convert("RGB")
    grime = base.convert("L").point(lambda v: int(150 + v * 0.42))  # light plate -> nearly full paint
    a = ImageChops.multiply(mask, grime)
    ink = Image.new("RGB", base.size, colour)
    return Image.composite(ink, base, a)


def with_alpha(rgb, alpha):
    out = rgb.convert("RGBA")
    out.putalpha(alpha)
    return out


def engraved(mask):
    """Style B: off-white glyph with a dark rim and a drop shadow."""
    rim = mask.filter(ImageFilter.MaxFilter(5))
    shadow = Image.new("L", mask.size, 0)
    shadow.paste(rim, (1, 2))
    shadow = shadow.filter(ImageFilter.GaussianBlur(1.2))
    out = Image.new("RGBA", mask.size, (0, 0, 0, 0))
    out = Image.composite(Image.new("RGBA", mask.size, (12, 10, 8, 200)), out, shadow)
    out = Image.composite(Image.new("RGBA", mask.size, (30, 26, 22, 255)), out, rim)
    return Image.composite(Image.new("RGBA", mask.size, BONE + (255,)), out, mask)


def write_all(out_dir, preview=None):
    tex = signs()
    glyphs = {"bar": tankard(), "trader": pouch(), "hq": hall(), "hostile": blades(tex)}
    sq, left, mid, right = plaque(tex)
    corner = 5
    written = []

    def save(img, name):
        img.save(os.path.join(out_dir, name))
        written.append(name)

    sq_alpha = rounded_alpha((H, H), corner)
    left_alpha = rounded_alpha((LEFT_W + corner * 2, H), corner).crop((0, 0, LEFT_W, H))
    right_alpha = rounded_alpha((RIGHT_W + corner * 2, H), corner).crop((corner * 2, 0, RIGHT_W + corner * 2, H))
    for kind, mask in sorted(glyphs.items()):
        colour = HOSTILE_PAINT if kind == "hostile" else PAINT
        save(with_alpha(paint_on(sq, mask, colour), sq_alpha), "kt_mk_a_%s.png" % kind)
        lft = Image.new("RGB", (LEFT_W, H))
        lft.paste(left, (0, 0))
        # the glyph sits where it sits on the square plaque: centred on H/2
        g = mask.crop((0, 0, LEFT_W, H))
        save(with_alpha(paint_on(lft, g, colour), left_alpha), "kt_mk_a_%s_l.png" % kind)
        save(engraved(mask), "kt_mk_b_%s.png" % kind)
    save(with_alpha(mid, Image.new("L", mid.size, 255)), "kt_mk_a_mid.png")
    save(with_alpha(right, right_alpha), "kt_mk_a_r.png")

    if preview:
        sheet = Image.new("RGBA", (H * 12, H * 5), (120, 96, 72, 255))
        # a desert-ish backdrop, light and dark, to judge contrast
        ImageDraw.Draw(sheet).rectangle((0, H * 2.5, H * 12, H * 5), fill=(64, 52, 44, 255))
        for row, y in ((0, H // 2), (1, H * 3)):
            x = H // 2
            for kind in ("bar", "trader", "hq", "hostile"):
                sheet.alpha_composite(Image.open(os.path.join(out_dir, "kt_mk_a_%s.png" % kind)), (x, y))
                sheet.alpha_composite(Image.open(os.path.join(out_dir, "kt_mk_b_%s.png" % kind)), (x + H + 4, y))
                x += H * 2 + 20
            # a hover sign: left + stretched middle + right
            sx, sy = H // 2, y + H + 6
            sheet.alpha_composite(Image.open(os.path.join(out_dir, "kt_mk_a_trader_l.png")), (sx, sy))
            midimg = Image.open(os.path.join(out_dir, "kt_mk_a_mid.png")).crop((0, 0, H * 4, H))
            sheet.alpha_composite(midimg, (sx + LEFT_W, sy))
            sheet.alpha_composite(Image.open(os.path.join(out_dir, "kt_mk_a_r.png")), (sx + LEFT_W + H * 4, sy))
        sheet = sheet.resize((sheet.size[0] * 2, sheet.size[1] * 2), Image.NEAREST)
        sheet.save(preview)
    return written


if __name__ == "__main__":
    out = sys.argv[1]
    if not os.path.isdir(out):
        os.makedirs(out)
    print("wrote", ", ".join(write_all(out, sys.argv[2] if len(sys.argv) > 2 else None)))
