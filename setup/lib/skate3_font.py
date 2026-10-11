"""Converts a Skate 3 bitmap font (miscboot.big: <name>.bmpFont + <name>.rx2) into
skate-data/font/<name>.png (white glyphs, alpha = coverage) and <name>.txt (glyph table).
The .rx2 is a RefPack-compressed RenderWare 4 Xbox 360 texture: DXT5, byte-swapped, untiled."""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(__file__))
from refpack import decompress  # noqa: E402

SRC = (SD + r"\extracted\miscboot")
OUT = (SD + r"\font")


def tiled_offset(x, y, width, log_bpb):
    """Xbox 360 XGAddress2DTiledOffset (element index)."""
    aligned = (width + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log_bpb + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << log_bpb
    offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return (((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F)) >> log_bpb


def write_png(path, width, height, rgba):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        rows.extend(rgba[y * width * 4:(y + 1) * width * 4])

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def dxt5_alpha(block):
    """8 alpha values for the 16 texels of a DXT5 block (little-endian after the byte swap)."""
    a0, a1 = block[0], block[1]
    if a0 > a1:
        table = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        table = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    bits = int.from_bytes(block[2:8], "little")
    return [table[(bits >> (3 * i)) & 7] for i in range(16)]


def convert(name, width=512, height=512):
    raw = decompress(open(os.path.join(SRC, name + ".rx2"), "rb").read())
    # Fetch constant in the RW4 header says DXT5 (format 0x14), 8-in-16 endian; blocks are linear.
    data = raw[-width * height:]  # DXT5 is 1 byte per texel; texels sit after the header
    b = bytearray(data)
    b[0::2], b[1::2] = data[1::2], data[0::2]  # 360 big-endian 16-bit words
    data = bytes(b)
    bw, bh = width // 4, height // 4
    rgba = bytearray(width * height * 4)
    for by in range(bh):
        for bx in range(bw):
            at = (by * bw + bx) * 16
            alpha = dxt5_alpha(data[at:at + 8])
            for i, a in enumerate(alpha):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                p = (y * width + x) * 4
                rgba[p:p + 4] = (255, 255, 255, a)
    os.makedirs(OUT, exist_ok=True)
    write_png(os.path.join(OUT, name + ".png"), width, height, rgba)
    open(os.path.join(OUT, name + ".txt"), "wb").write(decompress(open(os.path.join(SRC, name + ".bmpFont"), "rb").read()))
    print("wrote", name)


if __name__ == "__main__":
    for n in sys.argv[1:] or ["futuraheavy"]:
        convert(n)
