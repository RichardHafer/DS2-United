"""Recolour the in-game interface the way the frontend was recoloured.

global_assets\\global_assets.str (DS2DAT9.DAT):
- the Apt movies: the pause menu (sp10pausemenu.apx), the settings screens, popups and the button
  libraries (sharedlibpc, libshared, libsingleplayer), with recolor_frontend's movie pass;
- the RIG holograms: inventory, store, bench, text log and notice holograms and the locator
  line. Their chrome textures (rig_*, ui_pc_msgalert*, menu_bracket, ammocounter_bracket, ...)
  get their DXT colour endpoints moved and their effects (inv_*, ui_*, popup_*, ...) their colour
  keys, with recolor_frontend_scene's red shift. The suit's health and stasis gauges
  (player_*_ui) stay.
- the item pickup holograms (pickup_ui_* effects, ui_pickup* textures) the same way: the frame,
  scan lines, header plate with its category badge, the "E Pick Up" bar and the light turn red,
  the texts stay white.
- item pictures stay as they were everywhere (user, 16.10.): their textures (inv_* without
  inv_icon_*, mp_inv_*, storeicon_*, the placeholder card's ui_pickup_beacon) are not touched,
  and every particle that draws one gets its stock colour keys back after the shift, in the
  inventory, store and pickup holograms alike.
loading_screen\\loading_screen_global.str (DS2DAT6/7/8.DAT): the loading screen movies (their pages
are all chrome: rings, the ship emblem, the spinner), textures and effects.
global_mp_assets\\global_mp_assets.str (DS2DAT9.DAT, user 16.10.: the online pause menu stayed blue):
the MP screens (mp10pausemenuhuman with its timer and objective list, scoring, spectator, vent
pick, necro alerts) and the stream's own copies of the button libraries, the same way; the MP
health and stasis gauges (ui_mp_wep_health/stasis/h/s) stay like the campaign's. Its own copies
of the pickup and inventory holograms follow the same rules, so MP maps show the same cards.

Only colour values change: every asset keeps its size and layout, every other record is copied
as it is, so the streams' TOCs (names, dependencies, payload sizes, no file offsets) stay valid.
The plugin reads randomizer\\ds2coop_global_assets.str, randomizer\\ds2coop_loading_screen.str and
randomizer\\ds2coop_global_mp_assets.str through the frontend's file open hook ([Menu] RedInGameMenus).

    recolor_global_assets.py [--game DIR] [--out-dir DIR] [--rgb RRGGBB] [--highlight RRGGBB]
                             [--no-text-white] [--no-holo] [--check]

Defaults match the installed red frontend: --rgb D8443C, white text, --highlight FF8A2A.
"""
import argparse
import json
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ds2_paths import GAME_DIR  # noqa: E402
import frontend_str as fs  # noqa: E402
import recolor_frontend as rf  # noqa: E402
import recolor_frontend_scene as rs  # noqa: E402

BS = chr(92)
GLOBAL_ASSETS = 'global_assets' + BS + 'global_assets.str'
LOADING = 'loading_screen' + BS + 'loading_screen_global.str'
GLOBAL_MP_ASSETS = 'global_mp_assets' + BS + 'global_mp_assets.str'
# icons and controller art keep their pages (their texts and frames still turn red)
rf.CONTENT_PAGES.update({
    'sp13achievements.apx': 'all',
    'fe07controlshuman.apx': 'all',
    'fe30humancontrolmapping.apx': 'all',
    'fe07controlsnecro.apx': 'all',
    'fe31necrocontrolmapping.apx': 'all',
})
# the RIG holograms' chrome in global_assets (item pictures and storeicon_* are content)
HOLO_TEXTURES = re.compile(r'^(rig_|ui_pc_msgalert|ui_pc_arrow|menu_bracket|ammocounter_bracket|uitip_back|store_scroll|ui_bars|ui_log_|'
                           r'log_icon_|inv_icon_|holo01$|rig_holo$|hologram_randtext|ui_savek_|ui_player_landing|ui_fe_|ui_pdlc$)')
HOLO_EFFECTS = re.compile(r'^(ui_|inv_|invslot_|popup_|ds2_tip_|tutorial_|textlog_|log_|cshstore_|store_|nav_hand_|text_|cmplc_inv_|rig_air_guage)')
HOLO_EFFECTS_KEPT = re.compile(r'^(ui_fe_fuzz|human_fuzz)')
MP_GAUGES_KEPT = re.compile(r'^ui_mp_wep_(health|stasis|h|s)$')
# item pickup holograms (user, 16.10.: red like the rest, but the item picture stays as it was)
PICKUP_TEXTURES = re.compile(r'^ui_pickup')
PICKUP_EFFECTS = re.compile(r'^(ui_pickup|pickup_ui_)')
# item pictures in any hologram (inventory, store, pickup cards; mp_inv_*: the MP ammo card,
# ui_pickup_beacon: the placeholder card). Particles drawing them keep their stock colour keys.
ITEM_PICTURES = re.compile(r'^(inv_(?!icon_)|mp_inv_|storeicon_|ui_pickup_beacon$)')
PACK_CODES = {c: k + 1 for k, c in enumerate('abcdefghijklmnopqrstuvwxyz0123456789 _.')}


def holo_texture(n):
    return bool(HOLO_TEXTURES.match(n)) or (bool(PICKUP_TEXTURES.match(n)) and not ITEM_PICTURES.match(n))


def holo_effect(n):
    return bool(HOLO_EFFECTS.match(n) or PICKUP_EFFECTS.match(n)) and not HOLO_EFFECTS_KEPT.match(n)


STREAMS = {
    'global': {'dat': 'DS2DAT9.DAT', 'path': GLOBAL_ASSETS, 'out': 'ds2coop_global_assets.str',
               'textures': holo_texture, 'effects': holo_effect},
    'loading': {'dat': 'DS2DAT6.DAT', 'path': LOADING, 'out': 'ds2coop_loading_screen.str',
                'textures': lambda n: True,
                'effects': lambda n: not HOLO_EFFECTS_KEPT.match(n)},
    'mp': {'dat': 'DS2DAT9.DAT', 'path': GLOBAL_MP_ASSETS, 'out': 'ds2coop_global_mp_assets.str',
           'textures': holo_texture, 'effects': lambda n: holo_effect(n) and not MP_GAUGES_KEPT.match(n)},
}


def packed_name(name):
    """packedstring16 of a name (EARS Pack 0x00745BA0): how effect records name their textures."""
    s = name.lower()
    if len(s) > 23:
        s = '%08x' % fs.hash_string(name) + s[-15:]
    s = ''.join(c if c in PACK_CODES else '.' for c in s)
    out = bytearray(16)
    n = len(s)
    if n <= 16:
        out[:n] = s.encode()
        return bytes(out)
    k = 47 - 2 * n  # this tail stays text, the rest goes base 40, three letters per word
    out[:k] = s[n - k:].encode()
    out[k] = (PACK_CODES[s[0]] - 0x80) & 0xFF
    for j, i in enumerate(range(1, n - k, 3)):
        struct.pack_into('<H', out, k + 1 + 2 * j, PACK_CODES[s[i]] + 40 * PACK_CODES[s[i + 1]] + 1600 * PACK_CODES[s[i + 2]])
    return bytes(out)


def keep_pictures(edit, pictures, textures, notes):
    """Put the item picture particles of a shifted effect back to stock; returns their count."""
    kept = 0
    for i, o, end, tag in rs.record_table(edit.data):
        if tag != b'trp.':
            continue
        params = o + 0x0C + struct.unpack_from('<I', edit.data, o + 0x10)[0]
        tex = edit.data[params:params + 16]
        if tex not in textures and any(tex) and bytes(edit.out[o:end]) != edit.data[o:end]:
            notes.append('%s rec %d: shifted, texture %s is not in the stream' % (edit.name, i, tex.hex()))
        if tex not in pictures:
            continue
        kept += 1
        if bytes(edit.out[o:end]) != edit.data[o:end]:
            if len(edit.rows) != len(edit.swatches):
                raise SystemExit('%s: rows and swatches out of step' % edit.name)
            edit.out[o:end] = edit.data[o:end]
            edit.ranges = [r for r in edit.ranges if not o <= r[0] < end]
            pairs = [(r, s) for r, s in zip(edit.rows, edit.swatches) if not o <= r[1] < end]
            edit.rows, edit.swatches = [r for r, _ in pairs], [s for _, s in pairs]
    return kept


def recolor(data, shift, tints, red, wants):
    assets = fs.group(fs.records(data))
    index = {}
    for k, (shdr, _) in enumerate(assets):
        index.setdefault(fs.names(shdr), []).append(k)

    def load(k):
        shdr, bodies = assets[k]
        return fs.payload(bodies, fs.declared(shdr))

    new_payloads, stats, summary, problems, skipped, notes = {}, {}, [], [], [], []
    textures = {packed_name(b) for b, kind in index if kind == 'tg4d'}
    pictures = {packed_name(b) for b, kind in index if kind == 'tg4d' and ITEM_PICTURES.match(b)}
    pictures_kept = 0
    for k, (shdr, bodies) in enumerate(assets):
        base, kind = fs.names(shdr)
        if kind == 'APX':
            movie = rf.Movie(base, load(k))
            edit = rf.Edit(movie)
            rf.shift_movie(edit, shift, summary)
            rf.tint_movie(edit, tints)
            if edit.ranges:
                changed = rf.verify_movie(edit)
                new_payloads[k] = bytes(edit.out)
                stats[base] = {'kind': 'APX', 'fields': len(edit.ranges), 'bytes': changed}
        elif kind == 'tg4d' and red and wants['textures'](base):
            heads = index.get((base, 'tg4h'), [])
            if len(heads) != 1 or len(index.get((base, 'tg4d'), [])) != 1:
                notes.append('%s: %d headers, left' % (base, len(heads)))
                continue
            try:
                tex = rs.Texture(base, load(heads[0]), load(k))
            except SystemExit as e:
                notes.append('%s left: %s' % (base, e))
                continue
            edit = rs.Edit(base, 'tg4d', tex.data)
            blocks = rs.shift_texture(edit, tex, red)
            if blocks:
                stats[base] = {'kind': 'tg4d', 'blocks': blocks, 'bytes': rs.verify_edit(edit)}
                new_payloads[k] = bytes(edit.out)
        elif kind == 'VFX' and red and wants['effects'](base):
            edit = rs.Edit(base, 'VFX', load(k))
            try:
                rs.shift_vfx(edit, red, problems, skipped)
            except ValueError as e:
                notes.append('%s left: %s' % (base, e))
                continue
            pictures_kept += keep_pictures(edit, pictures, textures, notes)
            if edit.ranges:
                stats[base] = {'kind': 'VFX', 'fields': len(edit.rows), 'bytes': rs.verify_edit(edit)}
                new_payloads[k] = bytes(edit.out)

    out_assets = [(shdr, fs.rpak(new_payloads[k]) if k in new_payloads else bodies) for k, (shdr, bodies) in enumerate(assets)]
    out = fs.frame(out_assets)
    records = fs.validate(out)
    back = fs.group(fs.records(out))
    if len(back) != len(assets):
        raise SystemExit('asset count changed on read back')
    for k, ((s0, b0), (s1, b1)) in enumerate(zip(assets, back)):
        if s0 != s1:
            raise SystemExit('asset header %d changed' % k)
        if k in new_payloads:
            if fs.payload(b1, fs.declared(s1)) != new_payloads[k]:
                raise SystemExit('%s differs on read back' % fs.names(s1)[0])
        elif b0 != b1:
            raise SystemExit('asset %d (%s) changed' % (k, fs.names(s0)[0]))
        if fs.declared(s0) != fs.declared(s1):
            raise SystemExit('declared size of %s changed' % fs.names(s0)[0])
    kinds = {}
    for v in stats.values():
        kinds[v['kind']] = kinds.get(v['kind'], 0) + 1
    return out, {'records': records, 'assets': len(back), 'changed': kinds, 'pictures_kept': pictures_kept,
                 'problems': problems[:20], 'problem_count': len(problems),
                 'skipped': skipped[:10], 'skipped_count': len(skipped), 'notes': notes[:20], 'note_count': len(notes)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=str(GAME_DIR))
    ap.add_argument('--out-dir', default=str(ROOT / 'build' / 'global_red'))
    ap.add_argument('--rgb', default='D8443C')
    ap.add_argument('--band', type=float, default=30.0)
    ap.add_argument('--highlight', default='FF8A2A')
    ap.add_argument('--spread', type=float, default=0.08)
    ap.add_argument('--saturation', type=float, default=1.2)
    ap.add_argument('--no-text-white', action='store_true')
    ap.add_argument('--no-holo', action='store_true')
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    args.hue = None
    args.text_white = not args.no_text_white
    shift, label = rf.shift_from_args(args)
    tints, tint_label = rf.tints_from_args(args, shift)
    rgb = tuple(int(args.rgb.lstrip('#')[i:i + 2], 16) for i in (0, 2, 4))
    red = None if args.no_holo else rs.RedShift(rgb, args.spread, args.saturation)

    reports = {}
    for key, s in STREAMS.items():
        stock = fs.big_entry(Path(args.game) / s['dat'], s['path'])
        new, report = recolor(stock, shift, tints, red, s)
        out = Path(args.out_dir) / s['out']
        if not args.check:
            out.parent.mkdir(parents=True, exist_ok=True)
            tmp = out.with_suffix('.tmp')
            tmp.write_bytes(new)
            tmp.replace(out)
        report.update({'stock_size': len(stock), 'stock_crc32': '%08X' % zlib.crc32(stock), 'out': str(out), 'size': len(new),
                       'crc32': '%08X' % zlib.crc32(new), 'written': not args.check})
        reports[key] = report
    print(json.dumps({'shift': label, 'tints': tint_label, 'holo': None if red is None else red.label(), 'streams': reports}, indent=1))


if __name__ == '__main__':
    main()
