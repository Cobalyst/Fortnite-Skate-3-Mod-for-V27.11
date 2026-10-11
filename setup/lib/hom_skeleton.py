"""Converts Skate 3's Hall of Meat x-ray skeleton (the 19 "dem_bones_*" RW4 meshes from
marquee.big, material character.default_hom) into skate-data/hud/xray/skeleton.bin for the mod.

RW4 objects used (type ids from the object table, see rw4_named.py):
  0x10031 #1  vertex buffer, #2 index buffer (offsets into the data section)
  0x200E9     vertex declaration: u16 element count at +0x08, 16-byte elements from +0x10
              (u32 offset, u32 D3DDECLTYPE, u16 usage, u16 id, u32 stream); stride = byte after
  0xEB0023    mesh: index count u32 +0x6C (triangle list), bone palette u16 x count(+0x48) at +0x4C
  0xEB0001    skeleton: matrix offset +0x20 (inverse binds, identical to the skater rig's),
              name table offset +0x28 (offsets from the object), bone count u16 +0x34
Vertex (stride 24): SHORT4N position = n/32767*2 + (0, 0.8, 0) (skater bind space, metres, Y up),
HEND3N normal, SHORT2N uv, UBYTE4 palette indices, UBYTE4 weights (sum 255).

Output (little-endian): "XRAY", u32 version 1, u32 bone count, u32 piece count;
bones: char[24] skater rig names; pieces: char[24] name, u32 vertex count, u32 index count,
vertices {f32 pos[3] Unreal cm, f32 normal[3], f32 uv[2], u8 bone[4], u8 weight[4]}, u16 indices
(winding flipped for Unreal's handedness). Finger bones (absent from the skater rig) follow their hand.
python hom_skeleton.py"""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import glob
import json
import os
import re
import struct

ROOT = SD
SRC = os.path.join(ROOT, "extracted", "hom_skeleton")
OUT = os.path.join(ROOT, "hud", "xray", "skeleton.bin")
POS_SCALE, POS_OFFSET = 2.0, (0.0, 0.8, 0.0)


def rig_name(name, rig):
    up = name.upper()
    if up in rig:
        return up
    for side in ("LEFT", "RIGHT"):
        if up.startswith(side + "HAND") or up.startswith(side + "INHAND"):
            return side + "HAND"
    raise ValueError(f"bone {name} has no skater rig bone")


def hend3n(v):
    def s(x, bits):
        return (x - (1 << bits) if x & (1 << (bits - 1)) else x) / ((1 << (bits - 1)) - 1)
    return s(v & 0x7FF, 11), s((v >> 11) & 0x7FF, 11), s(v >> 22, 10)


def piece(path, rig, bones):
    d = open(path, "rb").read()
    u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    data, table = u32(0x44), u32(0x30)
    ents = [[u32(table + 24 * i + k) for k in range(0, 24, 4)] for i in range(u32(0x20))]
    of = lambda t: [e[0] for e in ents if e[5] == t]
    vb, ib = [data + o for o in of(0x10031)][:2]
    mesh, skel, decl = of(0xEB0023)[0], of(0xEB0001)[0], of(0x200E9)[0]
    name = re.search(rb"dem_bones_[a-z_]+?(?=_0x)", d).group().decode()

    elems = {}
    n = struct.unpack_from(">H", d, decl + 8)[0]
    for i in range(n):
        off, typ, usage, _, _ = struct.unpack_from(">IIHHI", d, decl + 0x10 + 16 * i)
        elems[usage] = (off, typ)
    stride = d[decl + 0x10 + 16 * n]
    assert elems[0][1] == 0x1A215A and elems[3][1] == 0x2A2190 and elems[5][1] == 0x2C2159, (name, elems)
    assert elems[2][1] == 0x1A2286 and elems[1][1] == 0x1A2286 and stride == 24, (name, elems, stride)

    cnt, mo, no = struct.unpack_from(">H", d, skel + 0x34)[0], u32(skel + 0x20), u32(skel + 0x28)
    names = []
    for i in range(cnt):
        p = skel + u32(skel + no + 4 * i)
        names.append(d[p:d.index(b"\0", p)].decode())
    pal = struct.unpack_from(">%dH" % u32(mesh + 0x48), d, mesh + u32(mesh + 0x4C))
    index_count = u32(mesh + 0x6C)
    assert index_count % 3 == 0

    def global_bone(local):
        rn = rig_name(names[pal[local]], rig)
        if rn not in bones:
            bones.append(rn)
        return bones.index(rn)

    indices = struct.unpack_from(">%dH" % index_count, d, ib)
    nv = max(indices) + 1
    verts = []
    lo, hi = [9e9] * 3, [-9e9] * 3
    for v in range(nv):
        b = vb + v * stride
        sx, sy, sz, _ = struct.unpack_from(">4h", d, b + elems[0][0])
        p = [sx / 32767 * POS_SCALE + POS_OFFSET[0], sy / 32767 * POS_SCALE + POS_OFFSET[1], sz / 32767 * POS_SCALE + POS_OFFSET[2]]
        lo = [min(a, c) for a, c in zip(lo, p)]
        hi = [max(a, c) for a, c in zip(hi, p)]
        nx, ny, nz = hend3n(u32(b + elems[3][0]))
        tu, tv = struct.unpack_from(">2h", d, b + elems[5][0])
        bi = d[b + elems[2][0]:b + elems[2][0] + 4]
        bw = d[b + elems[1][0]:b + elems[1][0] + 4]
        gb = [global_bone(bi[k]) if bw[k] else 0 for k in range(4)]
        verts.append(struct.pack("<8f4B4B", p[0] * 100, p[2] * 100, p[1] * 100, nx, nz, ny,
                                 tu / 32767, tv / 32767, *gb, *bw))
    # The mesh's own bounding box (first 0x20 bytes of the mesh object) must enclose the decode.
    bmin, bmax = struct.unpack_from(">3f", d, mesh), struct.unpack_from(">3f", d, mesh + 0x10)
    err = max(max(abs(a - c) for a, c in zip(lo, bmin)), max(abs(a - c) for a, c in zip(hi, bmax)))
    flipped = [i for t in range(0, index_count, 3) for i in (indices[t], indices[t + 2], indices[t + 1])]
    print(f"  {name}: {nv} vertices, {index_count // 3} triangles, bbox error {err * 1000:.1f} mm")
    assert err < 0.01, name
    return name, verts, flipped


def main():
    rig = {b["name"]: b for b in json.load(open(os.path.join(ROOT, "assets", "private", "rig.json")))
           } if os.path.exists(os.path.join(ROOT, "assets", "private", "rig.json")) else \
        {b["name"]: b for b in json.load(open(glob.glob(os.path.join(ROOT, "**", "rig.json"), recursive=True)[0]))}
    bones, pieces = [], []
    for path in sorted(glob.glob(os.path.join(SRC, "*.rx2.bin"))):
        d = open(path, "rb").read()
        if b"dem_bones_" in d and struct.pack(">I", 0xEB0023) in d:  # meshes (not the texture pack)
            pieces.append(piece(path, rig, bones))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(b"XRAY" + struct.pack("<3I", 1, len(bones), len(pieces)))
        for b in bones:
            f.write(b.encode().ljust(24, b"\0"))
        for name, verts, idx in pieces:
            f.write(name.encode().ljust(24, b"\0") + struct.pack("<2I", len(verts), len(idx)))
            f.write(b"".join(verts) + struct.pack("<%dH" % len(idx), *idx))
    print(f"{len(pieces)} pieces, {len(bones)} bones -> {OUT}")


if __name__ == "__main__" and "--atlas" not in __import__("sys").argv:
    main()


# --- x-ray shading atlas -------------------------------------------------------------------------
# Skate 3's defaulthom_defaultPS (shaders_final.big), per colour channel, without fog:
#   t = texel^2; base = boneClr * max(t, i_colorParams.y); g = 1 - (1 - sat(N.V))^m_params[1].w
#   x = base + i_colorParams.x * t * g * (t - base);  x' = 2 * m_params[0].y * x
#   L = 1 - (1 - x')^2 (x' <= 1) or 0.25 x' + 0.75 (HDR soft knee, stored as sqrt(L / 2))
# Constants: i_colorParams = (0.5, 0.3) and boneClr white (0.8, 0.8, 0.8) for injury levels 2-3,
# red (0.8, 0.05, 0) for 4+ (post_fx-free attrib hall_of_meat_skeleton); m_params[0].y = 1 and
# m_params[1].w = 2 (material_character/default_hom). The atlas holds the texture shaded at
# ATLAS_LEVELS values of g (columns) for white (rows 0-1) and red (rows 2-3), as display sRGB.
ATLAS_LEVELS = 8
COLOURS = ((0.8, 0.8, 0.8), (0.8, 0.05, 0.0))
COLOR_PARAMS = (0.5, 0.3)
EXPOSURE = 1.0
GRADE_SATURATION = 1.0  # the grade is applied to the world by the camera; the reference shows the bones unwashed
GRADE_MULTIPLY = (1.0, 1.0, 1.0)


def read_png_rgba(path):
    import zlib
    d = open(path, "rb").read()
    w, h = struct.unpack(">II", d[16:24])
    idat, p = b"", 8
    while p < len(d):
        n = struct.unpack(">I", d[p:p + 4])[0]
        if d[p + 4:p + 8] == b"IDAT":
            idat += d[p + 8:p + 8 + n]
        p += 12 + n
    raw = zlib.decompress(idat)
    assert all(raw[y * (w * 4 + 1)] == 0 for y in range(h)), "only unfiltered PNGs"
    return w, h, b"".join(raw[y * (w * 4 + 1) + 1:(y + 1) * (w * 4 + 1)] for y in range(h))


def shade(texel, clr, g):
    t = (texel / 255.0) ** 2
    base = clr * max(t, COLOR_PARAMS[1])
    x = 2.0 * EXPOSURE * (base + COLOR_PARAMS[0] * t * g * (t - base))
    lin = 1 - (1 - x) ** 2 if x <= 1 else 0.25 * x + 0.75
    lin = min(1.0, max(0.0, lin))
    s = lin * 12.92 if lin <= 0.0031308 else 1.055 * lin ** (1 / 2.4) - 0.055
    return s


def bake_atlas():
    sys_path = os.path.join(ROOT, "hud", "xray")
    w, h, px = read_png_rgba(os.path.join(sys_path, "bones.png"))
    tiles_x = 4
    tiles_y = (2 * ATLAS_LEVELS) // tiles_x
    out = bytearray(w * tiles_x * h * tiles_y * 4)
    stride = w * tiles_x * 4
    for ci, clr in enumerate(COLOURS):
        for lv in range(ATLAS_LEVELS):
            g = lv / (ATLAS_LEVELS - 1)
            lut = [[0] * 256 for _ in range(3)]
            for v in range(256):
                # The skeleton is part of the scene, so the Hall of Meat colour grade
                # (post_fx colour_matrix_hall_of_meat: saturation 0.5, multiply (1, 1, 1.2)) applies to it.
                rgb = [shade(v, clr[c], g) for c in range(3)]
                lum = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]
                for c in range(3):
                    graded = (lum + GRADE_SATURATION * (rgb[c] - lum)) * GRADE_MULTIPLY[c]
                    lut[c][v] = int(round(min(1.0, max(0.0, graded)) * 255))
            tile = ci * ATLAS_LEVELS + lv
            ox, oy = (tile % tiles_x) * w, (tile // tiles_x) * h
            for y in range(h):
                row = px[y * w * 4:(y + 1) * w * 4]
                o = (oy + y) * stride + ox * 4
                line = bytearray(w * 4)
                line[0::4] = bytes(lut[0][v] for v in row[0::4])
                line[1::4] = bytes(lut[1][v] for v in row[1::4])
                line[2::4] = bytes(lut[2][v] for v in row[2::4])
                line[3::4] = b"\xff" * w
                out[o:o + w * 4] = line
    sys.path.insert(0, os.path.dirname(__file__))
    from skate3_font import write_png
    write_png(os.path.join(sys_path, "bones_atlas.png"), w * tiles_x, h * tiles_y, bytes(out))
    print(f"atlas {w * tiles_x}x{h * tiles_y}: {ATLAS_LEVELS} rim levels x {len(COLOURS)} colours")


if __name__ == "__main__" and "--atlas" in __import__("sys").argv:
    import sys
    bake_atlas()
