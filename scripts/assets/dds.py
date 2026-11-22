"""Write uncompressed 32-bit BGRA DDS with a full mip chain."""
import struct, sys
from PIL import Image

def mip_chain(img):
    img = img.convert('RGBA')
    w, h = img.size
    mips = []
    while True:
        r, g, b, a = img.split()
        mips.append(Image.merge('RGBA', (b, g, r, a)).tobytes())
        if w == 1 and h == 1:
            break
        w, h = max(1, w // 2), max(1, h // 2)
        img = img.resize((w, h), Image.LANCZOS)
    return mips

def save(src_img, path):
    src_img = src_img.convert('RGBA')
    W, H = src_img.size
    mips = mip_chain(src_img)
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x8  # caps|height|width|pixelformat|mipmapcount|pitch
    pf = struct.pack('<II4sIIIII', 32, 0x41, b'\0\0\0\0', 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    caps = 0x1000 | 0x8 | 0x400000  # texture|complex|mipmap
    hdr = struct.pack('<4sIIIIIII', b'DDS ', 124, flags, H, W, W * 4, 0, len(mips)) + b'\0' * 44 + pf \
        + struct.pack('<IIIII', caps, 0, 0, 0, 0)
    with open(path, 'wb') as f:
        f.write(hdr)
        for m in mips:
            f.write(m)

if __name__ == '__main__':
    out = sys.argv[1]
    save(Image.open(out.replace('.dds', '.png')), out)
    save(Image.new('RGBA', (64, 64), (128, 128, 255, 255)), out.replace('.dds', '_n.dds'))
    print('ok', out)
