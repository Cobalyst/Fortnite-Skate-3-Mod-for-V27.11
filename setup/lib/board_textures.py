"""Writes the Skate 3 board textures from skate-data/assets/board.json as PNG files
into skate-data/board/ (tex0.png deck, tex1.png trucks, tex2.png wheels)."""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import json
import os
import struct
import zlib

ROOT = (SD + r"")


def write_png(path, width, height, rgba, opaque):
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # filter: none
        row = rgba[y * width * 4:(y + 1) * width * 4]
        if opaque:
            row = bytearray(row)
            row[3::4] = b"\xff" * width
        rows.extend(row)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    board = json.load(open(os.path.join(ROOT, "assets", "board.json")))
    out = os.path.join(ROOT, "board")
    os.makedirs(out, exist_ok=True)
    for i, t in enumerate(board["textures"]):
        alpha = t["rgba"][3::4]
        print(f"tex{i}: {t['width']}x{t['height']} alpha min {min(alpha)} max {max(alpha)}")
        # The alpha channel holds material data (gloss etc.), not transparency.
        write_png(os.path.join(out, f"tex{i}.png"), t["width"], t["height"], bytes(t["rgba"]), opaque=True)


if __name__ == "__main__":
    main()
