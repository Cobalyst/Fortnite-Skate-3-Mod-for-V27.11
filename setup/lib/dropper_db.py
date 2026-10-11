"""Parses Skate 3's park object databases (parkassets.big: dynamic_db / static_db .xml, chunkref-decoded)
into a list of objects: category, id, name, cap properties, LODs (arena id + material variants),
and the material table (texture ids per channel)."""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import re
import xml.etree.ElementTree as ET

PARK = (SD + '/extracted/parkassets/unpacked/')
DBS = {'dynamic': 'dynamic_db_x946ad45d_000.xml', 'static': 'static_db_xbfdba595_000.xml'}


def load(kind):
    text = open(PARK + DBS[kind], encoding='latin-1').read()
    root = ET.fromstring(text[text.index('<compositeasset'):])
    mats = {}
    for m in root.iter('mat'):
        mats[m.get('id')] = {'type': m.get('type'), 'sp': {s.get('chn'): s.get('id') for s in m.findall('sp')},
                             'ud': {c.tag: c.get('value') for u in m.findall('ud') for c in u}}
    objs = []
    for comp in root.iter('comp'):
        for mod in comp.findall('mod'):
            o = {'db': kind, 'comp': comp.get('n'), 'id': mod.get('id'), 'name': mod.get('n'),
                 'cap': {c.tag: c.get('value') for u in mod.findall('ud') for c in u}, 'lods': []}
            for lod in mod.findall('lod'):
                o['lods'].append({'idx': int(lod.get('idx')), 'id': lod.get('id'), 'arena': lod.get('arenaid'),
                                  'matvars': [(mv.get('id'), mv.get('n')) for mv in lod.iter('matvar')]})
            objs.append(o)
    return objs, mats


if __name__ == '__main__':
    import sys
    for k in DBS:
        objs, mats = load(k)
        print(k, len(objs), 'objects', len(mats), 'materials', sum(len(o['lods']) for o in objs), 'lods')
        for o in objs[:3]: print(' ', o)
