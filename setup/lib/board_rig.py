"""Writes assets\\rig.json and assets\\board.json from the converted skater (assets\\private\\skater.glb),
the way the 2010 Rust Rewrite Mashup does (crates/assets/src/skate_board.rs, Apache-2.0): the skater's
bones (bind and inverse bind matrices) and the skateboard surfaces with their textures (at most 512 px).
python board_rig.py   (paths from fnpaths.SD)"""
import json, math, os, struct, zlib
import numpy as np
from fnpaths import SD

TEXTURE_LIMIT = 512


def png_rgba(data):
    """8-bit non-interlaced PNG (grey, grey+alpha, RGB, RGBA, palette) -> (w, h, rgba bytes)."""
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, plte, trns = 8, b'', None, None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            plte = body
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    if depth != 8 or inter:
        raise ValueError('unsupported PNG (needs 8-bit, not interlaced)')
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    px = np.frombuffer(bytes(out), np.uint8).reshape(-1, ch)
    if ctype == 3:
        pal = np.frombuffer(plte, np.uint8).reshape(-1, 3)
        alpha = np.full(len(pal), 255, np.uint8)
        if trns:
            alpha[:len(trns)] = np.frombuffer(trns, np.uint8)
        rgba = np.concatenate([pal[px[:, 0]], alpha[px[:, 0]][:, None]], 1)
    elif ch == 1:
        rgba = np.concatenate([px.repeat(3, 1), np.full((len(px), 1), 255, np.uint8)], 1)
    elif ch == 2:
        rgba = np.concatenate([px[:, :1].repeat(3, 1), px[:, 1:2]], 1)
    elif ch == 3:
        rgba = np.concatenate([px, np.full((len(px), 1), 255, np.uint8)], 1)
    else:
        rgba = px
    return w, h, rgba.astype(np.uint8).tobytes()


def shrink(w, h, rgba):
    if w <= TEXTURE_LIMIT and h <= TEXTURE_LIMIT:
        return w, h, rgba
    scale = min(TEXTURE_LIMIT / w, TEXTURE_LIMIT / h)
    ow, oh = max(1, int(round_half_away(w * scale))), max(1, int(round_half_away(h * scale)))
    src = np.frombuffer(rgba, np.uint8).reshape(h, w, 4).astype(np.uint32)
    out = np.zeros((oh, ow, 4), np.uint8)
    for y in range(oh):
        y0 = y * h // oh; y1 = max((y + 1) * h // oh, y0 + 1)
        for x in range(ow):
            x0 = x * w // ow; x1 = max((x + 1) * w // ow, x0 + 1)
            n = (y1 - y0) * (x1 - x0)
            s = src[y0:y1, x0:x1].reshape(-1, 4).sum(0)
            out[y, x] = ((s + n // 2) // n).astype(np.uint8)
    return ow, oh, out.tobytes()


def round_half_away(v):
    return math.floor(v + 0.5) if v >= 0 else -math.floor(-v + 0.5)


class Glb:
    def __init__(self, b):
        if b[:4] != b'glTF':
            raise ValueError('skater.glb is not a GLB file')
        jl = struct.unpack_from('<I', b, 12)[0]
        self.json = json.loads(b[20:20 + jl])
        bl = struct.unpack_from('<I', b, 20 + jl)[0]
        self.blob = b[28 + jl:28 + jl + bl]

    def view(self, v):
        v = self.json['bufferViews'][v]
        off = v.get('byteOffset', 0)
        return self.blob[off:off + v['byteLength']], v.get('byteStride')

    def components(self, a):
        a = self.json['accessors'][a]
        count, width = a['count'], {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
        dt = {5121: '<u1', 5123: '<u2', 5125: '<u4', 5126: '<f4'}[a['componentType']]
        size = np.dtype(dt).itemsize
        data, stride = self.view(a['bufferView'])
        start = a.get('byteOffset', 0)
        stride = stride or size * width
        out = np.empty((count, width), dt)
        for e in range(count):
            out[e] = np.frombuffer(data, dt, width, start + e * stride)
        return out.reshape(-1)

    def texture(self, image):
        data, _ = self.view(self.json['images'][image]['bufferView'])
        return shrink(*png_rgba(data))


def f32(v):
    """A float32 written the shortest way that reads back as the same float32 (like Rust's serde_json)."""
    return float(np.format_float_positional(np.float32(v), unique=True, trim='-')) if np.isfinite(v) else 0.0


def export():
    assets = os.path.join(SD, 'assets')
    glb = Glb(open(os.path.join(assets, 'private', 'skater.glb'), 'rb').read())
    skin = glb.json['skins'][0]
    names = [glb.json['nodes'][j]['name'] for j in skin['joints']]
    inv = glb.components(skin['inverseBindMatrices']).astype(np.float32).reshape(-1, 16)
    basis = np.array([1., 0., 0., 0., 0., 0., -1., 0., 0., 1., 0., 0., 0., 0., 0., 1.]).reshape(4, 4).T   # columns
    basis_inv = np.linalg.inv(basis)
    rig = []
    for name, m in zip(names, inv):
        cols = m.astype(np.float64).reshape(4, 4).T          # column-major -> matrix
        bind = np.linalg.inv(cols) @ basis_inv
        rig.append({'name': name, 'bind': [float(x) for x in bind.T.reshape(-1)], 'inverse_bind': [f32(x) for x in m]})
    surfaces, textures = [], []
    for prim in glb.json['meshes'][0]['primitives']:
        mat = glb.json['materials'][prim['material']]
        mname = mat.get('name', '')
        if 'Skate' not in mname:
            continue
        tex = mat['pbrMetallicRoughness']['baseColorTexture']['index']
        w, h, rgba = glb.texture(glb.json['textures'][tex]['source'])
        textures.append({'width': w, 'height': h, 'rgba': list(rgba)})
        at = prim['attributes']
        pos = glb.components(at['POSITION']).astype(np.float32)
        nor = glb.components(at['NORMAL']).astype(np.float32)
        uv = glb.components(at['TEXCOORD_0']).astype(np.float32)
        jo = glb.components(at['JOINTS_0']).astype(np.uint32)
        we = glb.components(at['WEIGHTS_0']).astype(np.float32)
        n = len(pos) // 3
        uvh = uv.astype(np.float16).view(np.uint16).astype(np.uint32)
        verts = [{'position': [f32(x) for x in pos[v * 3:v * 3 + 3]], 'normal': [f32(x) for x in nor[v * 3:v * 3 + 3]],
                  'uv': int((uvh[v * 2] << 16) | uvh[v * 2 + 1]), 'joints': [int(x) for x in jo[v * 4:v * 4 + 4]],
                  'weights': [f32(x) for x in we[v * 4:v * 4 + 4]]} for v in range(n)]
        idx = glb.components(prim['indices']).astype(np.uint32).reshape(-1, 3)
        surfaces.append({'material': 'iw4l_skate/' + mname, 'texture': len(textures) - 1, 'vertices': verts,
                         'indices': [int(x) for a, b, c in idx for x in (a, c, b)]})
    if not surfaces:
        raise SystemExit('skater.glb has no skateboard surfaces')
    board = {'joints': [{'target': nm, 'origin': [0.0, 0.0, 0.0]} for nm in names], 'surfaces': surfaces, 'textures': textures}
    json.dump(rig, open(os.path.join(assets, 'rig.json'), 'w'), separators=(',', ':'))
    json.dump(board, open(os.path.join(assets, 'board.json'), 'w'), separators=(',', ':'))
    print(f'rig.json: {len(rig)} bones; board.json: {len(surfaces)} surfaces')


if __name__ == '__main__':
    export()
