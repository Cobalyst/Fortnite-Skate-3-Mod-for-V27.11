"""Decal layers of the skate.park pieces -> skate-data/dropper/decals.txt (+ their textures as PNG).

31 park materials are environmentParkDecal: besides the diffuse / normal / specular textures they have
a 'decal' texture (with alpha), drawn over the piece. The .skob files keep three textures per
material, so the decal is listed separately:
    <object id> <material index in the .skob> <decal texture id> <uv set the decal uses: 0 / 1>
The UV set is the one whose range fits 0..1 (decals are unique, not tiled).
"""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
import dropper_convert as dc
import dropper_db


def main():
    lines, tex = [], set()
    for kind in ('dynamic', 'static'):
        objs, mats = dropper_db.load(kind)
        for o in objs:
            arena = o['lods'][0]['arena']
            try:
                materials, meshes, *_ = dc.convert_arena(arena, mats, set())
            except Exception as e:
                print('FAILED', o['name'], e)
                continue
            # material index -> its mesh UVs
            for mi, (_flags, _tex, name) in enumerate(materials):
                m = dc.re.search(r'_(0x[0-9a-fA-F]{16})$', name)
                mv = m.group(1).lower() if m else None
                sp = mats.get(mv, {}).get('sp', {})
                dt = sp.get('decal')
                if not dt:
                    continue
                uvs0 = [mesh[4] for mesh in meshes if mesh[0] == mi]
                uvs1 = [mesh[5] for mesh in meshes if mesh[0] == mi]
                def spread(u):
                    if not u: return 1e9
                    a = np.concatenate(u)
                    return float(np.ptp(a[:, 0]) + np.ptp(a[:, 1]))
                uvset = 1 if spread(uvs1) <= spread(uvs0) else 0
                lines.append('%s %d %s %d' % (o['id'], mi, dt, uvset))
                tex.add(dt)
    open(dc.OUT + 'decals.txt', 'w').write('# object material decal-texture uv-set\n' + '\n'.join(lines) + '\n')
    print(len(lines), 'decal layers,', len(tex), 'textures; converted', dc.convert_textures(tex))


if __name__ == '__main__':
    main()
