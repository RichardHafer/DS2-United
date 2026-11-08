"""Give MP20 (the map picker's "game setup" screen) a third row, m2 "difficulty", under m0 (game mode) and m1 (game map).

Mark's movie (tools/build_frontend_override.py) holds the menu sprite level1.menu with the row clips m0 (depth 107) and
m1 (depth 128). The native screen fills one {text, value} entry per row; UIExternScrollMenuIterator::UpdateItem sets
"mai" to the row and calls the movie's UpdateMenuItem, which draws mMenuArray[mai]. InitMenu builds that array from the
clip names, so the third row needs:

  - a copy of m1's clip with its label "$ui_gamemap_mc" swapped for "$UIMenu_Difficulty" (stock text "difficulty"),
    placed as m2 at depth 129 (between m1 and the 3D transform pop at depth 130), one row pitch below m1. The copy
    starts on an empty stopped frame, so it stays invisible while the native screen drives only two rows;
  - InitMenu rewritten in place (same 113 bytes) to build [m0, m1, m2]; two constants are added to the cons table
    (the string "m2" and register 1), so every constant stays referenced exactly once, as in every stock movie, and
    the table is then renumbered to the order the actions name it (the loader asserts that order; 21.10. crash);
  - the description box (130), the description text (132), the map picture (95), the bottom bracket (3) and its
    flash (192) moved down by the pitch; --lift moves the whole panel (level1) up to recentre it.

A marker string (MARKER) goes at the end of the movie data; DS2Coop looks for it before it gives MP20 three items.

    mp20_third_row.py [--in FILE] [--out FILE] [--pitch PX] [--lift PX] [--check]

--in    built frontend stream, default the installed randomizer\\ds2coop_frontend.str (read only)
--out   default build/frontend/ds2coop_frontend.str
--pitch row pitch, default the distance from m0 to m1 in the movie (67.6)
--lift  move the panel up by this much, default 0 (Mark's top stays, the panel grows downwards)
--check build and verify in memory, write nothing

Every record other than the MP20 movie is copied as it is. The movie can also be built in by
build_frontend_override.py --third-row.
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from ds2_paths import GAME_DIR  # noqa: E402
import apx_edit as ax  # noqa: E402
import frontend_str as fs  # noqa: E402

MP20 = 'mp20gamesettings.apx'
LABEL = '$UIMenu_Difficulty'
MARKER = 'DS2Coop:MP20:m2'
ROW_DEPTH = 129
MOVED_DEPTHS = (3, 95, 130, 132, 192)
LEVEL1_DEPTH = 7
FLAG_MATRIX, FLAG_CLIPACTIONS = 0x04, 0x80
STOP_STREAM = b'\x07\x00'

# constant table entry types (cons): 1 string, 4 register, 5 bool, 6 float, 7 int, 8 constant pool lookup
STRING, REGISTER, BOOL, FLOAT, INT, LOOKUP = 1, 4, 5, 6, 7, 8
# opcodes with operands and their layout ('A' aligns to 4 in the movie data)
OPERANDS = {0x81: 'A4', 0x83: 'A44', 0x87: 'A4', 0x88: 'A44', 0x8A: 'A44', 0x8B: 'A4', 0x8C: 'A4', 0x8D: 'A4', 0x94: 'A4',
            0x96: 'A44', 0x99: 'A4', 0x9A: 'A4', 0x9D: 'A4', 0x9E: '', 0x9F: 'A4', 0xA1: 'A4', 0xA2: '1', 0xA3: '2',
            0xA4: 'A4', 0xA5: 'A4', 0xA6: 'A4', 0xA7: 'A4', 0xAE: '1', 0xAF: '1', 0xB0: '1', 0xB1: '1', 0xB2: '1', 0xB3: '1',
            0xB4: 'A4', 0xB5: '1', 0xB6: '2', 0xB7: 'A4', 0xB8: 'A4', 0xB9: '1'}
NAMES = {0x12: 'Not', 0x17: 'Pop', 0x1C: 'GetVariable', 0x1D: 'SetVariable', 0x3D: 'CallFunction', 0x42: 'InitArray', 0x4E: 'GetMember',
         0x87: 'StoreRegister', 0x96: 'PushData'}


class Cons:
    """The constant file: header, entry table {type, value}, strings (offsets into the file)."""

    def __init__(self, data):
        c = data.find(b'Apt constant file')
        if c != 0:
            raise SystemExit('cons: no constant file header at 0')
        self.data = bytes(data)
        self.movie, n, self.table = struct.unpack_from('<III', data, 20)
        if self.table != 32:
            raise SystemExit('cons: entry table at %#x, expected 0x20' % self.table)
        self.items = [list(struct.unpack_from('<II', data, self.table + 8 * i)) for i in range(n)]
        self.strings_at = self.table + 8 * n
        self.added = []
        bad = [i for i, (t, v) in enumerate(self.items) if t == STRING and not (self.strings_at <= v < len(data))]
        if bad:
            raise SystemExit('cons: string entries outside the string area: %s' % bad[:8])

    def text(self, v):
        e = self.data.index(b'\0', v)
        return self.data[v:e].decode('latin1')

    def add(self, kind, value):
        """A new entry at the end of the table; a string entry takes its text as the value."""
        self.items.append([kind, value])
        if kind == STRING:
            self.added.append(len(self.items) - 1)
        return len(self.items) - 1

    def build(self):
        """The file with the added entries: the old strings move by the table growth, new strings go at the end."""
        grow = 8 * len(self.items) - (self.strings_at - self.table)
        strings = bytearray(self.data[self.strings_at:])
        while len(strings) % 4:
            strings += b'\0'
        base = self.table + 8 * len(self.items)
        out = bytearray(self.data[:20])
        out += struct.pack('<III', self.movie, len(self.items), self.table)
        for i, (t, v) in enumerate(self.items):
            if i in self.added:
                text, v = v, base + len(strings)
                strings += text.encode('ascii') + b'\0'
            elif t == STRING:
                v += grow
            out += struct.pack('<II', t, v)
        out += strings
        return bytes(out), grow


def walk_stream(d, start, end=None, on_op=None):
    """Walk one action stream (nested functions included); on_op(at, op, operand offset, nested end)."""
    p = start
    while True:
        if end is not None and p >= end:
            return p
        op = d[p]
        at = p
        p += 1
        if op < 0x80:
            if on_op:
                on_op(at, op, None)
            if op == 0 and end is None:
                return p
            continue
        if op in (0x9B, 0x8E):
            p = (p + 3) & ~3
            if op == 0x9B:
                size = struct.unpack_from('<I', d, p + 12)[0]
                body = p + 24
            else:
                size = struct.unpack_from('<I', d, p + 16)[0]
                body = p + 28
            if on_op:
                on_op(at, op, p)
            walk_stream(d, body, body + size, on_op)
            p = body + size
            continue
        if op not in OPERANDS:
            raise SystemExit('action stream at %#x: unknown opcode %#04x at %#x' % (start, op, at))
        lay = OPERANDS[op]
        if lay.startswith('A'):
            p = (p + 3) & ~3
            lay = lay[1:]
        if on_op:
            on_op(at, op, p)
        p += sum(int(c) for c in lay)


def streams(apt):
    """(where, start) of every action stream: frame actions, init actions, clip events."""
    out = []
    owners = [('movie', apt.movie)] + [('char %d' % i, o) for i, o in enumerate(apt.char_table()) if o and apt.type_of(o) == 5]
    for name, o in owners:
        for k, fr in enumerate(apt.frames_of(o)):
            for io in apt.items_of(fr):
                t = apt.u32(io)
                if t == 1:
                    out.append(('%s frame %d' % (name, k), apt.u32(io + 4)))
                elif t == 8:
                    out.append(('%s frame %d init' % (name, k), apt.u32(io + 8)))
                elif t == 3 and apt.u32(io + 4) & FLAG_CLIPACTIONS and apt.u32(io + 60):
                    ev = apt.u32(io + 60)
                    n, q = apt.u32(ev), apt.u32(ev + 4)
                    for j in range(n):
                        out.append(('%s frame %d clip event %d' % (name, k, j), apt.u32(q + 12 * j + 8)))
    return out


def constant_refs(apt):
    """Every push list and constant pool of the movie: [(list offset, [entry indices])]."""
    lists = []
    d = apt.d

    def op(at, code, operand):
        if code in (0x96, 0x88):
            n, q = struct.unpack_from('<II', d, operand)
            lists.append((q, [struct.unpack_from('<I', d, q + 4 * i)[0] for i in range(n)]))
    for _, start in streams(apt):
        walk_stream(d, start, None, op)
    return lists


def reference_slots(apt):
    """Every cons index slot of every push list and constant pool, in the order the loader meets them."""
    slots = []
    d = apt.d

    def op(at, code, operand):
        if code in (0x96, 0x88):
            n, q = struct.unpack_from('<II', d, operand)
            slots.extend(q + 4 * i for i in range(n))
    for _, start in streams(apt):
        walk_stream(d, start, None, op)
    return slots


def renumber(apt, cons):
    """The loader relocates the cons entries in the order the actions name them and asserts
    nConstTableIndex == *pnCurrentConstantIndex (AptActionInterpreter.cpp:314, PC 0x005D320D): the
    entries must be named 0, 1, 2, ... in walk order (21.10.: the two entries added at the end of
    the table crashed the game on opening the map picker). Entries are moved to that order."""
    slots = reference_slots(apt)
    old = [struct.unpack_from('<I', apt.d, s)[0] for s in slots]
    if sorted(old) != list(range(len(cons.items))):
        raise SystemExit('renumber: the references are no permutation of the %d constants' % len(cons.items))
    items = [None] * len(old)
    for new, (slot, idx) in enumerate(zip(slots, old)):
        struct.pack_into('<I', apt.d, slot, new)
        items[new] = cons.items[idx]
    cons.added = sorted(old.index(a) for a in cons.added)
    cons.items = items
    return sum(1 for new, idx in enumerate(old) if new != idx)


def pool_of(apt):
    """The constant pool the movie's frame 0 action sets up (the scripts' lookups go through it)."""
    first = apt.items_of(apt.frames_of(apt.movie)[0])
    acts = [apt.u32(io + 4) for io in first if apt.u32(io) == 1]
    if len(acts) != 1 or apt.d[acts[0]] != 0x88:
        raise SystemExit('the movie frame 0 action does not start with a constant pool')
    p = (acts[0] + 1 + 3) & ~3
    n, q = struct.unpack_from('<II', apt.d, p)
    return acts[0], [struct.unpack_from('<I', apt.d, q + 4 * i)[0] for i in range(n)]


def meaning(cons, pool, idx):
    t, v = cons.items[idx]
    if t == STRING:
        return ('str', cons.text(v))
    if t == LOOKUP:
        return ('name', meaning(cons, pool, pool[v])[1]) if v < len(pool) else ('pool?', v)
    if t == REGISTER:
        return ('reg', v)
    if t == INT:
        return ('int', v)
    if t == FLOAT:
        return ('float', struct.unpack('<f', struct.pack('<I', v))[0])
    if t == BOOL:
        return ('bool', bool(v))
    return ('type%d' % t, v)


def function_named(apt, start, name):
    """(definition offset, body offset, body size) of a DefineFunction in one stream, by name."""
    hits = []

    def op(at, code, operand):
        if code != 0x9B or not apt.u32(operand) or apt.s(apt.u32(operand)) != name:
            return
        size = struct.unpack_from('<I', apt.d, operand + 12)[0]
        hits.append((at, operand + 24, size))
    walk_stream(apt.d, start, None, op)
    if len(hits) != 1:
        raise SystemExit('%s defined %d times' % (name, len(hits)))
    return hits[0]


def decode_body(apt, cons, pool, body, size):
    """[(offset, name, operand)] of a function body; push operands are lists of (entry index, meaning)."""
    out = []

    def op(at, code, operand):
        if code == 0x96:
            n, q = struct.unpack_from('<II', apt.d, operand)
            entries = [struct.unpack_from('<I', apt.d, q + 4 * i)[0] for i in range(n)]
            out.append((at, 'PushData', (q, [(e, meaning(cons, pool, e)) for e in entries])))
        elif code == 0x87:
            out.append((at, 'StoreRegister', apt.u32(operand)))
        else:
            out.append((at, NAMES.get(code, '%#04x' % code), None))
    walk_stream(apt.d, body, body + size, op)
    return out


MARK_INIT = [('PushData', [('name', 'isInitialized'), ('bool', True)]), ('SetVariable',), ('PushData', [('name', 'level1')]), ('GetVariable',),
             ('PushData', [('name', 'menu')]), ('GetMember',), ('StoreRegister', 1), ('Pop',),
             ('PushData', [('name', 'mMenuArray'), ('reg', 1), ('str', 'm1')]), ('GetMember',), ('PushData', [('reg', 1), ('name', 'm0')]), ('GetMember',),
             ('PushData', [('int', 2)]), ('InitArray',), ('SetVariable',), ('PushData', [('float', 0.0), ('name', 'AnimationComplete')]), ('CallFunction',)]
NEW_INIT = [('PushData', [('name', 'isInitialized'), ('bool', True)]), ('SetVariable',), ('PushData', [('name', 'level1')]), ('GetVariable',),
            ('PushData', [('name', 'menu')]), ('GetMember',), ('StoreRegister', 1), ('Pop',),
            ('PushData', [('name', 'mMenuArray'), ('reg', 1), ('str', 'm2')]), ('GetMember',), ('PushData', [('reg', 1), ('str', 'm1')]), ('GetMember',),
            ('PushData', [('reg', 1), ('name', 'm0')]), ('GetMember',), ('PushData', [('int', 3)]), ('InitArray',), ('SetVariable',),
            ('PushData', [('float', 0.0), ('name', 'AnimationComplete')]), ('CallFunction',)]


def shape_of(ops):
    out = []
    for _, name, operand in ops:
        if name == 'PushData':
            out.append((name, [m for _, m in operand[1]]))
        elif name == 'StoreRegister':
            out.append((name, operand))
        else:
            out.append((name,))
    return out


def tail_ok(ops, prefix):
    """The rest after the prefix: Not padding (Mark's) and a final Pop."""
    rest = shape_of(ops)[prefix:]
    return bool(rest) and rest[-1] == ('Pop',) and all(r == ('Not',) for r in rest[:-1])


def assemble_init(body, size, lists):
    """InitMenu's new body at `body` (absolute, alignment counts from the movie data), padded to `size` with Not."""
    out = bytearray()

    def here():
        return body + len(out)

    def align():
        while here() % 4:
            out.append(0)

    def push(ptr, n):
        out.append(0x96)
        align()
        out.extend(struct.pack('<II', n, ptr))

    def store(reg):
        out.append(0x87)
        align()
        out.extend(struct.pack('<I', reg))
    push(*lists['init'])
    out.append(0x1D)
    push(*lists['level1'])
    out.append(0x1C)
    push(*lists['menu'])
    out.append(0x4E)
    store(1)
    out.append(0x17)
    push(*lists['m2'])
    out.append(0x4E)
    push(*lists['m1'])
    out.append(0x4E)
    push(*lists['m0'])
    out.append(0x4E)
    push(*lists['count'])
    out.append(0x42)
    out.append(0x1D)
    push(*lists['done'])
    out.append(0x3D)
    if len(out) + 1 > size:
        raise SystemExit('the new InitMenu needs %d bytes, the old one has %d' % (len(out) + 1, size))
    out.extend(b'\x12' * (size - len(out) - 1))
    out.append(0x17)
    return bytes(out)


def placements(apt, sprite):
    """[(frame, item offset)] of every PlaceObject in a sprite (or the movie)."""
    return [(k, io) for k, fr in enumerate(apt.frames_of(sprite)) for io in apt.items_of(fr) if apt.u32(io) == 3]


def final_y(apt, sprite, depth):
    y = None
    for _, io in placements(apt, sprite):
        if apt.i32(io + 8) == depth and apt.u32(io + 4) & FLAG_MATRIX:
            y = apt.f32(io + 36)
    return y


def copy_sprite(apt, src_id, swap, lead_stop=False):
    """A new sprite with every frame, item list, item, label and action copied from src_id; swap maps character ids.

    lead_stop puts an empty frame with a stop action first: the clip shows nothing until a gotoAndPlay
    to one of its labels (UpdateMenuItem does that for each row the native screen updates)."""
    o = apt.sprite(src_id)
    frames = []
    if lead_stop:
        frames.append(apt.new_frame([struct.pack('<II', 1, apt.alloc(STOP_STREAM))]))
    for fr in apt.frames_of(o):
        items = []
        for io in apt.items_of(fr):
            t = apt.u32(io)
            if t == 3:
                if apt.u32(io + 4) & FLAG_CLIPACTIONS:
                    raise SystemExit('char %d: a placement with clip actions; not copied' % src_id)
                blob = bytearray(apt.d[io:io + 64])
                ch = struct.unpack_from('<i', blob, 12)[0]
                if ch in swap:
                    struct.pack_into('<i', blob, 12, swap[ch])
                nm = struct.unpack_from('<I', blob, 52)[0]
                if nm:
                    struct.pack_into('<I', blob, 52, apt.string(apt.s(nm)))
                items.append(bytes(blob))
            elif t == 2:
                items.append(apt.label_item(apt.s(apt.u32(io + 4)), apt.u32(io + 12) + (1 if lead_stop else 0)))
            elif t == 1:
                code = apt.u32(io + 4)
                if bytes(apt.d[code:code + 2]) != STOP_STREAM:
                    raise SystemExit('char %d: a frame action other than stop; not copied' % src_id)
                items.append(struct.pack('<II', 1, apt.alloc(STOP_STREAM)))
            elif t == 4:
                items.append(bytes(apt.d[io:io + 8]))
            else:
                raise SystemExit('char %d: frame item type %d; not copied' % (src_id, t))
        frames.append(apt.new_frame(items))
    head = bytearray(apt.d[o:o + 20])
    struct.pack_into('<II', head, 8, 0, 0)
    at = apt.alloc(bytes(head))
    apt.set_frames(at, frames)
    return at


def add_third_row(apx, pitch=None, lift=0.0):
    """The movie with row m2; returns (bytes, report)."""
    chunks = ax.parse_apx(apx)
    # Mark's own file has cons unaligned behind apti; the writer aligns every chunk, as build_map_pictures does
    realigned = ax.build_apx(chunks) != apx
    if realigned and ax.parse_apx(ax.build_apx(chunks)) != chunks:
        raise SystemExit('the movie does not round trip through the container writer')
    ia, ic = ax.chunk(chunks, b'apti'), ax.chunk(chunks, b'cons')
    apt = ax.Apt(chunks[ia][1], chunks[ic][1])
    cons = Cons(chunks[ic][1])
    geos = {g for g, _ in ax.parse_rubi(chunks[ax.chunk(chunks, b'rubi')][1])}
    baseline = set(apt.check(geos))
    if MARKER.encode() in apt.d:
        raise SystemExit('the movie has the third row already')

    table = apt.char_table()
    menus = []
    for cid, o in enumerate(table):
        if o and apt.type_of(o) == 5:
            names = {apt.s(apt.u32(io + 52)): io for _, io in placements(apt, o) if apt.u32(io + 52)}
            if 'm0' in names and 'm1' in names:
                menus.append((cid, o, names))
    if len(menus) != 1:
        raise SystemExit('expected one sprite placing m0 and m1, found %d' % len(menus))
    menu_id, menu, named = menus[0]
    if 'm2' in named:
        raise SystemExit('the menu sprite places m2 already')
    m0, m1 = named['m0'], named['m1']
    if (apt.i32(m0 + 8), apt.i32(m1 + 8)) != (107, 128):
        raise SystemExit('m0/m1 at depths %d/%d, expected 107/128' % (apt.i32(m0 + 8), apt.i32(m1 + 8)))
    used = {apt.i32(io + 8) for _, io in placements(apt, menu)}
    if ROW_DEPTH in used:
        raise SystemExit('depth %d is used in the menu sprite' % ROW_DEPTH)
    y0, y1 = final_y(apt, menu, 107), final_y(apt, menu, 128)
    step = float(pitch) if pitch is not None else y1 - y0
    if not 40.0 <= step <= 120.0:
        raise SystemExit('row pitch %.1f is out of range' % step)

    # the label EditText inside m1's clip
    row_clip = apt.i32(m1 + 12)
    labels = [(io, apt.i32(io + 12)) for _, io in placements(apt, apt.sprite(row_clip))
              if apt.u32(io + 4) & 2 and apt.type_of(table[apt.i32(io + 12)]) == 2]
    if len(labels) != 1:
        raise SystemExit('m1 holds %d label texts, expected one' % len(labels))
    label_id = labels[0][1]
    label_text = apt.s(apt.u32(table[label_id] + 52))
    if label_text != '$ui_gamemap_mc':
        raise SystemExit("m1's label reads %r" % label_text)

    # characters: the label and the row clip
    et = bytearray(apt.d[table[label_id]:table[label_id] + 64])
    struct.pack_into('<I', et, 52, apt.string(LABEL))
    var = struct.unpack_from('<I', et, 56)[0]
    if var:
        struct.pack_into('<I', et, 56, apt.string(apt.s(var)))
    new_label, new_row = len(table), len(table) + 1
    table.append(apt.alloc(bytes(et)))
    table.append(copy_sprite(apt, row_clip, {label_id: new_label}, lead_stop=True))
    apt.set_char_table(table)

    # m2 in frame 0 of the menu sprite, right after m1
    frames = apt.frames_of(menu)
    f0 = apt.items_of(frames[0])
    if m1 not in f0:
        raise SystemExit('m1 is not placed in frame 0')
    place = bytearray(apt.place_copy(m1, character=new_row, name='m2'))
    struct.pack_into('<i', place, 8, ROW_DEPTH)
    struct.pack_into('<f', place, 36, y1 + step)
    new_place = apt.alloc(bytes(place))
    order = []
    for io in f0:
        order.append(io)
        if io == m1:
            order.append(new_place)
    lst = apt.alloc(b''.join(struct.pack('<I', q) for q in order))
    frames[0] = (len(order), lst)
    apt.set_frames(menu, frames)

    # the lower half moves down by the pitch
    moved = {}
    for k, io in placements(apt, menu):
        dep = apt.i32(io + 8)
        if dep in MOVED_DEPTHS and apt.u32(io + 4) & FLAG_MATRIX:
            old = apt.f32(io + 36)
            struct.pack_into('<f', apt.d, io + 36, old + step)
            moved.setdefault(dep, []).append('f%d %.1f->%.1f' % (k, old, old + step))
    lifted = 0
    if lift:
        for _, io in placements(apt, apt.movie):
            if apt.i32(io + 8) == LEVEL1_DEPTH and apt.u32(io + 4) & FLAG_MATRIX:
                struct.pack_into('<f', apt.d, io + 36, apt.f32(io + 36) - lift)
                lifted += 1

    # InitMenu builds [m0, m1, m2]
    pool_at, pool = pool_of(apt)
    _, body, size = function_named(apt, pool_at, 'InitMenu')
    ops = decode_body(apt, cons, pool, body, size)
    if shape_of(ops)[:len(MARK_INIT)] != MARK_INIT or not tail_ok(ops, len(MARK_INIT)):
        raise SystemExit("InitMenu is not Mark's two row version: %s" % shape_of(ops))
    pushes = [operand for _, name, operand in ops if name == 'PushData']
    (l_init, _), (l_level1, _), (l_menu, _), (l_m1, e_m1), (l_m0, e_m0), (l_count, e_count), (l_done, _) = pushes
    old_reg_a, old_name_array, old_m1_str = e_m1[1][0], e_m1[0][0], e_m1[2][0]
    old_reg_b, old_name_m0 = e_m0[0][0], e_m0[1][0]
    count_entry = e_count[0][0]
    cons.items[count_entry] = [INT, 3]
    m2_entry = cons.add(STRING, 'm2')
    reg_entry = cons.add(REGISTER, 1)
    # lists: m2 reuses m1's three slots, m1 reuses m0's two, m0 gets a new list, the count reuses its own
    struct.pack_into('<III', apt.d, l_m1, old_name_array, old_reg_a, m2_entry)
    struct.pack_into('<II', apt.d, l_m0, reg_entry, old_m1_str)
    l_new_m0 = apt.alloc(struct.pack('<II', old_reg_b, old_name_m0))
    lists = {'init': (l_init, 2), 'level1': (l_level1, 1), 'menu': (l_menu, 1), 'm2': (l_m1, 3), 'm1': (l_m0, 2), 'm0': (l_new_m0, 2),
             'count': (l_count, 1), 'done': (l_done, 2)}
    code = assemble_init(body, size, lists)
    apt.d[body:body + size] = code

    apt.string(MARKER)
    while len(apt.d) % 4:
        apt.d.append(0)
    renumbered = renumber(apt, cons)
    new_cons, grow = cons.build()
    out_chunks = list(chunks)
    out_chunks[ia] = (b'apti', bytes(apt.d))
    out_chunks[ic] = (b'cons', bytes(new_cons))
    out = ax.build_apx(out_chunks)
    report = verify(out, baseline, step)
    report.update({'menu_sprite': menu_id, 'row_clip_copied': row_clip, 'label_copied': label_id, 'new_label_char': new_label,
                   'new_row_char': new_row, 'pitch': round(step, 2), 'rows_y': [round(y0, 1), round(y1, 1), round(y1 + step, 1)],
                   'moved': {str(k): ', '.join(v) for k, v in sorted(moved.items())}, 'lift': lift, 'level1_places_lifted': lifted,
                   'init_menu_body': '%#x+%d' % (body, size), 'cons_entries_added': [m2_entry, reg_entry], 'cons_renumbered': renumbered,
                   'cons_strings_moved_by': grow,
                   'size_before': len(apx), 'size_after': len(out), 'realigned': realigned})
    return out, report


def verify(apx, baseline, step):
    """Parse the new movie back and check what the third row depends on."""
    chunks = ax.parse_apx(apx)
    if ax.build_apx(chunks) != apx:
        raise SystemExit('the new movie does not round trip')
    apt = ax.Apt(chunks[ax.chunk(chunks, b'apti')][1], chunks[ax.chunk(chunks, b'cons')][1])
    cons = Cons(chunks[ax.chunk(chunks, b'cons')][1])
    geos = {g for g, _ in ax.parse_rubi(chunks[ax.chunk(chunks, b'rubi')][1])}
    new = sorted(set(apt.check(geos)) - baseline)
    if new:
        raise SystemExit('new movie problems: %s' % '; '.join(new[:8]))
    refs = {}
    pointers = {}
    for q, entries in constant_refs(apt):
        pointers[q] = pointers.get(q, 0) + 1
        for e in entries:
            if e >= len(cons.items):
                raise SystemExit('a push names constant %d of %d' % (e, len(cons.items)))
            refs[e] = refs.get(e, 0) + 1
    twice = [e for e, n in refs.items() if n != 1]
    unused = [e for e in range(len(cons.items)) if e not in refs]
    shared = [q for q, n in pointers.items() if n != 1]
    if twice or unused or shared:
        raise SystemExit('constants referenced twice %s, unused %s, shared lists %s' % (twice[:8], unused[:8], shared[:8]))
    order = [struct.unpack_from('<I', apt.d, s)[0] for s in reference_slots(apt)]
    out_of_order = [i for i, e in enumerate(order) if e != i]
    if out_of_order:
        raise SystemExit('the loader would assert: reference #%d names constant %d' % (out_of_order[0], order[out_of_order[0]]))
    pool_at, pool = pool_of(apt)
    _, body, size = function_named(apt, pool_at, 'InitMenu')
    ops = decode_body(apt, cons, pool, body, size)
    if shape_of(ops)[:len(NEW_INIT)] != NEW_INIT or not tail_ok(ops, len(NEW_INIT)):
        raise SystemExit('InitMenu reads back as %s' % shape_of(ops))
    menus = [o for o in apt.char_table() if o and apt.type_of(o) == 5 and
             any(apt.u32(io + 52) and apt.s(apt.u32(io + 52)) == 'm2' for _, io in placements(apt, o))]
    if len(menus) != 1:
        raise SystemExit('m2 is placed by %d sprites' % len(menus))
    rows = {}
    for _, io in placements(apt, menus[0]):
        nm = apt.u32(io + 52)
        if nm and apt.s(nm) in ('m0', 'm1', 'm2'):
            rows[apt.s(nm)] = (apt.i32(io + 8), apt.i32(io + 12))
    clip = apt.sprite(rows['m2'][1])
    texts = [apt.s(apt.u32(apt.char_table()[apt.i32(io + 12)] + 52)) for _, io in placements(apt, clip)
             if apt.u32(io + 4) & 2 and apt.type_of(apt.char_table()[apt.i32(io + 12)]) == 2]
    if texts != [LABEL]:
        raise SystemExit('m2 shows %s' % texts)
    first = apt.items_of(apt.frames_of(clip)[0])
    if [apt.u32(io) for io in first] != [1] or bytes(apt.d[apt.u32(first[0] + 4):apt.u32(first[0] + 4) + 2]) != STOP_STREAM:
        raise SystemExit("m2's first frame is not an empty stop")
    labels = [(name, k) for k, name, _, _ in apt.frame_labels(clip)]
    if labels != [('ia', 1), ('a', 6)]:
        raise SystemExit("m2's labels are %s" % labels)
    if apt.d.find(MARKER.encode()) < 0:
        raise SystemExit('marker missing')
    return {'rows': rows, 'm2_label': texts[0], 'constants': len(cons.items), 'push_lists': len(pointers),
            'init_menu': 'mMenuArray = [m0, m1, m2]'}


def swap_movie(data, fn):
    """The stream with the MP20 movie replaced by fn(movie); every other record stays as it is."""
    assets = fs.group(fs.records(data))
    hits = [k for k, (shdr, _) in enumerate(assets) if fs.names(shdr) == (MP20, 'APX')]
    if len(hits) != 1:
        raise SystemExit('%s found %d times' % (MP20, len(hits)))
    k = hits[0]
    shdr, bodies = assets[k]
    movie = fs.payload(bodies, fs.declared(shdr))
    new, report = fn(movie)
    out_assets = [(fs.with_declared(s, len(new)), fs.rpak(new)) if i == k else (s, b) for i, (s, b) in enumerate(assets)]
    stream = fs.frame(out_assets)
    records = fs.validate(stream)
    back = fs.group(fs.records(stream))
    if len(back) != len(assets):
        raise SystemExit('asset count changed')
    for i, ((s0, b0), (s1, b1)) in enumerate(zip(assets, back)):
        if i == k:
            if fs.payload(b1, fs.declared(s1)) != new or fs.declared(s1) != len(new):
                raise SystemExit('MP20 differs on read back')
        elif s0 != s1 or b0 != b1:
            raise SystemExit('asset %d (%s) changed' % (i, fs.names(s0)[0]))
    report.update({'records': records, 'assets': len(back), 'movie_sha256': hashlib.sha256(new).hexdigest(), 'movie_index': k,
                   'others_identical': len(back) - 1})
    return stream, report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in', dest='src', default=str(Path(GAME_DIR) / 'randomizer' / 'ds2coop_frontend.str'))
    ap.add_argument('--out', default=str(ROOT / 'build' / 'frontend' / 'ds2coop_frontend.str'))
    ap.add_argument('--pitch', type=float, default=None)
    ap.add_argument('--lift', type=float, default=0.0)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    src = Path(args.src)
    target = Path(args.out)
    if target.resolve() == src.resolve():
        raise SystemExit('--out must not be the input')
    data = src.read_bytes()
    stream, report = swap_movie(data, lambda m: add_third_row(m, args.pitch, args.lift))
    if not args.check:
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp = target.with_suffix('.tmp')
        tmp.write_bytes(stream)
        tmp.replace(target)
    report.update({'in': str(src), 'in_crc32': '%08X' % zlib.crc32(data), 'out': str(target), 'size': len(stream),
                   'crc32': '%08X' % zlib.crc32(stream), 'sha256': hashlib.sha256(stream).hexdigest(), 'written': not args.check})
    print(json.dumps(report, indent=1))


if __name__ == '__main__':
    main()
