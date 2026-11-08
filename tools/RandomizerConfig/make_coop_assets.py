"""Builds assets/CoopMod_Logo.png from assets/DS2Randomizer_Logo.png, then Logo.png and App.ico.

The helix and the red marble of the randomizer logo stay; the wordmark becomes
UNITED in a wide face (Bahnschrift Bold, stretched). Run from the
repository root: python tools/RandomizerConfig/make_coop_assets.py
The original asset is only read. Needs Pillow and numpy.
"""
import os
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_assets  # noqa: E402

ASSETS = os.path.join(HERE, "..", "..", "assets")
SOURCE = os.path.join(ASSETS, "DS2Randomizer_Logo.png")
TARGET = os.path.join(ASSETS, "United_Logo.png")
FONT = r"C:\Windows\Fonts\bahnschrift.ttf"
WORD = "UNITED"


def marble(rgba, red, box, size):
    """A red marble field of `size`, filled from the old wordmark's own pixels."""
    x0, y0, x1, y1 = box
    band = rgba[y0:y1 + 1, x0:x1 + 1, :3].astype(np.float32)
    have = red[y0:y1 + 1, x0:x1 + 1].copy()
    # fill the gaps between the old letters from their neighbours
    for step in range(1, 400):
        if have.all():
            break
        for dx in (step, -step):
            src = np.roll(band, dx, axis=1)
            ok = np.roll(have, dx, axis=1) & ~have
            band[ok] = src[ok]
            have |= ok
    tile = Image.fromarray(band.clip(0, 255).astype(np.uint8), "RGB")
    out = Image.new("RGB", size)
    for x in range(0, size[0], tile.width):
        for y in range(0, size[1], tile.height):
            out.paste(tile, (x, y))
    return out


def wordmark_mask(width, height):
    """UNITED as a white-on-black mask, letters spaced like the original."""
    font = ImageFont.truetype(FONT, 400)
    try:
        font.set_variation_by_name("Bold")
    except Exception:
        pass
    # every glyph in the cap height box of H, so a hyphen stays in the middle
    _, top, _, bottom = font.getbbox("H")
    glyphs = []
    for ch in WORD:
        l, _, r, _ = font.getbbox(ch)
        if ch == " ":
            l, r = 0, int(font.getlength(" "))
        im = Image.new("L", (max(r - l, 1) + 4, bottom - top + 4), 0)
        ImageDraw.Draw(im).text((2 - l, 2 - top), ch, font=font, fill=255)
        glyphs.append(im)
    cap = max(g.height for g in glyphs)
    stretch = 1.55            # the original face is extended
    gap = int(0.36 * cap)     # and widely tracked
    scaled = [g.resize((int(g.width * stretch), cap), Image.LANCZOS) for g in glyphs]
    total = sum(g.width for g in scaled) + gap * (len(scaled) - 1)
    mask = Image.new("L", (total, cap), 0)
    x = 0
    for g in scaled:
        mask.paste(g, (x, 0))
        x += g.width + gap
    return mask.resize((width, height), Image.LANCZOS)


def main():
    im = Image.open(SOURCE).convert("RGBA")
    rgba = np.array(im)
    x0, y0, x1, y1, red = make_assets.wordmark_box(rgba)
    color = make_assets.accent(rgba, red)
    # the helix without the old letters and their light shadow: the band is
    # cleared, except the helix itself where no letter was near
    base = rgba.copy()
    near = np.array(Image.fromarray((red * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(21))) > 0
    band = np.zeros(red.shape, bool)
    band[max(0, y0 - 14):y1 + 15, :] = True
    helix_cols = np.zeros(red.shape, bool)
    helix_cols[:, 780:1140] = True
    base[band & (~helix_cols | near), 3] = 0
    # the new word: same cap height, centred on the helix, a little wider than before
    cap = y1 - y0 + 1
    width = min(int((x1 - x0) * 1.12), rgba.shape[1] - 120)
    mask = wordmark_mask(width, cap)
    # a slightly worn edge like the original
    m = np.array(mask).astype(np.float32) / 255.0
    rng = np.random.default_rng(7)
    noise = np.array(Image.fromarray((rng.random(m.shape) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2))).astype(np.float32) / 255.0
    m = np.clip((m - 0.5) * 6 + (noise - 0.5) * 0.9 + 0.5, 0, 1)
    texture = np.array(marble(rgba, red, (x0, y0, x1, y1), (width, cap))).astype(np.float32)
    left = (rgba.shape[1] - width) // 2
    out = base.astype(np.float32)
    # a soft light shadow under the letters, as in the original
    shadow = np.array(Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(4))).astype(np.float32) / 255.0
    sy, sx = y0 + 3, left + 3
    region = out[sy:sy + cap, sx:sx + width]
    a = shadow * 0.35
    region[..., :3] = region[..., :3] * (1 - a[..., None]) + 200 * a[..., None]
    region[..., 3] = np.maximum(region[..., 3], a * 255)
    region = out[y0:y0 + cap, left:left + width]
    region[..., :3] = region[..., :3] * (1 - m[..., None]) + texture * m[..., None]
    region[..., 3] = np.maximum(region[..., 3], m * 255)
    logo = Image.fromarray(out.clip(0, 255).astype(np.uint8), "RGBA")
    logo.save(TARGET, optimize=True)
    print("wrote", TARGET, "accent #%02x%02x%02x" % color)
    # banner and icon exactly as for the randomizer, from the new logo
    rgba2 = np.array(logo)
    bx0, by0, bx1, by1, red2 = make_assets.wordmark_box(rgba2)
    make_assets.banner(logo, (bx0, by0, bx1, by1)).save(os.path.join(HERE, "Logo.png"), optimize=True)
    art = make_assets.icon_image(rgba2, red2, (bx0, by0, bx1, by1), color)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [art.resize((s, s), Image.LANCZOS) for s in sizes]
    frames[-1].save(os.path.join(HERE, "App.ico"), format="ICO", sizes=[(s, s) for s in sizes], append_images=frames[:-1])
    print("wrote Logo.png and App.ico")


if __name__ == "__main__":
    main()
