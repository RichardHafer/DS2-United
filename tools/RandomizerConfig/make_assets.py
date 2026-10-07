"""Builds Logo.png (header banner) and App.ico from assets/DS2Randomizer_Logo.png.

Run from the repository root: python tools/RandomizerConfig/make_assets.py
The original asset is only read. Needs Pillow and numpy.
"""
import os
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "assets", "DS2Randomizer_Logo.png")


def wordmark_box(rgba):
    r, g, a = rgba[..., 0].astype(int), rgba[..., 1].astype(int), rgba[..., 3]
    red = (a > 128) & (r > 120) & (r > g * 1.8)
    ys, xs = np.nonzero(red)
    return xs.min(), ys.min(), xs.max(), ys.max(), red


def accent(rgba, red):
    # median of the solid wordmark pixels, printed for Look.Accent
    px = rgba[red & (rgba[..., 3] == 255)][:, :3]
    return tuple(int(v) for v in np.median(px, axis=0))


def banner(im, box):
    x0, y0, x1, y1 = box
    mid = (y0 + y1) // 2
    half = 200
    crop = im.crop((x0 - 20, mid - half, x1 + 20, mid + half))
    # the helix fades out above and below the wordmark
    arr = np.array(crop).astype(np.float32)
    h = arr.shape[0]
    fade = 140
    ramp = np.ones(h, np.float32)
    ramp[:fade] = np.linspace(0, 1, fade) ** 1.6
    ramp[-fade:] = np.linspace(1, 0, fade) ** 1.6
    arr[..., 3] *= ramp[:, None]
    out = Image.fromarray(arr.clip(0, 255).astype(np.uint8), "RGBA")
    width = 1200
    return out.resize((width, round(out.height * width / out.width)), Image.LANCZOS)


def icon_image(rgba, red_mask, box, color):
    # white helix with a red belt where the wordmark crosses it, on a dark tile
    x0, y0, x1, y1 = box
    arr = rgba.copy()
    white = (arr[..., 3] > 40) & (arr[..., 1] > 120) & ~red_mask
    white[y0 - 30:y1 + 30, :] = False
    ys, xs = np.nonzero(white)
    hx0, hx1, hy0, hy1 = xs.min(), xs.max(), ys.min(), ys.max()
    h = arr[hy0:hy1 + 1, hx0:hx1 + 1].astype(np.float32)
    keep = (h[..., 1] > 90) & (h[..., 0] < h[..., 1] * 1.3)
    h[..., 3] = np.where(keep, h[..., 3], 0)
    h[..., :3] = 255
    helix = Image.fromarray(h.clip(0, 255).astype(np.uint8), "RGBA")
    big, radius = 1024, 190
    canvas = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle((0, 0, big - 1, big - 1), radius=radius, fill=(24, 26, 30, 255),
                        outline=color + (255,), width=34)
    target_h = int(big * 0.80)
    scale = target_h / helix.height
    hw = int(helix.width * scale)
    helix = helix.resize((hw, target_h), Image.LANCZOS)
    left, top = (big - hw) // 2, (big - target_h) // 2
    canvas.alpha_composite(helix, (left, top))
    belt0 = top + int((y0 - 14 - hy0) * scale)
    belt1 = top + int((y1 + 14 - hy0) * scale)
    pad = int(hw * 0.28)
    d.rectangle((left - pad, belt0, left + hw + pad, belt1), fill=color + (255,))
    return canvas


def main():
    im = Image.open(SOURCE).convert("RGBA")
    rgba = np.array(im)
    x0, y0, x1, y1, red = wordmark_box(rgba)
    color = accent(rgba, red)
    print("wordmark", (x0, y0, x1, y1), "accent #%02x%02x%02x" % color)
    banner(im, (x0, y0, x1, y1)).save(os.path.join(HERE, "Logo.png"), optimize=True)
    art = icon_image(rgba, red, (x0, y0, x1, y1), color)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    frames = [art.resize((s, s), Image.LANCZOS) for s in sizes]
    frames[-1].save(os.path.join(HERE, "App.ico"), format="ICO", sizes=[(s, s) for s in sizes],
                    append_images=frames[:-1])
    preview = os.environ.get("ICON_PREVIEW")
    if preview:
        sheet = Image.new("RGBA", (sum(sizes) + 10 * len(sizes), 256), (15, 16, 18, 255))
        x = 0
        for f in frames:
            sheet.alpha_composite(f, (x, 0))
            x += f.width + 10
        sheet.save(preview)


if __name__ == "__main__":
    main()
