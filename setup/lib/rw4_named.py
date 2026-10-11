"""Converts a Skate 3 RW4 (Xbox 360) texture pack (decompressed .rx2.bin) into one PNG per texture,
named by the texture's own name ("26.Texture" -> 26.png, "xray.Texture" -> xray.png).

RW4 layout: header +0x20 object count, +0x30 object table (24-byte entries: offset, 0, size,
alignment, type index, type id), +0x44 start of the data section. Texture headers (type 0x200E8,
offsets into the file) hold an Xbox 360 GPUTEXTURE_FETCH_CONSTANT: word0 bit 31 tiled, bits 22..30
pitch / 32; word1 low 6 bits format; word2 size (width-1 bits 0..12, height-1 bits 13..25). Pixel
blocks (type 0x10031, offsets into the data section) follow in the same order. Names are the
"<name>.Texture" strings in the same order. Tiled addressing follows Xenia's Tiled2D.
Formats: 0x06 8_8_8_8, 0x04 5_6_5, 0x12 DXT1, 0x13 DXT2/3, 0x14 DXT4/5.
python rw4_named.py <file.rx2.bin> <out_dir>"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from skate3_font import write_png, dxt5_alpha  # noqa: E402


def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def xenia_tiled(x, y, pitch_blocks, log2_bpb):
    """Xenia texture_address.h Tiled2D: byte offset of block (x, y) in a tiled surface."""
    outer = ((y >> 5) * (pitch_blocks >> 5) + (x >> 5)) << 6
    inner = (((y >> 1) & 7) << 3) | (x & 7)
    oi = (outer | inner) << log2_bpb
    bank = (y >> 4) & 1
    pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1)
    return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 15) | (((oi >> 4) & 1) << 5) | (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12)


def rgb565(c):
    return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def dxt_colors(blk, dxt1):
    c0, c1 = struct.unpack("<HH", blk[0:4])
    p0, p1 = rgb565(c0), rgb565(c1)
    if dxt1 and c0 <= c1:
        pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
    else:
        pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
    bits = struct.unpack("<I", blk[4:8])[0]
    return [pal[(bits >> (2 * i)) & 3] for i in range(16)]


def swap16(b):
    return bytes(x for k in range(0, len(b), 2) for x in (b[k + 1], b[k]))


def decode(d, at, w, h, fmt, tiled, pitch_texels):
    px = bytearray(w * h * 4)
    block = fmt in (0x12, 0x13, 0x14)
    bpb = {0x12: 8, 0x13: 16, 0x14: 16, 0x06: 4, 0x04: 2}.get(fmt)
    if bpb is None:
        raise ValueError(f"format {fmt:#x}")
    bw, bh = (max(1, w // 4), max(1, h // 4)) if block else (w, h)
    pitch = max(bw, (pitch_texels // 4) if block else pitch_texels)
    if tiled:
        pitch = ((pitch + 31) // 32) * 32
    log2 = {16: 4, 8: 3, 4: 2, 2: 1}[bpb]
    for by in range(bh):
        for bx in range(bw):
            o = at + (xenia_tiled(bx, by, pitch, log2) if tiled else (by * pitch + bx) * bpb)
            raw = d[o:o + bpb]
            if len(raw) < bpb:
                continue
            if block:
                blk = swap16(raw)  # 16-bit byte-swapped on the 360
                if fmt == 0x12:
                    texels = dxt_colors(blk, True)
                elif fmt == 0x14:
                    a = dxt5_alpha(blk)
                    texels = [c[:3] + (a[i],) for i, c in enumerate(dxt_colors(blk[8:], False))]
                else:  # DXT3: explicit 4-bit alpha
                    alpha = [((blk[i // 2] >> (4 * (i & 1))) & 15) * 17 for i in range(16)]
                    texels = [c[:3] + (alpha[i],) for i, c in enumerate(dxt_colors(blk[8:], False))]
                for i in range(16):
                    x, y = bx * 4 + i % 4, by * 4 + i // 4
                    if x < w and y < h:
                        p = (y * w + x) * 4
                        px[p:p + 4] = bytes(texels[i])
            else:
                if fmt == 0x06:   # ARGB, big-endian dword
                    a, r, g, b = raw[0], raw[1], raw[2], raw[3]
                else:             # 565
                    r, g, b = rgb565(struct.unpack(">H", raw)[0])
                    a = 255
                p = (by * w + bx) * 4
                px[p:p + 4] = bytes((r, g, b, a))
    return px


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    os.makedirs(out, exist_ok=True)
    data_start = u32(d, 0x44)
    table, count = u32(d, 0x30), u32(d, 0x20)
    entries = [[u32(d, table + 24 * i + k) for k in range(0, 24, 4)] for i in range(count)]
    heads = [e[0] for e in entries if e[5] == 0x200E8]
    blocks = [e[0] for e in entries if e[5] == 0x10031]
    names = [m.group(1).decode().split("\\")[-1] for m in re.finditer(rb"([A-Za-z0-9_\\\-]+)\.Texture", d)]
    print(f"{len(heads)} textures, {len(blocks)} pixel blocks, {len(names)} names, data at {data_start:#x}")
    for h, block, name in zip(heads, blocks, names):
        for k in range(0, 0x34 - 8, 4):
            a, b, c = u32(d, h + k), u32(d, h + k + 4), u32(d, h + k + 8)
            if (a & 0xFFFF) == 2:
                break
        w, hh = (c & 0x1FFF) + 1, ((c >> 13) & 0x1FFF) + 1
        fmt, tiled, pitch = b & 0x3F, a >> 31, ((a >> 22) & 0x1FF) * 32
        try:
            px = decode(d, data_start + block, w, hh, fmt, tiled, pitch)
        except ValueError as e:
            print(f"  {name}: skipped ({e})")
            continue
        write_png(os.path.join(out, f"{name}.png"), w, hh, bytes(px))
        print(f"  {name}.png {w}x{hh} fmt {fmt:#x} {'tiled' if tiled else 'linear'} pitch {pitch}")


if __name__ == "__main__":
    main()
