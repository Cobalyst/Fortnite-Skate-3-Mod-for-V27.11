"""Carves every SNR stream from a Skate 3 SPLC .bnk (headers need not be 4-byte aligned):
codec byte <= 0x0f, channel byte <= 0x0f, a common sample rate, then sample count repeated at +4/+12.
python carve_splc.py <bank.bnk> <out_dir>"""
import os, re, struct, sys
src, out = sys.argv[1], sys.argv[2]
d = open(src, "rb").read()
os.makedirs(out, exist_ok=True)
rates = {48000: b'\xbb\x80', 44100: b'\xac\x44', 32000: b'\x7d\x00', 24000: b'\x5d\xc0', 22050: b'\x56\x22', 16000: b'\x3e\x80'}
starts = set()
for r, rb in rates.items():
    for m in re.finditer(re.escape(rb), d):
        s = m.start() - 2
        if s < 0 or s + 16 > len(d) or d[s] > 0x0f or d[s + 1] > 0x0f: continue
        n1, n2, n3 = struct.unpack_from(">III", d, s + 4)
        if 0 < n1 < r * 120 and n1 == n3:
            starts.add(s)
starts = sorted(starts)
base = os.path.splitext(os.path.basename(src))[0]
for i, s in enumerate(starts):
    e = starts[i + 1] if i + 1 < len(starts) else len(d)
    open(os.path.join(out, f"{base}_{i:03d}.sps"), "wb").write(d[s:e])
print(f"{len(starts)} streams carved from {base}")
