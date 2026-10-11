"""Builds skate-data/aems/: each Skate 3 skateboard ABK bank (copied as-is, for the AEMS module
emulator), its samples decoded once (no loop repeats) as <bank>/<bnk index>.wav, and <bank>.json
with per-sample rate / length / loop points. Subsong order = abk_modules order (same as vgmstream)."""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import json, os, re, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(__file__))
import abk_modules

SRC = (SD + r"\extracted\audio")
OUT = (SD + r"\aems")
V = VGMSTREAM
BANKS = ["GRINDS", "Sk8_Air_Flip_Tricks", "Seams_Bank", "WHEEL_SKID_BANK", "board_scrapes", "Brd_Squeaks",
         "FOOT_DRAG", "Rolling_Rattles", "sense_of_speed", "Treatments", "Bodyslide", "Foley_Cloth",
         "PatchBank_Rolling_Surfaces", "Common", "killed_it", "owned_it", "hom_slo_mo"]

def meta(path, s):
    out = subprocess.run([V, "-m", "-s", str(s), path], capture_output=True, text=True).stdout
    g = lambda k: (re.search(k + r": (\d+)", out) or [None, None])[1]
    return {"rate": int(g("sample rate") or 0), "channels": int(g("channels") or 0),
            "samples": int(g("stream total samples") or 0),
            "loop": "loop start" in out, "loop_start": int(g("loop start") or 0), "loop_end": int(g("loop end") or 0)}

os.makedirs(OUT, exist_ok=True)
for b in (sys.argv[1:] or BANKS):
    src = os.path.join(SRC, b + ".abk")
    if not os.path.exists(src):
        print("missing", b); continue
    shutil.copyfile(src, os.path.join(OUT, b + ".abk"))
    d, mods = abk_modules.parse(src)
    subsongs = {}
    for m in mods:
        for p in m["players"]:
            for no, idx, pr, az in p["sounds"]:
                subsongs.setdefault(idx, no)
    os.makedirs(os.path.join(OUT, b), exist_ok=True)
    info = {}
    for idx, no in sorted(subsongs.items()):
        wav = os.path.join(OUT, b, f"{idx}.wav")
        subprocess.run([V, "-i", "-s", str(no + 1), "-o", wav, src], capture_output=True)
        info[idx] = meta(src, no + 1)
    json.dump(info, open(os.path.join(OUT, b + ".json"), "w"), indent=0)
    print(f"{b}: {len(info)} samples, {sum(1 for v in info.values() if v['loop'])} looping")
