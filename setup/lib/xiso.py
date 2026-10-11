"""Reads an Xbox 360 game disc image (XDVDFS / "XISO", full XGD2/XGD3 dumps or trimmed images): lists its
files and copies selected ones out, without extracting the whole disc.

Layout: the game partition starts at 0 (trimmed), 0x18300000 (XGD1), 0xFD90000 (XGD2) or 0x2080000 (XGD3).
Sector 32 of the partition holds "MICROSOFT*XBOX*MEDIA", the root directory's sector and size (LE u32).
A directory is a binary tree of entries: u16 left, u16 right (in dwords from the directory start),
u32 sector, u32 size, u8 attributes (0x10 = directory), u8 name length, name. Sectors are 2048 bytes."""
import os
import struct

SECTOR = 2048
MAGIC = b'MICROSOFT*XBOX*MEDIA'
PARTITIONS = (0, 0xFD90000, 0x2080000, 0x18300000)


class Iso:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.base = None
        for base in PARTITIONS:
            self.f.seek(base + 32 * SECTOR)
            head = self.f.read(28)
            if head[:20] == MAGIC:
                self.base = base
                self.root_sector, self.root_size = struct.unpack('<II', head[20:28])
                break
        if self.base is None:
            raise ValueError('not an Xbox 360 disc image')

    def close(self):
        self.f.close()

    def _read(self, sector, size):
        self.f.seek(self.base + sector * SECTOR)
        return self.f.read(size)

    def _walk_dir(self, sector, size, prefix, out):
        data = self._read(sector, size)
        todo = [0] if size else []
        seen = set()
        while todo:
            off = todo.pop()
            if off in seen or off + 14 > len(data):
                continue
            seen.add(off)
            left, right, sec, sz, attr, nlen = struct.unpack_from('<HHIIBB', data, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue   # padding
            name = data[off + 14:off + 14 + nlen].decode('latin-1')
            path = prefix + name
            if attr & 0x10:
                out.append((path + '/', sec, sz, True))
                if sz:
                    self._walk_dir(sec, sz, path + '/', out)
            else:
                out.append((path, sec, sz, False))
            if left:
                todo.append(left * 4)
            if right:
                todo.append(right * 4)

    def files(self):
        """[(path with '/', sector, size, is_dir)]"""
        out = []
        self._walk_dir(self.root_sector, self.root_size, '', out)
        return out

    def extract(self, wanted, dest, progress=None):
        """Copies every file whose path (case-insensitive, '/') equals or starts with an entry of wanted."""
        wanted = [w.lower().replace('\\', '/') for w in wanted]
        picked = [(p, s, n) for p, s, n, d in self.files()
                  if not d and any(p.lower() == w or p.lower().startswith(w.rstrip('/') + '/') for w in wanted)]
        total = sum(n for _, _, n in picked) or 1
        done = 0
        for p, s, n in picked:
            target = os.path.join(dest, *p.split('/'))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            self.f.seek(self.base + s * SECTOR)
            left = n
            with open(target, 'wb') as o:
                while left:
                    chunk = self.f.read(min(left, 8 << 20))
                    if not chunk:
                        raise IOError(f'disc image ends early in {p}')
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done, total)
        return [p for p, _, _ in picked]
