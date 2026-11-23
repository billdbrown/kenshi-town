"""Placeholder overhead icons: a coloured disc with a letter, 64 px, PNG.

    python scripts/assets/icons.py mod/TownLedger

The PNGs are committed in mod/TownLedger (build.cmd copies that folder), because
the mod build runs Python isolated (-I) and has no PIL.

Prototype art only (2026-11-23): the mechanism is what's being proved. The
real icons, and which things get one, are Bill's to choose.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ICONS = {
    "kt_icon_bar.png": ("B", (196, 138, 44)),      # the bar
    "kt_icon_hq.png": ("H", (70, 150, 90)),        # the town hall
    "kt_icon_trader.png": ("T", (60, 120, 190)),   # traders and shopkeepers
    "kt_icon_hostile.png": ("!", (190, 50, 40)),   # a hostile squad
}


def font(size):
    for name in ("arialbd.ttf", "arial.ttf"):
        try:
            return ImageFont.truetype(os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name), size)
        except OSError:
            pass
    return ImageFont.load_default()


def write_all(out_dir):
    f = font(40)
    for name, (letter, colour) in ICONS.items():
        big = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        d = ImageDraw.Draw(big)
        d.ellipse((8, 8, 248, 248), fill=(20, 16, 12, 235))
        d.ellipse((24, 24, 232, 232), fill=colour + (255,))
        img = big.resize((64, 64), Image.LANCZOS)
        d = ImageDraw.Draw(img)
        box = d.textbbox((0, 0), letter, font=f)
        d.text((32 - (box[0] + box[2]) / 2, 32 - (box[1] + box[3]) / 2), letter, font=f, fill=(250, 244, 230, 255))
        img.save(os.path.join(out_dir, name))
    return sorted(ICONS)


if __name__ == "__main__":
    print("wrote", write_all(sys.argv[1]))
