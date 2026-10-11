"""Converts every Skate 3 object-dropper / skate.park object (parkassets.big) into the mod's runtime format.

For each object (dynamic_db + static_db): its LOD0 RW4 arena is decoded into
  - render meshes (positions, normals, tangents, two UV sets, indices) with their material,
  - the RenderWare collision mesh (ClusteredMesh 0x80006: every triangle with its surface id),
  - the physics volumes (SimpleMappedArray 0x80002 entries: type, 3x4 transform, parameters),
  - the rigid-body blocks (0x40007 / 0x40006) kept raw for the physics port,
and written to skate-data/dropper/objects/<object id>.skob. Materials name their textures, which are
converted once each to skate-data/dropper/textures/<texture id>.png. A catalog of every object
(database, category, names, caps) goes to skate-data/dropper/objects.txt.

Space: Skate 3 model space (metres, Y up) is kept; the runtime converts to Unreal.

.skob layout (little-endian):
  'SKOB' u32 version(1)
  f32 bounds min[3], max[3]
  u32 material count; per material: u32 flags (1 = alpha test), 3 x (u16 len + chars): diffuse, normal, specular texture ids
  u32 mesh count; per mesh: u32 material, u32 vertex count, u32 index count,
      vertices: pos[3] normal[3] tangent[3] uv0[2] uv1[2] (13 f32), indices u32
  u32 collision triangle count; per triangle: v0[3] v1[3] v2[3] f32, u16 surface id, u8 edge codes[3], u8 flags
  u32 volume count; per volume: u32 type, f32 rot[9] (rows), f32 pos[3], f32 params[4], u32 surface id
  u32 raw rigid-body bytes (0x40007), bytes; u32 raw bytes (0x40006), bytes
  (v2) u32 grind spline count; per spline: u32 flags, u32 surface, f32 normal[3], u32 segment count,
      per segment 4 cubic Bezier control points x 3 f32 (Skate 3 0xeb001f grind data)
"""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
import rw4lib, dropper_db, chunkref

OUT = (SD + '/dropper/')
VERSION = 2

# Xbox 360 D3DDECLTYPE values used by these meshes
FLOAT3, FLOAT16_4, FLOAT16_2, HEND3N, SHORT2N = 0x2A23B9, 0x1A2360, 0x2C235F, 0x2A2190, 0x2C2159


def hend3n(w):
    """11:11:10 signed normalised (x bits 0-10, y 11-21, z 22-31)."""
    w = w.astype(np.int64)
    x = w & 0x7FF; y = (w >> 11) & 0x7FF; z = (w >> 22) & 0x3FF
    x = np.where(x >= 0x400, x - 0x800, x) / 1023.0
    y = np.where(y >= 0x400, y - 0x800, y) / 1023.0
    z = np.where(z >= 0x200, z - 0x400, z) / 511.0
    return np.stack([x, y, z], 1).astype(np.float32)


def decode_vertices(A, desc_i, buf_raw):
    d = A.d
    off, _ = A.body(desc_i)
    n = struct.unpack_from('>H', d, off + 8)[0]
    elems = []
    for e in range(n):
        o = off + 0x10 + e * 16
        eoff, typ, usage, _rwid, _stream = struct.unpack_from('>IIHHI', d, o)
        elems.append((eoff, typ, usage))
    stride = d[off + 0x10 + n * 16]
    count = len(buf_raw) // stride
    raw = np.frombuffer(buf_raw[:count * stride], np.uint8).reshape(count, stride)
    out = {}
    for eoff, typ, usage in elems:
        col = raw[:, eoff:]
        if typ == FLOAT3:
            v = col[:, :12].copy().view('>f4').astype(np.float32)
        elif typ == FLOAT16_4:
            v = col[:, :8].copy().view('>f2').astype(np.float32)
        elif typ == FLOAT16_2:
            v = col[:, :4].copy().view('>f2').astype(np.float32)
        elif typ == HEND3N:
            v = hend3n(col[:, :4].copy().view('>u4').reshape(-1))
        elif typ == SHORT2N:
            v = col[:, :4].copy().view('>i2').astype(np.float32) / 32767.0
        else:
            raise ValueError('vertex type %x' % typ)
        key = {0: 'pos', 3: 'normal', 5: 'uv', 6: 'tangent', 7: 'binormal'}.get(usage, 'u%d' % usage)
        if key in out:
            key += '2'
        out[key] = v
    return out


def dictionary(A):
    """Arena dictionary (0xeb000b): subref -> (64-bit id, type)."""
    d = A.d
    res = {}
    for i in A.of(0xeb000b):
        off, size = A.body(i)
        n = struct.unpack_from('>I', d, off)[0]
        p = off + 0x18
        for k in range(n):
            tag, hid, typ, sub = struct.unpack_from(">IQII", d, p + k * 0x18)
            if tag == 0xfeffffff:
                res[sub] = (hid, typ)
    return res


def material_names(A):
    """64-bit material id -> material name (from the 0xeb0005 material block)."""
    d = A.d
    res = {}
    for i in A.of(0xeb0005):
        off, size = A.body(i)
        blk = d[off:off + size]
        for m in re.finditer(rb'[\x20-\x7e]{6,}\x00', blk):
            pass
        # an id is followed by the offset of its name string (relative to the block)
        for p in range(0, size - 12, 4):
            hid = struct.unpack_from('>Q', blk, p)[0]
            no = struct.unpack_from('>I', blk, p + 8)[0]
            if 0 < no < size and hid and hid >> 32 not in (0, 0xdeadbeef):
                end = blk.find(b'\0', no)
                s = blk[no:end]
                if end > no and all(32 <= c < 127 for c in s) and b'_0x' in s:
                    res.setdefault(hid, s.decode())
    return res


def collision(A):
    d = A.d
    tris = []
    for i in A.of(0x80006):
        base, _ = A.body(i)
        clo = struct.unpack_from('>I', d, base + 0x34)[0]
        gran = struct.unpack_from('>f', d, base + 0x38)[0]
        gsz, ssz = d[base + 0x3e], d[base + 0x3f]
        ncl = struct.unpack_from('>I', d, base + 0x40)[0]
        for c in range(ncl):
            co = struct.unpack_from('>I', d, base + clo + 4 * c)[0]
            cb = base + co
            nu, uds, uns, nst, tot = struct.unpack_from('>5H', d, cb)
            nv, nn, mode = d[cb + 10], d[cb + 11], d[cb + 12]
            p = cb + 0x10
            # Vertex modes as the skate engine's RWCM reader (skate-data retail_collision.rs, from the
            # game's GetVertex): 1 = signed i32 base + UNSIGNED u16 deltas (vmrghh with zero),
            # converted to float before the granularity multiply; 0 = float4; 2 = i32 x granularity.
            if mode == 1:
                ox, oy, oz = struct.unpack_from('>3i', d, p); p += 12
                V = [(float(ox + x) * gran, float(oy + y) * gran, float(oz + z) * gran) for x, y, z in
                     (struct.unpack_from('>3H', d, p + k * 6) for k in range(nv))]
            elif mode == 0:
                V = [struct.unpack_from('>3f', d, p + k * 16) for k in range(nv)]
            elif mode == 2:
                V = [tuple(float(c) * gran for c in struct.unpack_from('>3i', d, p + k * 12)) for k in range(nv)]
            else:
                raise ValueError('cluster mode %d' % mode)
            u = cb + 0x10 + uns * 16
            end = u + uds
            while u < end:
                f = d[u]; t = f & 15; n = 3 if t == 1 else 4
                idx = list(d[u + 1:u + 1 + n]); u += 1 + n
                edges = list(d[u:u + n]) if f & 0x20 else [0] * n
                if f & 0x20: u += n
                if f & 0x40: u += gsz
                sid = 0
                if f & 0x80:
                    # 2-byte surface ids are little-endian (retail_collision.rs id(): u16::from_le_bytes)
                    sid = struct.unpack_from('<H', d, u)[0] if ssz == 2 else d[u]
                    u += ssz
                tris.append((V[idx[0]], V[idx[1]], V[idx[2]], sid, edges[:3], f))
                if n == 4:
                    tris.append((V[idx[0]], V[idx[2]], V[idx[3]], sid, [edges[2], edges[3], 0], f))
    return tris


def volumes(A):
    d = A.d
    out = []
    for i in A.of(0x80002):
        off, size = A.body(i)
        cnt = struct.unpack_from('>I', d, off + 0x24)[0]
        vo = struct.unpack_from('>I', d, off + 0x30)[0]
        for k in range(cnt):
            o = off + vo + k * 0x60
            if o + 0x60 > off + size:
                break
            rot = [struct.unpack_from('>f', d, o + r * 16 + c * 4)[0] for r in range(3) for c in range(3)]
            pos = struct.unpack_from('>3f', d, o + 0x30)
            typ = struct.unpack_from('>I', d, o + 0x40)[0]
            params = struct.unpack_from('>4f', d, o + 0x44)
            sid = struct.unpack_from('>I', d, o + 0x58)[0]
            if typ == 6:
                continue  # aggregate pointer (to the clustered mesh / another array)
            out.append((typ, rot, pos, params, sid))
    return out


def grind_splines(A):
    """0xeb001f: u32 count, u32 total control points, 0, count, u32 header offset, control point
    offset, normal offset, end. Headers 0x50: AABB min/max (float4), +0x30 control offset, +0x38
    normal offset, +0x3c surface, +0x44 flags. Control points are float4, four per cubic segment."""
    d = A.d
    out = []
    for i in A.of(0xeb001f):
        off, size = A.body(i)
        n = struct.unpack_from('>I', d, off)[0]
        ho, co, no = struct.unpack_from('>3I', d, off + 0x10)
        heads = []
        for k in range(n):
            h = off + ho + k * 0x50
            c_off, _z, n_off, surface, _z2, flags = struct.unpack_from('>6I', d, h + 0x30)
            heads.append((c_off, n_off, surface, flags))
        starts = sorted(set(h[0] for h in heads)) + [no]
        for c_off, n_off, surface, flags in heads:
            end = min(x for x in starts if x > c_off)
            cps = [struct.unpack_from('>3f', d, off + c_off + j * 16) for j in range((end - c_off) // 16)]
            segs = [cps[j:j + 4] for j in range(0, len(cps) - 3, 4)]
            normal = struct.unpack_from('>3f', d, off + n_off)
            out.append((flags, surface, normal, segs))
    return out


def convert_arena(arena, mats_xml, tex_needed):
    A = rw4lib.Arena(rw4lib.PARK + arena + '.rx2')
    d = A.d
    dic = dictionary(A)
    mnames = material_names(A)
    materials, mat_index, meshes = [], {}, []
    lo, hi = np.full(3, 1e30, np.float32), np.full(3, -1e30, np.float32)
    for mi in A.of(0xeb0023):
        off, _ = A.body(mi)
        sub, vdesc, _va, ib, vb = struct.unpack_from('>5I', d, off + 0x24)
        # draw ranges (+0x38 count, 16 bytes each from +0x60: primitive, 0, first index, index count);
        # most meshes have one, some (ai_MegaQuaterPipe90Corner) several
        nranges = struct.unpack_from('>I', d, off + 0x38)[0]
        vbo, vbs = A.body(vb - 1)
        ibo, ibs = A.body(ib - 1)
        V = decode_vertices(A, vdesc, d[vbo:vbo + vbs])
        allidx = np.frombuffer(d[ibo:ibo + ibs - ibs % 2], '>u2').astype(np.uint32)
        parts = []
        for r in range(max(1, nranges)):
            prim, _z, first, count = struct.unpack_from('>4I', d, off + 0x60 + 16 * r)
            if prim != 4:
                raise ValueError('primitive type %d' % prim)
            parts.append(allidx[first:first + count])
        idx = np.concatenate(parts)
        pos = V['pos'][:, :3]
        uv = V.get('uv', np.zeros((len(pos), 2), np.float32))
        uv0 = uv[:, :2]
        uv1 = uv[:, 2:4] if uv.shape[1] >= 4 else (V['uv2'][:, :2] if 'uv2' in V else uv0)
        tan = V.get('tangent', np.zeros((len(pos), 3), np.float32))
        if 'normal' in V:
            nrm = V['normal']
        else:
            b = V.get('binormal', np.zeros_like(tan))
            nrm = np.cross(tan, b)
            # the stored frame's handedness: pick the sign that agrees with the faces
            tri = idx.reshape(-1, 3)
            fn = np.cross(pos[tri[:, 1]] - pos[tri[:, 0]], pos[tri[:, 2]] - pos[tri[:, 0]])
            agree = np.einsum('ij,ij->i', fn, nrm[tri[:, 0]]).sum()
            if agree < 0:
                nrm = -nrm
        ln = np.linalg.norm(nrm, axis=1, keepdims=True); nrm = nrm / np.maximum(ln, 1e-9)
        lo = np.minimum(lo, pos.min(0)); hi = np.maximum(hi, pos.max(0))
        # material
        hid = dic.get(sub, (None, None))[0]
        name = mnames.get(hid, '')
        m = re.search(r'_(0x[0-9a-fA-F]{16})$', name)
        mv = m.group(1).lower() if m else None
        if mv not in mat_index:
            xm = mats_xml.get(mv, {})
            sp = xm.get('sp', {})
            flags = 1 if 'AlphaTest' in (xm.get('type') or '') or 'transparency' in sp else 0
            tex = [sp.get('diffuse', ''), sp.get('normal', ''), sp.get('specular', '')]
            for t in tex:
                if t: tex_needed.add(t)
            mat_index[mv] = len(materials)
            materials.append((flags, tex, name))
        meshes.append((mat_index[mv], pos, nrm, tan, uv0, uv1, idx))
    tris = collision(A)
    vols = volumes(A)
    rb = [d[slice(*((lambda o, s: (o, o + s))(*A.body(i))))] for i in A.of(0x40007)]
    rb2 = [d[slice(*((lambda o, s: (o, o + s))(*A.body(i))))] for i in A.of(0x40006)]
    if tris:
        tv = np.array([[t[0], t[1], t[2]] for t in tris], np.float32).reshape(-1, 3)
        lo = np.minimum(lo, tv.min(0)); hi = np.maximum(hi, tv.max(0))
    return materials, meshes, tris, vols, (rb[0] if rb else b''), (rb2[0] if rb2 else b''), lo, hi, grind_splines(A)


def write_skob(path, conv):
    materials, meshes, tris, vols, rb, rb2, lo, hi, splines = conv
    def s16(s):
        b = s.encode(); return struct.pack('<H', len(b)) + b
    out = bytearray(b'SKOB' + struct.pack('<I', VERSION))
    out += struct.pack('<6f', *lo, *hi)
    out += struct.pack('<I', len(materials))
    for flags, tex, _name in materials:
        out += struct.pack('<I', flags) + b''.join(s16(t) for t in tex)
    out += struct.pack('<I', len(meshes))
    for mat, pos, nrm, tan, uv0, uv1, idx in meshes:
        out += struct.pack('<III', mat, len(pos), len(idx))
        verts = np.concatenate([pos, nrm, tan, uv0, uv1], 1).astype('<f4')
        out += verts.tobytes() + idx.astype('<u4').tobytes()
    out += struct.pack('<I', len(tris))
    for a, b, c, sid, edges, f in tris:
        out += struct.pack('<9fH3BB', *a, *b, *c, sid & 0xFFFF, *(e & 0xFF for e in edges), f & 0xFF)
    out += struct.pack('<I', len(vols))
    for typ, rot, pos, params, sid in vols:
        out += struct.pack('<I9f3f4fI', typ, *rot, *pos, *params, sid)
    out += struct.pack('<I', len(rb)) + rb + struct.pack('<I', len(rb2)) + rb2
    out += struct.pack('<I', len(splines))
    for flags, surface, normal, segs in splines:
        out += struct.pack('<II3fI', flags, surface, *normal, len(segs))
        for seg in segs:
            for cp in seg:
                out += struct.pack('<3f', *cp)
    open(path, 'wb').write(out)


def convert_textures(ids):
    import rw4_named, io, contextlib, tempfile, shutil
    os.makedirs(OUT + 'textures', exist_ok=True)
    tmp = tempfile.mkdtemp(dir=OUT)
    done = 0
    for t in sorted(ids):
        dst = OUT + 'textures/%s.png' % t
        if os.path.exists(dst):
            continue
        src = rw4lib.PARK + t + '.rx2'
        if not os.path.exists(src):
            print('  missing texture', t); continue
        b = tmp + '/' + t + '.bin'
        open(b, 'wb').write(chunkref.unpack(open(src, 'rb').read()))
        od = tmp + '/' + t
        with contextlib.redirect_stdout(io.StringIO()):
            sys.argv = ['rw4_named.py', b, od]
            rw4_named.main()
        pngs = [f for f in os.listdir(od) if f.endswith('.png')] if os.path.isdir(od) else []
        if pngs:
            shutil.move(od + '/' + pngs[0], dst); done += 1
        else:
            print('  no image in', t)
    shutil.rmtree(tmp, ignore_errors=True)
    return done


def main():
    os.makedirs(OUT + 'objects', exist_ok=True)
    tex_needed = set()
    lines = []
    failed = 0
    for kind in ('dynamic', 'static'):
        objs, mats = dropper_db.load(kind)
        for o in objs:
            lod = o['lods'][0]
            try:
                conv = convert_arena(lod['arena'], mats, tex_needed)
            except Exception as e:
                print('FAILED', o['name'], lod['arena'], e); failed += 1
                continue
            write_skob(OUT + 'objects/%s.skob' % o['id'], conv)
            materials, meshes, tris, vols, rb, rb2, lo, hi, splines = conv
            caps = ';'.join('%s=%s' % (k.replace('cap.', ''), v) for k, v in o['cap'].items() if v)
            lines.append('\t'.join([kind, o['comp'], o['id'], o['name'], lod['arena'], caps,
                                    '%d' % len(meshes), '%d' % sum(len(m[6]) // 3 for m in meshes), '%d' % len(tris), '%d' % len(vols), '%d' % len(splines),
                                    ' '.join('%.3f' % x for x in (*lo, *hi))]))
    open(OUT + 'objects.txt', 'w').write('# db\tcategory\tid\tname\tarena\tcaps\tmeshes\trender tris\tcollision tris\tvolumes\tbounds\n' + '\n'.join(lines) + '\n')
    print(len(lines), 'objects,', failed, 'failed,', len(tex_needed), 'textures needed')
    print(convert_textures(tex_needed), 'textures converted')


if __name__ == '__main__':
    main()
