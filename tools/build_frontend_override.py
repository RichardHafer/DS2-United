"""Build randomizer/ds2coop_frontend.str for the native game mode row in the map picker.

The copy is the stock frontend\\frontend.str with one movie swapped: MP20GameSettings
comes from Mark's DS2 multiplayer (two menu items, the mode row and the map row),
together with his three recoloured libraries it imports from (appended), so it
looks as in his build; --stock-colours renames its 22 imports back to the stock
libraries instead. Every other record is copied byte for byte. DS2United.asi reads this file instead of the stock one
while [Menu] NativeModeRow=1; the archives are never touched.

It contains EA data and Mark's movie, so it is built on each machine from the
local installs and never shipped. tools/build_loadout_frontend.py builds the same
file with the MP suit and weapon screens extended on top of this.

    build_frontend_override.py [--game DIR] [--mark DIR] [--out FILE] [--third-row] [--check]

--game  the Dead Space 2 folder (DS2DAT6.DAT), default from ds2_paths
--mark  Mark's "Dead Space 2 Multiplayer" folder (its DS2DAT6.DAT)
--out   default <game>/randomizer/ds2coop_frontend.str
--third-row  MP20 gets a third row, m2 "difficulty" (tools/mp20_third_row.py, for [Difficulty] Pick=2)
--check build and verify in memory, write nothing
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
from ds2_paths import GAME_DIR  # noqa: E402
from frontend_str import (BLOCK, FRONTEND, HEADER, RAW_PIECE, big_entry, declared, frame, group, names,  # noqa: E402,F401
                          payload, records, rpak, sdbm, validate, with_declared)
import mp20_third_row  # noqa: E402

MOVIE = 'mp20gamesettings.apx'
STOCK_SHA = '7dc56fe9483c09a4645cfa3172a48298c0525b8b851e7adb753c47186eeec114'
# Mark's MP20 movie after the renames. Only this movie is checked: his updates
# change other screens of frontend.str (11.10.: mp_levels, lobby, leaderboards)
# while this movie stayed byte for byte the same.
MOVIE_SHA = '980715e25c222a03efbe8b8d912893137bad628951a2f9ee25f4627f438fd959'
STOCK_MOVIE_SIZE = 616880
# Mark's movie after the renames; DS2Coop checks the same size in the registry.
MOVIE_SIZE = 686320
RENAMES = [(b'LibFrontemp\x00', b'LibFrontend\x00', 7), (b'LibSharmp\x00', b'LibShared\x00', 3),
           (b'SharedLibMP\x00', b'SharedLibPC\x00', 12)]
# Mark's recoloured copies of the three libraries (orange instead of the stock
# colours). With them the MP20 movie keeps its own imports; the renames above
# only check that it is the movie this was made for.
LIBS = {'libfrontemp.apx': '086bdc2c6d6ea1bfe76b31afb6cea77ab98930ebc7494fbc0ed90225f62f288d',
        'libsharmp.apx': '67e4a2e00bd7d9900c86d67560355c673f87c6dad758dd80fe4923758c942d93',
        'sharedlibmp.apx': 'ec670082ae2a80f0d4ab64c524b7b6107aba7064af84f0da04e4418c3d067ad2'}


def build(stock, mark, mark_colours=True, replace=None, third_row=False):
    """The override bytes and a report. replace maps further stock APX names to new payloads."""
    replace = dict(replace or {})
    if MOVIE in replace:
        raise SystemExit('%s is swapped by this builder already' % MOVIE)
    if hashlib.sha256(stock).hexdigest() != STOCK_SHA:
        raise SystemExit('the stock frontend.str is not the Steam retail file')
    stock_assets = group(records(stock))
    mark_by = {names(s)[0]: (s, b) for s, b in group(records(mark))}
    ms, mb = mark_by[MOVIE]
    original = payload(mb, declared(ms))
    movie = original
    for old, new, want in RENAMES:
        got = movie.count(old)
        if got != want or len(old) != len(new):
            raise SystemExit('%r found %d times, expected %d' % (old, got, want))
        movie = movie.replace(old, new)
    if len(movie) != MOVIE_SIZE:
        raise SystemExit('movie is %d bytes, expected %d' % (len(movie), MOVIE_SIZE))
    if hashlib.sha256(movie).hexdigest() != MOVIE_SHA:
        raise SystemExit("Mark's MP20 movie is not the version this was made for")
    libs = []
    if mark_colours:
        movie = original
        for name, want in LIBS.items():
            if name not in mark_by:
                raise SystemExit("Mark's frontend.str holds no %s" % name)
            ls, lb = mark_by[name]
            if hashlib.sha256(payload(lb, declared(ls))).hexdigest() != want:
                raise SystemExit("Mark's %s is not the version this was made for" % name)
            libs.append((ls, lb))
    row = None
    if third_row:
        movie, row = mp20_third_row.add_third_row(movie)

    out_assets, replaced, swapped = [], 0, {}
    for shdr, bodies in stock_assets:
        base, kind = names(shdr)
        if base == MOVIE and kind == 'APX':
            if declared(shdr) != STOCK_MOVIE_SIZE:
                raise SystemExit('stock movie is %d bytes' % declared(shdr))
            out_assets.append((with_declared(shdr, len(movie)), rpak(movie)))
            replaced += 1
        elif base in replace and kind == 'APX':
            if base in swapped:
                raise SystemExit('%s found twice' % base)
            new = replace[base]
            out_assets.append((with_declared(shdr, len(new)), rpak(new)))
            swapped[base] = len(new)
        else:
            out_assets.append((shdr, bodies))
    if replaced != 1:
        raise SystemExit('the movie was found %d times' % replaced)
    missing = sorted(set(replace) - set(swapped))
    if missing:
        raise SystemExit('not in the stock frontend.str: %s' % ', '.join(missing))
    out_assets.extend(libs)   # at the end, as in Mark's file
    data = frame(out_assets)
    count = validate(data)

    back = group(records(data))
    if len(back) != len(out_assets):
        raise SystemExit('asset count changed on read back')
    changed = sum(1 for (s0, b0), (s1, b1) in zip(out_assets, back) if s0 != s1 or b0 != b1)
    if changed:
        raise SystemExit('%d assets differ on read back' % changed)
    for s, b in back:
        base = names(s)[0]
        if base == MOVIE and payload(b, declared(s)) != movie:
            raise SystemExit('the movie differs on read back')
        if base in replace and payload(b, declared(s)) != replace[base]:
            raise SystemExit('%s differs on read back' % base)
    return data, {'assets': len(back), 'records': count, 'movie_size': len(movie),
                  'movie_sha256': hashlib.sha256(movie).hexdigest(), 'mark_colours': bool(libs),
                  'swapped': swapped, 'third_row': bool(row), 'rows_y': row['rows_y'] if row else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=str(GAME_DIR))
    ap.add_argument('--mark', default=None)
    ap.add_argument('--out', default=None)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--stock-colours', action='store_true', help="the MP20 movie on the stock libraries (no orange)")
    ap.add_argument('--third-row', action='store_true', help='MP20 with the third row m2 "difficulty"')
    args = ap.parse_args()
    game = Path(args.game)
    mark_dir = Path(args.mark) if args.mark else game / 'Dead Space 2 Multiplayer'
    out = Path(args.out) if args.out else game / 'randomizer' / 'ds2coop_frontend.str'
    stock = big_entry(game / 'DS2DAT6.DAT', FRONTEND)
    mark = big_entry(mark_dir / 'DS2DAT6.DAT', FRONTEND)
    data, report = build(stock, mark, not args.stock_colours, third_row=args.third_row)
    del report['swapped']
    report.update({'out': str(out), 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'written': not args.check})
    if not args.check:
        out.parent.mkdir(parents=True, exist_ok=True)
        tmp = out.with_suffix('.tmp')
        tmp.write_bytes(data)
        tmp.replace(out)
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
