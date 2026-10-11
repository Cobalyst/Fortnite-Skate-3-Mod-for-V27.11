"""Skate 3 "chunkref" container: 'chunkref' u32 version, u32 total uncompressed size, u32 chunk size,
then chunks from 0x28 (headers at 8 mod 16): u32 compressed size, u32 kind (2 = RefPack, else stored)."""
import struct
import refpack


def unpack(raw):
    if not raw.startswith(b"chunkref"):
        i = raw.find(b"\x10\xfb", 0, 0x100)
        return refpack.decompress(raw[i:]) if i >= 0 else raw
    total = struct.unpack_from(">I", raw, 12)[0]
    out = bytearray()
    p = 0x28
    while len(out) < total and p + 8 <= len(raw):
        csize, kind = struct.unpack_from(">II", raw, p)
        body = raw[p + 8:p + 8 + csize]
        out += refpack.decompress(body) if kind == 2 else body
        end = p + 8 + csize
        p = end + ((8 - end) % 16)  # next header at 8 mod 16, so chunk data stays 16-aligned
    return bytes(out[:total])
