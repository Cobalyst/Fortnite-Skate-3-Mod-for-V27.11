"""EA RefPack (QFS) decompression.  python refpack.py <in> <out>"""
import sys


def decompress(data: bytes) -> bytes:
    flags = data[0]
    if data[1] != 0xFB:
        raise ValueError("not RefPack")
    pos = 2
    size_bytes = 4 if flags & 0x80 else 3
    if flags & 0x01:  # compressed size present
        pos += size_bytes
    out_size = int.from_bytes(data[pos:pos + size_bytes], "big")
    pos += size_bytes
    out = bytearray()
    while len(out) < out_size:
        b0 = data[pos]
        if b0 < 0x80:
            b1 = data[pos + 1]; pos += 2
            lit = b0 & 3
            length = ((b0 & 0x1C) >> 2) + 3
            offset = ((b0 & 0x60) << 3) + b1 + 1
        elif b0 < 0xC0:
            b1, b2 = data[pos + 1], data[pos + 2]; pos += 3
            lit = (b1 >> 6) & 3
            length = (b0 & 0x3F) + 4
            offset = ((b1 & 0x3F) << 8) + b2 + 1
        elif b0 < 0xE0:
            b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]; pos += 4
            lit = b0 & 3
            length = ((b0 & 0x0C) << 6) + b3 + 5
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1
        elif b0 < 0xFC:
            pos += 1
            lit = ((b0 & 0x1F) << 2) + 4
            length = 0
            offset = 0
        else:
            pos += 1
            lit = b0 & 3
            out += data[pos:pos + lit]
            break
        out += data[pos:pos + lit]
        pos += lit
        for _ in range(length):
            out.append(out[-offset])
    return bytes(out[:out_size])


if __name__ == "__main__":
    raw = open(sys.argv[1], "rb").read()
    open(sys.argv[2], "wb").write(decompress(raw))
