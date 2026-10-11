"""Builds skate-data/strings_en.txt (KEY<TAB>English text) from Skate 3's language tables
(miscboot.big: LANGUAGE_Labels_Global + LANGUAGE_English_Global, RefPack compressed).
Layout after decompression (little-endian): 0x08 u32 count, then at 0x24 `count` pairs of
(u32 hash, u32 offset); strings (nul-terminated) follow the table, offsets relative to it."""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from refpack import decompress  # noqa: E402

DIR = (SD + r"\extracted\miscboot")
OUT = (SD + r"\strings_en.txt")


def table(path):
    d = decompress(open(path, "rb").read())
    count = struct.unpack_from("<I", d, 8)[0]
    base = 0x24 + count * 8
    out = {}
    for i in range(count):
        h, off = struct.unpack_from("<II", d, 0x24 + i * 8)
        end = d.index(b"\0", base + off)
        out[h] = d[base + off:end].decode("utf-8", "replace")
    return out


labels = table(os.path.join(DIR, "LANGUAGE_Labels_Global_skate3ng.BIN"))
english = table(os.path.join(DIR, "LANGUAGE_English_Global_skate3ng.BIN"))
pairs = {labels[h]: english[h] for h in labels if h in english}
with open(OUT, "w", encoding="utf-8") as f:
    for k in sorted(pairs):
        f.write(f"{k}\t{pairs[k].replace(chr(10), ' ')}\n")
print(f"{len(labels)} labels, {len(english)} english, {len(pairs)} joined")
for k in ["ID_TRICK_FLIP_OLLIE", "ID_TRICK_GROUND_TRICK_POWERSLIDE", "GRIND_BS_50_50", "GRIND_BS_BOARD_SLIDE"]:
    print(k, "->", pairs.get(k))
