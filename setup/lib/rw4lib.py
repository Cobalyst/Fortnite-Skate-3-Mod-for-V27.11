"""Skate 3 RW4 arena (.rx2) helpers: load (chunkref-decoded), object table, typed object lookup."""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import chunkref

PARK = (SD + '/extracted/parkassets/')


class Arena:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        self.d = chunkref.unpack(raw)
        d = self.d
        if d[1:4] != b'RW4':
            raise ValueError('not RW4: %r' % d[:8])
        self.count = struct.unpack_from('>I', d, 0x20)[0]
        self.table = struct.unpack_from('>I', d, 0x30)[0]
        self.data = struct.unpack_from('>I', d, 0x44)[0]
        self.objs = []
        for i in range(self.count):
            off, _, size, al, ti, tid = struct.unpack_from('>6I', d, self.table + i * 24)
            self.objs.append((off, size, tid))

    def u32(self, o): return struct.unpack_from('>I', self.d, o)[0]
    def f32(self, o): return struct.unpack_from('>f', self.d, o)[0]

    def of(self, tid):
        return [i for i, o in enumerate(self.objs) if o[2] == tid]

    def body(self, i):
        off, size, tid = self.objs[i]
        if tid == 0x10031:  # base resource: in the data section
            return self.data + off, size
        return off, size
