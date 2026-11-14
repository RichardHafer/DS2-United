"""Close the old letter cut-outs in the ribbon behind I and T of the United logo.

    python -I tools/fix_united_logo.py

The ribbon in assets/United_Logo.png still had holes from an earlier letter
layout. The band between rows 466 and 612 is drawn again: the ribbon's outline
and its two dividing gaps run straight between the clean rows above and below,
the soft shadow is the blurred ribbon like everywhere else, and the red letters
go back on top. Writes the full logo and the configurator's crop.
"""
import shutil
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.ndimage import binary_dilation, gaussian_filter

ROOT = Path(__file__).resolve().parent.parent
FULL = ROOT / "assets" / "United_Logo.png"
CONFIG = ROOT / "tools" / "RandomizerConfig" / "Logo.png"
TOP, BOTTOM, LEFT, RIGHT = 466, 612, 760, 1160
SS = 4  # subsamples per axis


def lerp(points, y):
    ys = [p[0] for p in points]
    return float(np.interp(y, ys, [p[1] for p in points]))


# Measured on the clean rows above and below the cut-outs.
EDGE_L = [(466, 800), (478, 803), (600, 816), (612, 812)]
EDGE_R = [(466, 1111), (478, 1108), (600, 1071), (612, 1067)]
GAP_B = [(466, 1021.0, 25.0), (478, 1013.5, 24.0), (600, 942.5, 21.0), (612, 936.0, 22.0)]


def gap_a(y):
    # Leaves the ribbon over its left edge; the measured part is rows 455..478.
    center = 875.0 - 0.69 * (y - 478.0)
    return center, 17.0


def inside(x, y):
    if not (lerp(EDGE_L, y) <= x <= lerp(EDGE_R, y)):
        return False
    c, w = gap_a(y)
    if abs(x - c) < w / 2:
        return False
    c = lerp([(p[0], p[1]) for p in GAP_B], y)
    w = lerp([(p[0], p[2]) for p in GAP_B], y)
    return abs(x - c) >= w / 2


def main():
    backup = FULL.with_name("United_Logo.orig.png")
    if not backup.exists():
        shutil.copy2(FULL, backup)
    src = np.array(Image.open(backup).convert("RGBA")).astype(np.float32)
    h, w, _ = src.shape

    # Ribbon coverage: the source's white everywhere, the new outline inside the band.
    white = (src[..., 0] > 200) & (src[..., 1] > 200) & (src[..., 2] > 200)
    cover = np.where(white, src[..., 3] / 255.0, 0.0)
    red = (src[..., 3] > 100) & (src[..., 0] > 150) & (src[..., 1] < 130)
    for y in range(TOP, BOTTOM + 1):
        for x in range(LEFT, RIGHT):
            n = 0
            for sy in range(SS):
                for sx in range(SS):
                    n += inside(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS)
            cover[y, x] = n / (SS * SS)

    # The source's shadow is close to the ribbon blurred with sigma 9 at about 65/255.
    shadow = np.clip(gaussian_filter(cover, 9.0) * 65.0, 0, 255)

    out = src.copy()
    band = np.zeros((h, w), bool)
    band[TOP:BOTTOM + 1, LEFT:RIGHT] = True
    c = cover[band]
    sa = shadow[band] / 255.0
    a = c + sa * (1.0 - c)
    rgb = (255.0 * c + 21.0 * sa * (1.0 - c)) / np.maximum(a, 1e-6)
    out[band, 0] = out[band, 1] = out[band, 2] = rgb
    out[band, 3] = a * 255.0

    # The letters and their glow go back on top, untouched.
    keep = binary_dilation(red, iterations=4) & band
    letters = np.zeros_like(src)
    letters[keep] = src[keep]
    base = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8), "RGBA")
    base.alpha_composite(Image.fromarray(letters.astype(np.uint8), "RGBA"))
    base.save(FULL)

    # The configurator shows the letter line: the same crop and scale as before.
    cfg_backup = CONFIG.with_name("Logo.orig.png")
    if not cfg_backup.exists():
        shutil.copy2(CONFIG, cfg_backup)
    base.crop((120, 338, 1799, 738)).resize((1200, 286), Image.LANCZOS).save(CONFIG)
    print("written", FULL, CONFIG)


if __name__ == "__main__":
    main()
