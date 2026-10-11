"""Snap locators of every skate.park piece (Skate 3's object dropper snapping) -> skate-data/dropper/snaps.txt.

The piece's arena (objects.txt column 5, parkassets/<arena>.rx2) has a locator list 0xeb001e: u32 count,
u32 first entry offset, then 0x70-byte entries: 4x4 matrix (rows x, y, z, position; z = the outward
normal), AABB min / max, +0x60 type, +0x64 group. Type 8 entries are snap locators (828402a8): group 0
= a snap point, otherwise the locators of one group are the corners of a snap face (8284c290: the face's
frame is its first corner's, its vertices the corners in that frame).

Output, one line per locator (Skate 3 model space, metres, y up):
    <object id> <group> x0 x1 x2 y0 y1 y2 z0 z1 z2 px py pz
"""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import rw4lib

ROOT = (SD + '/dropper/')


def locators(arena):
    A = rw4lib.Arena(rw4lib.PARK + arena + '.rx2')
    d = A.d
    out = []
    for i in A.of(0xeb001e):
        off, size = A.body(i)
        n, start = struct.unpack_from('>2I', d, off)
        for k in range(n):
            e = off + start + k * 0x70
            if e + 0x70 > off + size:
                break
            m = struct.unpack_from('>16f', d, e)
            typ, grp = struct.unpack_from('>2I', d, e + 0x60)
            if typ != 8:
                continue
            out.append((grp, m[0:3], m[4:7], m[8:11], m[12:15]))
    return out


def main():
    lines, pieces, total = [], 0, 0
    for l in open(ROOT + 'objects.txt'):
        if l.startswith('#'):
            continue
        f = l.rstrip('\n').split('\t')
        oid, arena = f[2], f[4]
        if not os.path.exists(rw4lib.PARK + arena + '.rx2'):
            continue
        locs = locators(arena)
        if locs:
            pieces += 1
        for grp, x, y, z, p in locs:
            total += 1
            lines.append('%s %d %s' % (oid, grp >> 24 if grp & 0xFFFFFF == 0 else grp,
                                       ' '.join('%.6g' % v for v in (*x, *y, *z, *p))))
    open(ROOT + 'snaps.txt', 'w').write(
        '# object group(0 = point) x-row y-row z-row(normal) position  (Skate 3 model space, metres)\n' + '\n'.join(lines) + '\n')
    print(pieces, 'pieces with snap locators,', total, 'locators')


if __name__ == '__main__':
    main()
