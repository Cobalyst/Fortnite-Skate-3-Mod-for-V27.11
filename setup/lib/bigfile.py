"""Lists or extracts EA .BIG archives (BIGF / BIG4 / EB variants seen in Skate 3).
  python bigfile.py list <file.big> [filter]
  python bigfile.py extract <file.big> <out_dir> [filter]
Read-only on the archive."""
import os
import struct
import sys


def entries(path):
    with open(path, "rb") as f:
        head = f.read(16)
        magic = head[:4]
        if magic in (b"BIGF", b"BIG4"):
            count, header_len = struct.unpack(">II", head[8:16])
            f.seek(16)
            table = f.read(header_len - 16)
            out, pos = [], 0
            for _ in range(count):
                offset, size = struct.unpack(">II", table[pos:pos + 8])
                end = table.index(b"\0", pos + 8)
                name = table[pos + 8:end].decode("latin-1")
                pos = end + 1
                out.append((name, offset, size))
            return magic.decode(), out
        if head[:2] == b"EB":
            return parse_eb(f, head)
        raise ValueError(f"unknown BIG magic {head[:8]!r}")


def parse_eb(f, head):
    """EA "EB" v3 big (Skate 3), big-endian:
    0x04 u32 file count, 0x0A u8 offset shift, 0x0C u32 name table offset,
    0x14 u8 name entry size (2-byte folder index + name), 0x16 u16 folder count.
    File table at 0x30: 16 bytes per file = u32 offset>>shift, u32 (0), u32 size, u32 hash."""
    f.seek(0)
    h = f.read(0x30)
    count = struct.unpack(">I", h[4:8])[0]
    shift = h[0x0A]
    names_at = struct.unpack(">I", h[0x0C:0x10])[0]
    name_size = h[0x14]
    table = f.read(count * 16)
    f.seek(names_at)
    names = f.read(count * name_size)
    out = []
    for i in range(count):
        offset, _, size, _ = struct.unpack(">IIII", table[i * 16:i * 16 + 16])
        raw = names[i * name_size + 2:(i + 1) * name_size]
        name = raw.split(b"\0", 1)[0].decode("latin-1")
        out.append((name, offset << shift, size))
    return "EB", out


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    kind, items = entries(path)
    if cmd == "list":
        flt = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        print(f"{kind}: {len(items)} files")
        for name, offset, size in items:
            if flt in name.lower():
                print(f"{size:10}  {name}")
    else:
        out = sys.argv[3]
        flt = sys.argv[4].lower() if len(sys.argv) > 4 else ""
        with open(path, "rb") as f:
            for name, offset, size in items:
                if flt not in name.lower():
                    continue
                dest = os.path.join(out, name.replace("\\", "/"))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                f.seek(offset)
                with open(dest, "wb") as o:
                    o.write(f.read(size))


if __name__ == "__main__":
    main()
