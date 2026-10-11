"""FN Skate 3 setup: builds the mod's skate-data folder from the player's own copy of Skate 3 (Xbox 360).

Finds Skate 3 on this PC (a disc image .iso, or an extracted game folder with default.xex and data\\),
copies out only the game files the mod needs, converts them, and checks every result against the
mod's manifest. Nothing is downloaded and no game files come with the mod.

    setup\\python\\python.exe setup\\fnskate_setup.py [path to Skate 3 .iso or default.xex]
"""
import glob, hashlib, json, os, re, shutil, string, struct, subprocess, sys, time

SETUP = os.path.dirname(os.path.abspath(__file__))
MOD = os.path.dirname(SETUP)
SD = os.path.join(MOD, 'skate-data')
WORK = os.path.join(SETUP, 'work')
LIB = os.path.join(SETUP, 'lib')
PY = sys.executable
VGM = os.path.join(SETUP, 'vgmstream', 'vgmstream-cli.exe')
CONVERT = os.path.join(SETUP, 'skate-convert', 'iw4l-skate-convert.exe')
os.environ['FNSKATE_DATA'] = SD
os.environ['FNSKATE_VGMSTREAM'] = VGM
sys.path.insert(0, LIB)
import bigfile, refpack, chunkref, xiso  # noqa: E402

NEEDED_GAME_FILES = ['default.xex', 'data/big', 'data/audio', 'data/anim', 'data/content/parkassets.big',
                     'data/content/marquee.big', 'data/content/createacharacter.big']
LOG = None


def say(text=''):
    print(text, flush=True)
    if LOG:
        LOG.write(text + '\n'); LOG.flush()


def fail(text):
    say(''); say('SETUP STOPPED: ' + text)
    say('Nothing in your game files was changed. Fix the problem above and run the setup again.')
    raise SystemExit(1)


# ----------------------------------------------------------------------------- finding Skate 3
def is_skate3_folder(folder):
    return (os.path.isfile(os.path.join(folder, 'default.xex')) and
            os.path.isfile(os.path.join(folder, 'data', 'big', 'miscboot.big')) and
            os.path.isfile(os.path.join(folder, 'data', 'content', 'parkassets.big')))


def is_skate3_iso(path):
    try:
        iso = xiso.Iso(path)
    except Exception:
        return False
    try:
        names = {p.lower() for p, _, _, d in iso.files() if not d}
        return 'default.xex' in names and 'data/big/miscboot.big' in names and 'data/content/parkassets.big' in names
    finally:
        iso.close()


def search_pc():
    """Looks for Skate 3 copies on every drive (folders up to 6 deep; skips Windows and program folders)."""
    skip = {'windows', '$recycle.bin', 'system volume information', 'programdata', 'program files',
            'program files (x86)', 'appdata', 'node_modules', '.git', 'msocache', 'recovery', 'perflogs'}
    found = []
    drives = [d + ':\\' for d in string.ascii_uppercase if os.path.exists(d + ':\\')]
    say('Looking for Skate 3 on drives ' + ', '.join(d[:2] for d in drives) + ' (this can take a minute)...')
    for drive in drives:
        stack = [(drive, 0)]
        while stack:
            folder, depth = stack.pop()
            try:
                entries = list(os.scandir(folder))
            except OSError:
                continue
            names = {e.name.lower() for e in entries}
            if 'default.xex' in names and 'data' in names and is_skate3_folder(folder):
                found.append(os.path.join(folder, 'default.xex'))
            for e in entries:
                try:
                    if e.is_dir(follow_symlinks=False):
                        if depth < 6 and e.name.lower() not in skip:
                            stack.append((e.path, depth + 1))
                    elif e.name.lower().endswith('.iso') and e.stat().st_size > 3 << 30 and is_skate3_iso(e.path):
                        found.append(e.path)
                except OSError:
                    pass
    return found


def choose_source(arg):
    if arg:
        p = arg.strip().strip('"')
        if os.path.isdir(p):
            p = os.path.join(p, 'default.xex')
        if p.lower().endswith('.iso') and os.path.isfile(p) and is_skate3_iso(p):
            return p
        if os.path.basename(p).lower() == 'default.xex' and is_skate3_folder(os.path.dirname(p)):
            return p
        fail(f'"{arg}" is not a Skate 3 (Xbox 360) .iso, or default.xex in an extracted Skate 3 folder.')
    found = search_pc()
    if not found:
        say('')
        say('No Skate 3 copy was found automatically.')
        say('Drag your Skate 3 .iso (or the default.xex of an extracted copy) into this window, then press Enter:')
        return choose_source(input('> '))
    if len(found) == 1:
        say('Found Skate 3: ' + found[0])
        return found[0]
    say('Found more than one Skate 3 copy:')
    for i, f in enumerate(found, 1):
        say(f'  {i}. {f}')
    while True:
        a = input(f'Type the number of the one to use (1-{len(found)}) and press Enter: ').strip()
        if a.isdigit() and 1 <= int(a) <= len(found):
            return found[int(a) - 1]


def game_folder(source):
    """An extracted game folder: the player's own, or the needed files copied out of the .iso."""
    if not source.lower().endswith('.iso'):
        return os.path.dirname(source)
    out = os.path.join(WORK, 'game')
    say('Copying the needed files out of the disc image (a few GB, takes a few minutes)...')
    iso = xiso.Iso(source)
    last = [0]

    def progress(done, total):
        pct = done * 100 // total
        if pct >= last[0] + 5:
            last[0] = pct
            say(f'  {pct}%')
    try:
        iso.extract(NEEDED_GAME_FILES, out, progress)
    finally:
        iso.close()
    if not is_skate3_folder(out):
        fail('the disc image did not contain the expected Skate 3 files.')
    return out


# ----------------------------------------------------------------------------- steps
def run(args, what, cwd=None, quiet=True):
    r = subprocess.run(args, cwd=cwd, capture_output=True, text=True, errors='replace')
    if LOG:
        LOG.write(f'$ {" ".join(map(str, args))}\n{r.stdout}\n{r.stderr}\n')
    if r.returncode != 0:
        say(r.stdout[-2000:]); say(r.stderr[-2000:])
        fail(f'{what} failed.')
    return r.stdout


def lib(script, *args, what=None):
    return run([PY, os.path.join(LIB, script), *map(str, args)], what or script, cwd=LIB)


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def engine_data(game):
    say('Converting the skate engine data (animations, physics, skater) - takes a few minutes...')
    out = os.path.join(WORK, 'convert')
    shutil.rmtree(out, ignore_errors=True)
    run([CONVERT, '--xex', os.path.join(game, 'default.xex'), '--out', out], 'the skate engine data conversion')
    shutil.rmtree(os.path.join(SD, 'assets'), ignore_errors=True)
    shutil.move(os.path.join(out, 'assets'), os.path.join(SD, 'assets'))


def archive_files(game, manifest):
    say('Copying the HUD, menu, sound and object files out of the game archives...')
    by_big = {}
    for e in manifest['extract']:
        by_big.setdefault(e['big'], []).append(e)
    for big, wanted in sorted(by_big.items()):
        path = os.path.join(game, 'data', *big.split('/'))
        _, items = bigfile.entries(path)
        with open(path, 'rb') as f:
            for e in wanted:
                i = e['index']
                if i >= len(items) or items[i][0] != e['entry']:
                    fail(f'{e["entry"]} is missing from {big} - this does not look like the right version of Skate 3.')
                _, off, size = items[i]
                f.seek(off)
                raw = f.read(size)
                data = refpack.decompress(raw) if e['mode'] == 'refpack' else chunkref.unpack(raw) if e['mode'] == 'chunkref' else raw
                if hashlib.sha256(data).hexdigest() != e['sha256']:
                    fail(f'{e["entry"]} in {big} is not the same as in the Skate 3 version the mod was made for.')
                dest = os.path.join(SD, *e['out'].split('/'))
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                open(dest, 'wb').write(data)
    # the object databases inside parkassets are chunkref-packed too
    park = os.path.join(SD, 'extracted', 'parkassets')
    os.makedirs(os.path.join(park, 'unpacked'), exist_ok=True)
    for f in glob.glob(os.path.join(park, '*')):
        if os.path.isfile(f) and not f.endswith('.rx2'):
            try:
                open(os.path.join(park, 'unpacked', os.path.basename(f)), 'wb').write(chunkref.unpack(open(f, 'rb').read()))
            except Exception:
                pass


def grain_sources(game):
    """sounds\\grains: each .grain of data\\audio\\grains.big without its header, decoded."""
    out = os.path.join(SD, 'sounds', 'grains')
    os.makedirs(out, exist_ok=True)
    path = os.path.join(game, 'data', 'audio', 'grains.big')
    _, items = bigfile.entries(path)
    with open(path, 'rb') as f:
        for name, off, size in items:
            if not name.lower().endswith('.grain'):
                continue
            f.seek(off)
            d = f.read(size)
            o = int.from_bytes(d[0:4], 'big')
            base = os.path.splitext(os.path.basename(name.replace('\\', '/')))[0]
            snr = os.path.join(WORK, 'grains', base + '.snr')
            os.makedirs(os.path.dirname(snr), exist_ok=True)
            open(snr, 'wb').write(d[o:])
            run([VGM, '-o', os.path.join(out, base + '.wav'), snr], 'decoding a rolling sound')


def hud_textures():
    say('Converting HUD and menu textures...')
    ex = os.path.join(SD, 'extracted')
    pairs = [('trickdisplay/trickdisplay2', 'textures'), ('homscoring/homscoring', 'homscoring'),
             ('hudphonelist/hudphonelist', 'hudphonelist'), ('actionoverlay2/actionoverlay2', 'actionoverlay2'),
             ('dropper_fe/objectdropper', 'objectdropper'), ('quickmenu_controls/sk8popup', 'sk8popup'),
             ('noise_search/vitextures', 'vitextures'), ('hom_skeleton/0x2c7f38180018000d', 'xray')]
    for d in ('menu_picker', 'menu_part_text_hilite', 'menu_part_glow', 'scrollbar', 'tabs', 'swatch_renderer',
              'button_item2'):
        pairs.append(('dropper_controls/' + d, d))
    pairs.append(('image_renderer/image_renderer', 'image_renderer'))
    for d in ('panel', 'highlight_selector', 'highlight_slider', 'highlight_option', 'highlight_color_grid', 'button_menu'):
        pairs.append(('quickmenu_controls/' + d, d))
    for src, hud in pairs:
        pack = os.path.join(ex, *src.split('/')) + '.rx2.bin'
        if os.path.exists(pack):
            lib('rw4_named.py', pack, os.path.join(SD, 'hud', hud), what=f'converting {hud} textures')
    # the x-ray bone sheet's source picture is the x-ray texture itself
    xr = os.path.join(SD, 'hud', 'xray')
    if os.path.exists(os.path.join(xr, '0x2c7f38180018000d.png')):
        shutil.copyfile(os.path.join(xr, '0x2c7f38180018000d.png'), os.path.join(xr, 'bones.png'))
    # button glyphs: Skate 3's own button art (miscboot button_*.png), drawn for "<name>" and "<name>_hud"
    btn = os.path.join(SD, 'hud', 'buttons')
    os.makedirs(btn, exist_ok=True)
    for f in glob.glob(os.path.join(ex, 'buttons', 'button_*.png')):
        n = os.path.basename(f)[len('button_'):-4]
        shutil.copyfile(f, os.path.join(btn, n + '.png'))
        shutil.copyfile(f, os.path.join(btn, n + '_hud.png'))


def fix_strings():
    """strings_en.txt as the mod reads it: lines with characters the converter wrote as U+FFFD get the game's
    original Latin-1 bytes (0xAB is the Skate logo glyph), and Christ Air is shown as Even Flow (a mod choice)."""
    def table(p):
        d = refpack.decompress(open(p, 'rb').read())
        c = struct.unpack_from('<I', d, 8)[0]
        b = 0x24 + c * 8
        o = {}
        for i in range(c):
            h, off = struct.unpack_from('<II', d, 0x24 + i * 8)
            e = d.index(b'\0', b + off)
            o[h] = d[b + off:e]
        return o
    mb = os.path.join(SD, 'extracted', 'miscboot')
    labels = table(os.path.join(mb, 'LANGUAGE_Labels_Global_skate3ng.BIN'))
    english = table(os.path.join(mb, 'LANGUAGE_English_Global_skate3ng.BIN'))
    raw = {labels[h].decode('latin-1'): english[h] for h in labels if h in english}
    p = os.path.join(SD, 'strings_en.txt')
    lines = open(p, 'rb').read().split(b'\n')
    for i, ln in enumerate(lines):
        if b'\xef\xbf\xbd' not in ln:
            continue
        cr = ln.endswith(b'\r')
        body = ln[:-1] if cr else ln
        tab = body.find(b'\t')
        if tab < 0:
            continue
        key = body[:tab].decode('latin-1').strip()
        if key in raw:
            lines[i] = body[:tab + 1] + raw[key] + (b'\r' if cr else b'')
    d = b'\n'.join(lines)
    old = b'ID_TRICK_GRAB_CHRIST_AIR\tChrist Air'
    d = d.replace(old + b'\r', b'ID_TRICK_GRAB_CHRIST_AIR\tEven Flow\r').replace(old + b'\n', b'ID_TRICK_GRAB_CHRIST_AIR\tEven Flow\n')
    open(p, 'wb').write(d)


def text_and_fonts():
    say('Converting text and fonts...')
    lib('skate3_strings.py', what='the game text')
    fix_strings()
    lib('skate3_font.py', 'futuraheavy', 'futuraglow', 'futurashadow', 'futuraoverprint', what='the fonts')


def skater_extras():
    say('Converting the board and the Hall of Meat x-ray...')
    lib('board_rig.py', what='the skater rig and board model')
    lib('board_textures.py', what='the board textures')
    lib('hom_skeleton.py', what='the x-ray skeleton')
    lib('hom_skeleton.py', '--atlas', what='the x-ray bone sheet')
    lib('hom_vignette.py', what='the x-ray vignette')


def dropper_objects():
    say('Converting the object dropper pieces (takes a few minutes)...')
    lib('dropper_convert.py', what='the object dropper pieces')
    lib('dropper_decals.py', what='the object dropper decals')
    lib('dropper_snaps.py', what='the object dropper snap points')
    # colour swatches and untinted pieces, from the converted game database
    s = open(os.path.join(SD, 'assets', 'private', 'stock', 'skater-collections.json'), encoding='utf-8', errors='replace').read()
    m = re.search(r'"Hash_FD2B311040A28477": \{"type": "Attrib::Types::Vector4", "data": "[0-9A-F]+", "array": \{[^}]*"items": \[([^\]]*)\]', s)
    items = re.findall(r'"([0-9A-F]{32})"', m.group(1)) if m else []
    cols = [struct.unpack('>4f', bytes.fromhex(it)) for it in items]
    open(os.path.join(SD, 'dropper', 'tints.txt'), 'w').write(
        '# skatepark_presentation/tint FD2B311040A28477: the 64 colour swatches (r g b a), in order\n' +
        ''.join('%.6f %.6f %.6f %.6f\n' % c for c in cols))
    nt = re.findall(r'\{"class": "Hash_B318DCD18A1EDA42", "key": "[^"]+", "parent": "default", "fields": \{"Hash_D11EA890B771D093": '
                    r'\{"type": "EA::Reflection::UInt64", "data": "([0-9A-F]+)"', s)
    open(os.path.join(SD, 'dropper', 'no_tint_models.txt'), 'w').write(
        '# terrain_no_tint_models (B318DCD18A1EDA42 / D11EA890B771D093)\n' + '\n'.join('0x' + x.lower() for x in nt) + '\n')


def sounds():
    say('Decoding the skateboard sounds (takes a few minutes)...')
    lib('build_aems_assets.py', what='the skateboard sound banks')
    audio = os.path.join(SD, 'extracted', 'audio')
    shutil.copyfile(os.path.join(SD, 'extracted', 'audio_all', 'fstep_skateshoe1_sm.abk'), os.path.join(audio, 'fstep_skateshoe1_sm.abk'))
    lib('build_aems_assets.py', 'fstep_skateshoe1_sm', what='the footstep sounds')
    for j in glob.glob(os.path.join(SD, 'aems', '*.json')):
        d = json.load(open(j))
        with open(j[:-5] + '.txt', 'w') as f:
            f.write('# bnk_index rate channels samples loop loop_start loop_end\n')
            for k, v in sorted(d.items(), key=lambda kv: int(kv[0])):
                f.write(f"{k} {v['rate']} {v['channels']} {v['samples']} {int(v['loop'])} {v['loop_start']} {v['loop_end']}\n")
    # every stream of every SPLC bank -> sounds\full\<bank>_NNN.wav
    full = os.path.join(SD, 'sounds', 'full')
    os.makedirs(full, exist_ok=True)
    for bnk in sorted(glob.glob(os.path.join(SD, 'extracted', 'audio_all', '*.bnk'))):
        b = os.path.splitext(os.path.basename(bnk))[0]
        carve = os.path.join(WORK, 'splc', b)
        lib('carve_splc.py', bnk, carve, what='splitting a sound bank')
        for sps in sorted(glob.glob(os.path.join(carve, '*.sps'))):
            snr = sps[:-4] + '.snr'
            os.replace(sps, snr)
            subprocess.run([VGM, '-o', os.path.join(full, os.path.basename(snr)[:-4] + '.wav'), snr], capture_output=True)
    # the individual sounds the sound settings name: a stream of a bank, or one of the sounds\full files
    raw = os.path.join(SD, 'sounds', 'raw')
    os.makedirs(raw, exist_ok=True)
    for name, src in json.load(open(os.path.join(SETUP, 'raw_sources.json'))).items():
        if 'full' in src:
            shutil.copyfile(os.path.join(full, src['full']), os.path.join(raw, name))
        else:
            subprocess.run([VGM, '-s', str(src['stream']), '-o', os.path.join(raw, name), os.path.join(SD, *src['bank'].split('/'))],
                           capture_output=True)


def check(manifest):
    say('Checking every file...')
    bad, missing = [], []
    for rel, want in manifest['runtime'].items():
        p = os.path.join(SD, *rel.split('/'))
        if not os.path.exists(p):
            missing.append(rel)
        elif sha(p) != want:
            bad.append(rel)
    return bad, missing


def tidy(manifest):
    """Leaves only what the mod reads (and the settings) in skate-data."""
    keep = {os.path.normcase(os.path.join(SD, *r.split('/'))) for r in manifest['runtime']}
    for root, dirs, files in os.walk(SD, topdown=False):
        for f in files:
            p = os.path.join(root, f)
            if os.path.normcase(p) not in keep:
                os.remove(p)
        if not os.listdir(root) and root != SD:
            os.rmdir(root)


def main():
    global LOG
    os.makedirs(WORK, exist_ok=True)
    LOG = open(os.path.join(SETUP, 'setup.log'), 'w', encoding='utf-8')
    say('FN Skate 3 setup')
    say('================')
    say('This builds the mod\'s Skate 3 data from YOUR copy of Skate 3. Nothing is downloaded.')
    say('')
    manifest = json.load(open(os.path.join(SETUP, 'manifest.json')))
    source = choose_source(sys.argv[1] if len(sys.argv) > 1 else '')
    t0 = time.time()
    game = game_folder(source)
    os.makedirs(SD, exist_ok=True)
    if not (os.environ.get('FNSKATE_RESUME') and os.path.exists(os.path.join(SD, 'assets', 'private', 'skater.glb'))):
        engine_data(game)   # (FNSKATE_RESUME=1, for testing the setup: keeps an earlier engine conversion)
    archive_files(game, manifest)
    hud_textures()
    text_and_fonts()
    skater_extras()
    dropper_objects()
    grain_sources(game)
    sounds()
    bad, missing = check(manifest)
    if bad or missing:
        for r in (missing + bad)[:30]:
            say(('  missing: ' if r in missing else '  different: ') + r)
        fail(f'{len(missing)} file(s) missing and {len(bad)} different from what the mod expects '
             '(a different Skate 3 version?). See setup\\setup.log.')
    tidy(manifest)
    shutil.rmtree(WORK, ignore_errors=True)
    say('')
    say(f'Done in {int(time.time() - t0) // 60} min. Skate 3 data is ready: you can start the mod now.')


if __name__ == '__main__':
    try:
        main()
    except SystemExit:
        raise
    except Exception as e:
        import traceback
        traceback.print_exc()
        if LOG:
            traceback.print_exc(file=LOG)
        fail(f'unexpected error: {e}')
