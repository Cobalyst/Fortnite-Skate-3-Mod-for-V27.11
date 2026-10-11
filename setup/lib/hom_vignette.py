"""Bakes Skate 3's Hall of Meat vignette (postfx_uberPS, f_Hom_Params / f_Hom_Vig_Params) into
skate-data/hud/xray/hom_vignette.png: 4 x 4 tiles = 16 strengths (0 .. 1), each a multiply factor
for the whole screen (drawn with modulate blending).

Shader, per pixel with p = screen position in -1..1:
  d = max(0, |p.x| - start) + max(0, |p.y| - start)      start = hom_vignette_start_radius (0.2)
  v = d * strength                                        strength = f_Hom_Params.w (0 + fade, 1 while on)
  colour = lerp(colour, colour * hom_vignette_color, v)   hom_vignette_color = (0.4, 0.2, 0.2)
The fade (0x827D20B0): 1 while the effect is on, then -0.02 per frame (hom 0x517E...).
python hom_vignette.py"""
from fnpaths import SD, VGMSTREAM  # noqa: E402
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from skate3_font import write_png  # noqa: E402

START = 0.2
COLOUR = (0.4, 0.2, 0.2)
TILE, TILES = 256, 4


def main():
    size = TILE * TILES
    px = bytearray(size * size * 4)
    for level in range(TILES * TILES):
        strength = level / (TILES * TILES - 1)
        ox, oy = (level % TILES) * TILE, (level // TILES) * TILE
        for y in range(TILE):
            py = (y + 0.5) / TILE * 2 - 1
            dy = max(0.0, abs(py) - START)
            for x in range(TILE):
                pxv = (x + 0.5) / TILE * 2 - 1
                v = (max(0.0, abs(pxv) - START) + dy) * strength
                o = ((oy + y) * size + ox + x) * 4
                for c in range(3):
                    f = min(1.0, max(0.0, 1 + v * (COLOUR[c] - 1)))
                    px[o + c] = int(round(f * 255))
                px[o + 3] = 255
    out = os.path.join(SD, "hud", "xray", "hom_vignette.png")
    write_png(out, size, size, bytes(px))
    print("wrote", out)


if __name__ == "__main__":
    main()
