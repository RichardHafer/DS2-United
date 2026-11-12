"""Build the MP loadout screens with the campaign suits and guns added.

Writes, from the local stock install (and Mark's DS2DAT6.DAT for the MP20 movie, as
tools/build_frontend_override.py does):

  <out>/ds2coop_frontend.str      frontend.str with the MP20 swap and Mark's three recoloured
                                  libraries (build_frontend_override), plus
                                  - fe12setupsuit.apx: frames frame6..frame28 in the suit picture
                                    sprite (char 124) and empty frames in the tint mask sprite
                                    (char 132), one 256x512 DXT5 page per suit;
                                  - fe11setupweapons.apx: frames frame6..frame9 in the weapon icon
                                    sprite (char 37), before its "locked" frame, on one new page.
  <out>/ds2coop_mpskins.lua       the stock mpskins.lua and 23 rows for the campaign suits
  <out>/ds2coop_loadout_rows.tsv  code name -> campaign Uniform GUID, PlayerSkinDB row, style index
  src/generated_loadout_rows.h    the same rows and the four guns for DS2Coop

Suit pictures come from tools/loadout_photos/<code name>.png (any size, transparent or dark
background; fitted into the 196x512 portrait area). A suit without a photo gets a placeholder
card: its helmet icon from the campaign inventory and its name.

    build_loadout_frontend.py [--game DIR] [--mark DIR] [--out DIR] [--photos DIR]
                              [--review DIR] [--stock-colours] [--check] [--header-only]

--out          default <game>/randomizer
--review DIR   also write each picture/icon frame as the movie maps it, as PNG, for review
--check        build and verify everything in memory, write nothing (except --review)
--header-only  only write src/generated_loadout_rows.h (needs no game files)

The outputs hold EA data; the release zip carries the finished frontend (tools/make_release_package.py).
"""
import argparse
import hashlib
import json
import re
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ds2_paths import GAME_DIR  # noqa: E402
import apx_edit as ax  # noqa: E402
import build_frontend_override as fo  # noqa: E402
import frontend_str as fs  # noqa: E402

PHOTOS = ROOT / 'tools' / 'loadout_photos'
HEADER = ROOT / 'src' / 'generated_loadout_rows.h'
BS = '\\'
MPSKINS = 'mpskins.lua'
MPSKINS_SHA = 'a185eaa661933d4a2a21b6837e142af6df408b7e305a8159e9c2b5fe335ffb14'
TEXT_STR = BS.join(['text_assets', 'text', 'f40cc683.str'])
TEXT_LH2 = ('deadspace2_global.en.lh2', 'LH2')
GLOBAL_ASSETS = BS.join(['global_assets', 'global_assets.str'])
STOCK_SKIN_ROWS = 36
FIRST_STYLE = 6

# The campaign suits in bot::kSuits order (analysis/DS2_MP_LOADOUT_CONTENT_2026-10-12.md 2.1).
# name id: HashString_SDBM(name id) is the hash of the campaign store name.
SUITS = [
    # code name, suit, campaign Uniform GUID, name id, store name hash, helmet icon
    ('sp_arctic_security', 'Arctic Security Suit', (0x38CB666F, 0xE989A6F7, 0x54524542, 0x314D4152), 'cs00_aekwc7l', 0x9C2B9A79, 'inv_sec_desert_suit'),
    ('sp_riot_security', 'Riot Security Suit', (0x38CB666F, 0xF99BFFA1, 0x54524542, 0x314D4152), 'cs01_al9wm9h', 0xCB400E85, 'inv_secsuit'),
    ('sp_soldier', 'Soldier Suit', (0x38CB666F, 0xF2B9B9FB, 0x54524542, 0x314D4152), 'cs02_axbsouq', 0xD0667D76, 'inv_sec_army_suit'),
    ('sp_patrol', 'Patrol Suit', (0x31CB7561, 0x1B516549, 0x534F5254, 0x30314E49), 'cs03_besmn67', 0x3F2C3DA6, 'inv_secsuit'),
    ('sp_zealot', 'Zealot Suit', (0x58CB5F60, 0x4BF6F5A0, 0x574F4843, 0x39323031), 'cs04_dd2stbt', 0x83B4A4BA, 'inv_secsuit'),
    ('sp_shockpoint', 'Shockpoint Suit', (0x38CB63F1, 0xFA62DA20, 0x54524542, 0x314D4152), 'cs05_afhrfki', 0x1F1FBC4B, 'inv_advsuit'),
    ('sp_bloody_vintage', 'Bloody Vintage Suit', (0x38CB63F2, 0x5D6D2A6A, 0x54524542, 0x314D4152), 'cs06_ayyhfwc', 0x48FEDC82, 'inv_antsuit'),
    ('sp_forged_engineering', 'Forged Engineering Suit', (0x38CB63F2, 0x508D4590, 0x54524542, 0x314D4152), 'cs07_gftiu3q', 0x5DD6C777, 'inv_engsuit'),
    ('sp_earthgov_security', 'EarthGov Security Suit', (0x38CB63F2, 0x42052D6A, 0x54524542, 0x314D4152), 'cs08_a7auirt', 0x37E6253A, 'inv_secsuit'),
    ('sp_heavy_duty_vintage', 'Heavy Duty Vintage Suit', (0x38CB63F2, 0x1C0C50E0, 0x54524542, 0x314D4152), 'cs09_jtxi896', 0xAEA62BF4, 'inv_antsuit'),
    ('sp_hazard', 'Hazard Suit', (0x38CB63F1, 0xDECD957E, 0x54524542, 0x314D4152), 'cs10_a545tys', 0x9746FE49, 'inv_engsuit'),
    ('sp_agility_advanced', 'Agility Advanced Suit', (0x38CB63F2, 0x2BE46F24, 0x54524542, 0x314D4152), 'cs11_a59erem', 0x7635972B, 'inv_advsuit'),
    ('sp_triage', 'Triage Suit', (0x38CB63F1, 0xED1EDDAA, 0x54524542, 0x314D4152), 'cs12_aqqsc1x', 0xDB335D4A, 'inv_secsuit'),
    ('sp_advanced', 'Advanced Suit', (0x39CAC0A5, 0x686ED487, 0x4E415548, 0x38343147), 'cs13_bgk0lky', 0xE691FB23, 'inv_advsuit'),
    ('sp_patient', 'Patient Suit', (0x30C9FF4E, 0x44D00498, 0x54544950, 0x32303153), 'cs14_a_6zh15', 0x92F4201E, None),
    ('sp_engineering', 'Engineering Suit', (0x30C9B946, 0xE243D95F, 0x54544950, 0x32303153), 'cs15_bqfgvqv', 0x1ACCDE20, 'inv_engsuit'),
    ('sp_security', 'Security Suit', (0x39CA8FDF, 0x97CF9234, 0x574F5242, 0x3130314E), 'cs16_iq93imq', 0x5EB93D21, 'inv_secsuit'),
    ('sp_vintage', 'Vintage Suit', (0x39CB3355, 0x740F0370, 0x4E415548, 0x38343147), 'cs17_bk701ws', 0xA2A59C22, 'inv_antsuit'),
    ('sp_elite_engineering', 'Elite Engineering Suit', (0x38CB37E4, 0xC671D1BE, 0x54524542, 0x314D4152), 'cs18_c0oylfn', 0x6CD2BC55, 'inv_eng_eli_suit'),
    ('sp_elite_security', 'Elite Security Suit', (0x38CB37E6, 0x424A40DE, 0x54524542, 0x314D4152), 'cs19_ashcf2t', 0x82FF1D94, 'inv_sec_eli_suit'),
    ('sp_elite_vintage', 'Elite Vintage Suit', (0x38CB37E7, 0x2058CA16, 0x54524542, 0x314D4152), 'cs20_bn5sxoe', 0x992B7ED3, 'inv_ant_eli_suit'),
    ('sp_elite_advanced', 'Elite Advanced Suit', (0x38CB37E7, 0xAD1B5F9C, 0x54524542, 0x314D4152), 'cs21_adtzlvu', 0xAF57E012, 'inv_adv_eli_suit'),
    ('sp_hacker', 'Hacker Suit', (0x58CB43ED, 0xEDE44FA8, 0x4E4F574B, 0x35373230), 'cs22_cdvaado', 0x25F92771, 'inv_hackersuit'),
]
# Profile level each campaign suit unlocks at in the MP suit screen ([Loadout] Unlocks=0).
SUIT_LEVELS = {
    'sp_engineering': 0, 'sp_security': 0, 'sp_patient': 1, 'sp_vintage': 2, 'sp_advanced': 6, 'sp_hacker': 10,
    'sp_arctic_security': 12, 'sp_riot_security': 15, 'sp_soldier': 18, 'sp_patrol': 21, 'sp_zealot': 24,
    'sp_shockpoint': 27, 'sp_bloody_vintage': 29, 'sp_forged_engineering': 31, 'sp_earthgov_security': 34,
    'sp_heavy_duty_vintage': 38, 'sp_hazard': 40, 'sp_agility_advanced': 43, 'sp_triage': 47,
    'sp_elite_engineering': 50, 'sp_elite_security': 53, 'sp_elite_vintage': 55, 'sp_elite_advanced': 58,
}
# HashString_SDBM("sp_desc_c18ocdz") is the hash of the stock string "Isaac's Suit".
DESC_ID, DESC_HASH = 'sp_desc_c18ocdz', 0xFC3801E8

# FE11 rows after the seven stock ones; the icon frame is the row - 1.
GUNS = [
    # code, gun, FE11 row, name id, campaign GunProxy GUID, icon texture, stream holding it
    ('ripper', 'Ripper', 7, '$online_weapon_ripper', (0x36C8BC4F, 0x6F9078E2, 0x44455257, 0x34303245), 'inv_yoyo', GLOBAL_ASSETS),
    ('flamethrower', 'Flamethrower', 8, '$online_weapon_flamethrower', (0x36C8BC40, 0x0C67B69A, 0x44455257, 0x34303245), 'inv_flame', GLOBAL_ASSETS),
    ('contactbeam', 'Contact Beam', 9, '$online_weapon_contactbeam', (0x36C8BC3F, 0xF55615C8, 0x44455257, 0x34303245), 'inv_cbeam', GLOBAL_ASSETS),
    ('detonator', 'Detonator', 10, '$online_weapon_detonator', (0x58CA30C7, 0x78E54CEB, 0x3045454C, 0x58373634), 'inv_detonator', GLOBAL_ASSETS),
    # the Foam Finger; no text id names it, so the row shows plain text (as DS2Coop's descriptions do)
    ('handcannon', 'Hand Cannon', 11, 'hand cannon', (0x38CB6750, 0x72BC9767, 0x54524542, 0x314D4152), 'inv_fingergun', GLOBAL_ASSETS),
]

# fe12setupsuit.apx: mCurrentSuitIcon > mBaseSuit (125) > mSuit (124), frames frame0..5, one
# textured shape each (char = geometry 113 for frame0); mBaseSuitTint (132) holds the masks.
FE12 = {'apx': 'fe12setupsuit.apx', 'sprite': 124, 'tint': 132, 'template': 113, 'frames': 6,
        'page': (256, 512), 'area': (30, 0, 226, 512)}
# fe11setupweapons.apx: mCurrentWeaponIcon (37), frames frame0..5 and "locked".
FE11 = {'apx': 'fe11setupweapons.apx', 'sprite': 37, 'template': 27, 'frames': 6,
        'page': (256, 512), 'cell': 128, 'icon': 88}
# The Ripper's inventory icon keeps its prototype name (inv_wpn_ripper.vfx uses inv_yoyo).
ICON_CROP = {}


def guid_text(g):
    return '-'.join('%08X' % w for w in g)


# ---------------------------------------------------------------- pictures

def font(size):
    for name in ('arialbd.ttf', 'Arial Bold.ttf', 'DejaVuSans-Bold.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default(size)


def key_black(img, threshold=6, pocket=400):
    """A photo without transparency gets its black background keyed out.

    Background is every dark area (no channel above threshold) that touches the
    border, plus every larger pure black pocket (between an arm and the body). The
    dark fringe the renderer left on the silhouette fades out. Returns the RGBA image
    and the share of pixels keyed out (0.0 when the photo already had transparency)."""
    import numpy as np
    from scipy import ndimage
    rgba = img.convert('RGBA')
    a = np.array(rgba)
    if a[..., 3].min() < 255:
        return rgba, 0.0
    peak = a[..., :3].max(axis=2)
    labels, n = ndimage.label(peak <= threshold)
    if not n:
        return rgba, 0.0
    ids = np.arange(1, n + 1)
    sizes = ndimage.sum(np.ones_like(peak), labels, ids)
    zeros = ndimage.sum(peak == 0, labels, ids)
    edge = np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))
    keep = np.zeros(n + 1, dtype=bool)
    keep[edge] = True
    keep[1:] |= (sizes >= pocket) & (zeros >= sizes * 0.9)
    keep[0] = False
    background = keep[labels]
    alpha = np.where(background, 0, 255).astype(np.int32)
    near = ndimage.binary_dilation(background, structure=np.ones((3, 3), dtype=bool)) & ~background
    alpha[near] = np.minimum(255, peak[near].astype(np.int32) * 255 // 40)
    a[..., 3] = alpha.astype(np.uint8)
    return Image.fromarray(a, 'RGBA'), float(background.mean())


def fit(img, w, h):
    """img scaled to fit w x h (aspect kept, transparent border trimmed), centred on a clear canvas."""
    img = img.convert('RGBA')
    box = img.getchannel('A').getbbox()
    if box:
        img = img.crop(box)
    scale = min(w / img.width, h / img.height)
    img = img.resize((max(1, round(img.width * scale)), max(1, round(img.height * scale))), Image.LANCZOS)
    out = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    out.alpha_composite(img, ((w - img.width) // 2, (h - img.height) // 2))
    return out


def placeholder(title, icon, w, h):
    card = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(card)
    d.rounded_rectangle((3, 3, w - 4, h - 4), radius=10, fill=(14, 18, 24, 235), outline=(62, 146, 186, 255), width=3)
    if icon is not None:
        card.alpha_composite(fit(icon, 160, 160), ((w - 160) // 2, 70))
    f = font(24)
    lines, line = [], ''
    for word in title.upper().split():
        test = (line + ' ' + word).strip()
        if line and d.textlength(test, font=f) > w - 24:
            lines.append(line)
            line = word
        else:
            line = test
    lines.append(line)
    y = 280
    for text in lines:
        d.text(((w - d.textlength(text, font=f)) / 2, y), text, font=f, fill=(210, 230, 240, 255))
        y += 32
    return card


def suit_pages(photos, icons):
    pw, ph = FE12['page']
    x0, y0, x1, y1 = FE12['area']
    pages, sources = [], []
    for code, title, _, _, _, icon in SUITS:
        path = photos / (code + '.png')
        if path.is_file():
            photo, keyed = key_black(Image.open(path))
            art = fit(photo, x1 - x0, y1 - y0)
            sources.append('photo %s%s' % (path.name, ' (black background keyed out, %.0f%%)' % (keyed * 100) if keyed else ''))
        else:
            art = placeholder(title, icons.get(icon) if icon else None, x1 - x0, y1 - y0)
            sources.append('placeholder (%s)' % (icon or 'no helmet icon'))
        page = Image.new('RGBA', (pw, ph), (0, 0, 0, 0))
        page.alpha_composite(art, (x0, y0))
        pages.append(page)
    return pages, sources


def gun_page(icons):
    pw, ph = FE11['page']
    cell, size = FE11['cell'], FE11['icon']
    page = Image.new('RGBA', (pw, ph), (0, 0, 0, 0))
    areas = []
    for k, gun in enumerate(GUNS):
        cx, cy = (k % 2) * cell, (k // 2) * cell
        at = (cx + (cell - size) // 2, cy + (cell - size) // 2)
        icon = icons[gun[5]]
        if gun[5] in ICON_CROP:
            icon = icon.crop(ICON_CROP[gun[5]])
        page.alpha_composite(fit(icon, size, size), at)
        areas.append((at[0], at[1], at[0] + size, at[1] + size))
    return page, areas


def load_icons(game):
    wanted = {}
    for s in SUITS:
        if s[5]:
            wanted.setdefault(GLOBAL_ASSETS, set()).add(s[5])
    for g in GUNS:
        wanted.setdefault(g[6], set()).add(g[5])
    icons = {}
    for stream, texs in wanted.items():
        data = fs.big_entry(Path(game) / 'DS2DAT9.DAT', stream)
        keys = {(t, k) for t in texs for k in ('tg4h', 'tg4d')}
        got = fs.find_assets(data, keys)
        for t in texs:
            if (t, 'tg4h') not in got or (t, 'tg4d') not in got:
                raise SystemExit('%s holds no texture %s' % (stream, t))
            icons[t] = ax.texture_rgba(got[(t, 'tg4h')], got[(t, 'tg4d')])
    return icons


# ---------------------------------------------------------------- movies

def open_movie(apx):
    chunks = ax.parse_apx(apx)
    if ax.build_apx(chunks) != apx:
        raise SystemExit('the stock movie does not round trip through the container writer')
    apt = ax.Apt(chunks[ax.chunk(chunks, b'apti')][1], chunks[ax.chunk(chunks, b'cons')][1])
    geos = ax.parse_rubi(chunks[ax.chunk(chunks, b'rubi')][1])
    tx = chunks[ax.chunk(chunks, b'tx4h')][1]
    setname, pages = ax.parse_tx4h(tx)
    if ax.build_tx4h(setname, pages) != tx:
        raise SystemExit('the stock page table does not round trip')
    baseline = set(apt.check({g for g, _ in geos}))
    return chunks, apt, geos, setname, pages, baseline


def labels_of(apt, sprite):
    return [name for _, name, _, _ in apt.frame_labels(sprite)]


def place_item(apt, frame):
    hits = [io for io in apt.items_of(frame) if apt.u32(io) == 3]
    if len(hits) != 1:
        raise SystemExit('expected one PlaceObject in the template frame')
    return hits[0]


def close_movie(chunks, apt, geos, setname, pages, new_pages):
    out = []
    for tag, body in chunks:
        if tag == b'apti':
            body = bytes(apt.d)
        elif tag == b'rubi':
            body = ax.build_rubi(geos)
        elif tag == b'tx4h':
            body = ax.build_tx4h(setname, pages)
        out.append((tag, body))
    out.extend((b'tx4t', data) for data in new_pages)
    return ax.build_apx(out)


def extend_fe12(apx, page_images):
    chunks, apt, geos, setname, pages, baseline = open_movie(apx)
    want = ['frame%d' % k for k in range(FE12['frames'])]
    suit, tint = apt.sprite(FE12['sprite']), apt.sprite(FE12['tint'])
    if labels_of(apt, suit) != want or labels_of(apt, tint) != want:
        raise SystemExit('fe12setupsuit.apx is not the stock movie (picture frames)')
    sframes, tframes = apt.frames_of(suit), apt.frames_of(tint)
    template_place = place_item(apt, sframes[-1])
    shape = apt.shape(FE12['template'])
    gtemplate = dict(geos)[FE12['template']]
    table = apt.char_table()
    pw, ph = FE12['page']
    new_pages = []
    for k, img in enumerate(page_images):
        cid = len(table)
        page_no = len(pages) + 1
        geos.append((cid, ax.quad_on_page(gtemplate, page_no, FE12['area'], pw, ph)))
        table.append(apt.add_shape(shape, cid))
        index = FE12['frames'] + k
        label = 'frame%d' % index
        sframes.append(apt.new_frame([apt.label_item(label, index), apt.place_copy(template_place, character=cid)]))
        tframes.append(apt.new_frame([apt.label_item(label, index)]))
        pages.append({'name': str(page_no), 'size': pw * ph, 'w': pw, 'h': ph, 'mips': 1, 'format': 3})
        new_pages.append(ax.dxt5_encode(img))
    apt.set_frames(suit, sframes)
    apt.set_frames(tint, tframes)
    apt.set_char_table(table)
    return close_movie(chunks, apt, geos, setname, pages, new_pages), baseline


def extend_fe11(apx, page_image, areas):
    chunks, apt, geos, setname, pages, baseline = open_movie(apx)
    icon = apt.sprite(FE11['sprite'])
    stock = ['frame%d' % k for k in range(FE11['frames'])] + ['locked']
    if labels_of(apt, icon) != stock:
        raise SystemExit('fe11setupweapons.apx is not the stock movie (icon frames)')
    frames = apt.frames_of(icon)
    head, locked = frames[:FE11['frames']], frames[FE11['frames']]
    template_place = place_item(apt, head[-1])
    shape = apt.shape(FE11['template'])
    gtemplate = dict(geos)[FE11['template']]
    table = apt.char_table()
    pw, ph = FE11['page']
    page_no = len(pages) + 1
    added = []
    for k, (gun, area) in enumerate(zip(GUNS, areas)):
        cid = len(table)
        geos.append((cid, ax.quad_on_page(gtemplate, page_no, area, pw, ph)))
        table.append(apt.add_shape(shape, cid))
        index = gun[2] - 1
        added.append(apt.new_frame([apt.label_item('frame%d' % index, index), apt.place_copy(template_place, character=cid)]))
    # "locked" moves behind the new frames; its label keeps its name, only the index changes
    for io in apt.items_of(locked):
        if apt.u32(io) == 2:
            apt.put(io + 12, len(head) + len(added))
    apt.set_frames(icon, head + added + [locked])
    apt.set_char_table(table)
    pages.append({'name': str(page_no), 'size': pw * ph, 'w': pw, 'h': ph, 'mips': 1, 'format': 3})
    return close_movie(chunks, apt, geos, setname, pages, [ax.dxt5_encode(page_image)]), baseline


def verify_movie(apx, baseline, sprites, review, prefix, names):
    """Parse the written movie back, check every reference, render each picture frame."""
    chunks = ax.parse_apx(apx)
    if ax.build_apx(chunks) != apx:
        raise SystemExit('%s does not round trip' % prefix)
    apt = ax.Apt(chunks[ax.chunk(chunks, b'apti')][1], chunks[ax.chunk(chunks, b'cons')][1])
    geos = dict(ax.parse_rubi(chunks[ax.chunk(chunks, b'rubi')][1]))
    setname, pages = ax.parse_tx4h(chunks[ax.chunk(chunks, b'tx4h')][1])
    datas = [b for t, b in chunks if t == b'tx4t']
    if len(datas) != len(pages) or any(len(d) != p['size'] or p['size'] != p['w'] * p['h'] for d, p in zip(datas, pages)):
        raise SystemExit('%s: pages and page data do not match' % prefix)
    if any(p['id'] != zlib.crc32((setname + p['name']).encode()) for p in pages):
        raise SystemExit('%s: page ids' % prefix)
    new = sorted(set(apt.check(set(geos))) - baseline)
    if new:
        raise SystemExit('%s: %s' % (prefix, '; '.join(new[:8])))
    report = {}
    table = apt.char_table()
    decoded = {}
    for sprite_id, want in sprites.items():
        o = apt.sprite(sprite_id)
        got = [(k, name, idx) for k, name, _, idx in apt.frame_labels(o)]
        if [name for _, name, _ in got] != want or any(k != idx for k, _, idx in got):
            raise SystemExit('%s: sprite %d labels %s' % (prefix, sprite_id, [n for _, n, _ in got]))
        report['sprite %d' % sprite_id] = '%d frames, labels %s..%s' % (len(want), want[0], want[-1])
    if review is not None:
        review.mkdir(parents=True, exist_ok=True)
        o = apt.sprite(next(iter(sprites)))
        tiles = []
        for k, fr in enumerate(apt.frames_of(o)):
            places = [io for io in apt.items_of(fr) if apt.u32(io) == 3]
            if not places:
                continue
            ch = apt.i32(places[0] + 12)
            so = table[ch]
            if apt.type_of(so) != 1:
                continue
            page, u0, v0, u1, v1, x0, y0, x1, y1 = ax.quad_info(geos[apt.u32(so + 24)])
            p = pages[page - 1]
            if page not in decoded:
                decoded[page] = ax.dxt_decode(datas[page - 1], p['w'], p['h'])
            crop = decoded[page].crop((round(u0 * p['w']), round(v0 * p['h']), round(u1 * p['w']), round(v1 * p['h'])))
            crop = crop.resize((round((x1 - x0) * 2), round((y1 - y0) * 2)), Image.LANCZOS)
            label = [apt.s(apt.u32(io + 4)) for io in apt.items_of(fr) if apt.u32(io) == 2][0]
            tag = names.get(label, 'stock')
            crop.save(review / ('%s_%s_%s.png' % (prefix, label, tag)))
            tiles.append(crop)
        if tiles:
            tw, th = max(t.width for t in tiles), max(t.height for t in tiles)
            cols = 8
            sheet = Image.new('RGBA', (cols * (tw + 6), ((len(tiles) + cols - 1) // cols) * (th + 6)), (40, 40, 40, 255))
            for i, t in enumerate(tiles):
                sheet.alpha_composite(t, ((i % cols) * (tw + 6), (i // cols) * (th + 6)))
            sheet.save(review / ('%s_sheet.png' % prefix))
    return report


# ---------------------------------------------------------------- text and rows

def lh2_table(game):
    data = fs.big_entry(Path(game) / 'DS2DAT0.DAT', TEXT_STR)
    lh2 = fs.find_assets(data, {TEXT_LH2}).get(TEXT_LH2)
    if not lh2 or lh2[:4] != b'LCH2':
        raise SystemExit('%s holds no English text table' % TEXT_STR)
    n = struct.unpack_from('<I', lh2, 0x10)[0]
    hashes = struct.unpack_from('<%dI' % n, lh2, 0x20)
    offs = struct.unpack_from('<%dI' % n, lh2, 0x20 + 4 * n)
    return {h: lh2[o:lh2.index(b'\0', o)].decode('utf-8', 'replace') for h, o in zip(hashes, offs)}


def check_text(texts):
    out = {}
    for code, title, _, name_id, want, _ in SUITS:
        h = fs.hash_string(name_id)
        if h != want or h not in texts:
            raise SystemExit('%s: name id %s does not resolve' % (code, name_id))
        out[code] = texts[h].strip()
    if fs.hash_string(DESC_ID) != DESC_HASH or DESC_HASH not in texts:
        raise SystemExit('description id %s does not resolve' % DESC_ID)
    out['description'] = texts[DESC_HASH]
    for code, _, _, name_id, _, _, _ in GUNS:
        if not name_id.startswith('$'):
            out[code] = name_id
            continue
        h = fs.hash_string(name_id.lstrip('$'))
        if h not in texts:
            raise SystemExit('%s: %s is not in the text table' % (code, name_id))
        out[code] = texts[h]
    return out


def mpskins(stock):
    if hashlib.sha256(stock).hexdigest() != MPSKINS_SHA:
        raise SystemExit('the stock mpskins.lua is not the Steam retail file')
    nl = '\r\n'
    lines = ['', '-- DS2Coop: the 23 campaign suits for the MP suit screen (tools/build_loadout_frontend.py).',
             '-- The name ids hash to the campaign store names; DS2Coop maps the code name to the',
             '-- campaign Uniform. Style index 6.. = frame6.. of the suit picture in fe12setupsuit.apx.']
    for k, (code, title, _, name_id, _, _) in enumerate(SUITS):
        lines += ['', '-- %s' % title,
                  'PlayerSkinDB_RegisterSkin("%s",' % code, '"%s",' % name_id, '"%s",' % DESC_ID, '%d,' % SUIT_LEVELS[code], '"tint",',
                  '1.0, 1.0, 1.0,', '%d)' % (FIRST_STYLE + k)]
    return stock + (nl.join(lines) + nl).encode('ascii')


TOKEN = re.compile(r'\s+|--[^\n]*|"(?:[^"\\\n]|\\.)*"|-?\d+(?:\.\d+)?|[A-Za-z_][A-Za-z0-9_]*|[(),]')


def check_lua(data, stock):
    """Syntax of the subset mpskins.lua uses (calls with string and number arguments) and the rows."""
    if not data.startswith(stock):
        raise SystemExit('the stock rows changed')
    text = data.decode('ascii')
    pos, toks = 0, []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise SystemExit('lua: unexpected text at %d: %r' % (pos, text[pos:pos + 20]))
        t = m.group()
        if not (t.isspace() or t.startswith('--')):
            toks.append(t)
        pos = m.end()
    rows, i = [], 0
    while i < len(toks):
        if not re.fullmatch(r'[A-Za-z_]\w*', toks[i]) or i + 1 >= len(toks) or toks[i + 1] != '(':
            raise SystemExit('lua: expected a call at token %d (%r)' % (i, toks[i]))
        name, i, args = toks[i], i + 2, []
        while True:
            t = toks[i]
            if t.startswith('"'):
                args.append(t[1:-1])
            elif re.fullmatch(r'-?\d+(?:\.\d+)?', t):
                args.append(float(t) if '.' in t else int(t))
            else:
                raise SystemExit('lua: bad argument %r in call %d' % (t, len(rows)))
            i += 1
            if toks[i] == ')':
                i += 1
                break
            if toks[i] != ',':
                raise SystemExit('lua: expected , or ) in call %d' % len(rows))
            i += 1
        if name != 'PlayerSkinDB_RegisterSkin':
            raise SystemExit('lua: unexpected call %s' % name)
        rows.append(args)
    if len(rows) != STOCK_SKIN_ROWS + len(SUITS):
        raise SystemExit('lua: %d rows' % len(rows))
    codes = [r[0] for r in rows]
    if len(set(codes)) != len(codes):
        raise SystemExit('lua: duplicate code names')
    for k, (row, suit) in enumerate(zip(rows[STOCK_SKIN_ROWS:], SUITS)):
        want = [suit[0], suit[3], DESC_ID, SUIT_LEVELS[suit[0]], 'tint', 1.0, 1.0, 1.0, FIRST_STYLE + k]
        if row != want:
            raise SystemExit('lua: row %s reads back as %s' % (suit[0], row))
    return len(rows)


def rows_tsv():
    out = ['code_name\tskin_index\tstyle_index\tuniform_guid\tcode_hash\tname_id\tsuit']
    for k, (code, title, guid, name_id, _, _) in enumerate(SUITS):
        out.append('%s\t%d\t%d\t%s\t%08X\t%s\t%s' % (code, STOCK_SKIN_ROWS + k, FIRST_STYLE + k, guid_text(guid),
                                                    fs.hash_string(code), name_id, title))
    return ('\r\n'.join(out) + '\r\n').encode('ascii')


def header_text():
    out = ['// Generated by tools/build_loadout_frontend.py; do not edit.',
           '// The campaign suits and guns added to the MP suit (FE12) and weapon (FE11) screens.',
           '// Suits: code name as registered in randomizer\\ds2coop_mpskins.lua, its HashString_SDBM',
           '// (SkinEntry code-name hash), the campaign Uniform GUID, the FE12 style index (frameN of the',
           '// suit picture), the PlayerSkinDB row (36 stock rows come first) and the default unlock level',
           '// (DS2United.ini [Unlocks] overrides it by code name).',
           '// Guns: FE11 table row (the stored weapon index is row - 1), name id, campaign GunProxy GUID',
           '// and the FE11 icon frame (frameN).',
           '#pragma once',
           'namespace loadout_data {',
           'struct SuitRow { const char* code; uint32_t nameHash; uint32_t uniform[4]; uint8_t uiIndex; uint8_t skinIndex; uint8_t level; };',
           'struct GunRow { int row; const char* nameId; uint32_t proxy[4]; int iconFrame; };',
           'inline constexpr int kStockSkinRows = %d;' % STOCK_SKIN_ROWS,
           'inline constexpr int kFirstSuitStyle = %d;' % FIRST_STYLE,
           'inline constexpr int kFirstGunRow = %d;' % GUNS[0][2],
           'inline constexpr SuitRow kSuits[] = {']
    for k, (code, title, guid, _, _, _) in enumerate(SUITS):
        out.append('    {"%s", 0x%08XU, {0x%08XU, 0x%08XU, 0x%08XU, 0x%08XU}, %d, %d, %d},  // %s' % (
            code, fs.hash_string(code), guid[0], guid[1], guid[2], guid[3], FIRST_STYLE + k, STOCK_SKIN_ROWS + k, SUIT_LEVELS[code], title))
    out.append('};')
    out.append('inline constexpr GunRow kGuns[] = {')
    for code, title, row, name_id, guid, _, _ in GUNS:
        out.append('    {%d, "%s", {0x%08XU, 0x%08XU, 0x%08XU, 0x%08XU}, %d},  // %s' % (
            row, name_id, guid[0], guid[1], guid[2], guid[3], row - 1, title))
    out.append('};')
    out.append('}  // namespace loadout_data')
    return ('\r\n'.join(out) + '\r\n').encode('ascii')


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_bytes(data)
    tmp.replace(path)


def file_report(path, data, written):
    return {'path': str(path), 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'written': written}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=str(GAME_DIR))
    ap.add_argument('--mark', default=None)
    ap.add_argument('--out', default=None, help='output folder, default <game>/randomizer')
    ap.add_argument('--photos', default=str(PHOTOS))
    ap.add_argument('--review', default=None)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--stock-colours', action='store_true')
    ap.add_argument('--header-only', action='store_true')
    args = ap.parse_args()

    header = header_text()
    header_written = False
    if not args.check and (not HEADER.exists() or HEADER.read_bytes() != header):
        write(HEADER, header)
        header_written = True
    if args.header_only:
        print(json.dumps({'header': file_report(HEADER, header, header_written)}, indent=1))
        return

    codes = [s[0] for s in SUITS]
    if len(set(codes)) != len(codes) or any(not re.fullmatch(r'sp_[a-z0-9_]+', c) for c in codes):
        raise SystemExit('code names must be unique lower case sp_ names')
    game = Path(args.game)
    mark_dir = Path(args.mark) if args.mark else game / 'Dead Space 2 Multiplayer'
    out_dir = Path(args.out) if args.out else game / 'randomizer'
    photos = Path(args.photos)
    if not photos.is_dir() and not args.check:
        photos.mkdir(parents=True, exist_ok=True)
    review = Path(args.review) if args.review else None

    stock = fs.big_entry(game / 'DS2DAT6.DAT', fs.FRONTEND)
    mark = fs.big_entry(mark_dir / 'DS2DAT6.DAT', fs.FRONTEND)
    movies = fs.find_assets(stock, {(FE12['apx'], 'APX'), (FE11['apx'], 'APX')})
    icons = load_icons(game)
    texts = check_text(lh2_table(game))

    pages, sources = suit_pages(photos, icons)
    fe12, base12 = extend_fe12(movies[(FE12['apx'], 'APX')], pages)
    gpage, areas = gun_page(icons)
    fe11, base11 = extend_fe11(movies[(FE11['apx'], 'APX')], gpage, areas)
    names12 = {'frame%d' % (FIRST_STYLE + k): s[0] for k, s in enumerate(SUITS)}
    names11 = {'frame%d' % (g[2] - 1): g[0] for g in GUNS}
    v12 = verify_movie(fe12, base12, {FE12['sprite']: ['frame%d' % k for k in range(FIRST_STYLE + len(SUITS))],
                                       FE12['tint']: ['frame%d' % k for k in range(FIRST_STYLE + len(SUITS))]},
                       review, 'fe12', names12)
    v11 = verify_movie(fe11, base11, {FE11['sprite']: ['frame%d' % k for k in range(GUNS[-1][2])] + ['locked']},
                       review, 'fe11', names11)

    frontend, report = fo.build(stock, mark, not args.stock_colours, {FE12['apx']: fe12, FE11['apx']: fe11})
    stock_lua = fs.big_entry(game / 'DS2DAT6.DAT', MPSKINS)
    lua = mpskins(stock_lua)
    lua_rows = check_lua(lua, stock_lua)
    tsv = rows_tsv()

    outputs = [(out_dir / 'ds2coop_frontend.str', frontend), (out_dir / 'ds2coop_mpskins.lua', lua),
               (out_dir / 'ds2coop_loadout_rows.tsv', tsv)]
    if not args.check:
        for path, data in outputs:
            write(path, data)
    for (code, title, _, _, _, _), k, src in zip(SUITS, range(len(SUITS)), sources):
        print('%-22s style %2d  %-24s %s' % (code, FIRST_STYLE + k, texts[code], src))
    for code, title, row, name_id, _, icon, _ in GUNS:
        print('%-22s row %2d   frame%d  %-14s icon %s' % (code, row, row - 1, texts[code], icon))
    summary = {
        'frontend': dict(file_report(outputs[0][0], frontend, not args.check), assets=report['assets'],
                         records=report['records'], mark_colours=report['mark_colours'], swapped=report['swapped']),
        'mpskins': dict(file_report(outputs[1][0], lua, not args.check), rows=lua_rows),
        'rows': file_report(outputs[2][0], tsv, not args.check),
        'header': file_report(HEADER, header, header_written),
        'fe12': dict(size=len(fe12), sha256=hashlib.sha256(fe12).hexdigest(), **v12),
        'fe11': dict(size=len(fe11), sha256=hashlib.sha256(fe11).hexdigest(), **v11),
        'description': texts['description'], 'photos': str(photos), 'review': str(review) if review else None,
    }
    print(json.dumps(summary, indent=1))


if __name__ == '__main__':
    main()
