"""Read and write Dead Space 2 animation banks (.bnk.win, version 9).

Format knowledge comes from our own dumps of the retail player cutscene banks,
cross-checked against the read-only decoder of the DS2Import UE5 plugin
(a friend's importer; none of its code is used here).

Container (all pointers are file offsets, fixed up at load through the
relocation list):
  +0x00 body end (the relocation list follows the body)
  +0x04 version 9          +0x0C skeleton hash (sdbm of the rig name)
  +0x20 largest node count of one action graph
  +0x24/+0x28 action count / table      +0x2C/+0x30 owner count / table
  +0x34/+0x38 event definition count / table
  action header: name pointer, root node pointer, then the name string
  node: type u8, child count u8; children at +0x0C. Leaf: type 0x16, no
        children, leaf id i16 at +2, timing block at +4, owner at +0x0C
  timing block: start, end, step (f32); retail clips use end = (frames-1)*step
  owner (marker 0x0C): +0 sdbm(action name), +8 frames, +0x10 extra channel
        count, +0x18 rig mask hash, +0x2C channel layout, +0x34/+0x38 events;
        the channel stream starts at owner+0x40
  relocation list: u32 field offsets, descending, then a zero

Channel stream: one record per channel, for every bone of the rig mask in
order, then the extra channels (a camera clip's FOV). Layout 0x80000004 has
seven channels per bone: rotation as quaternion log x, y, z, a zero channel,
then translation x, y, z. A record starts with u16 (count << 4 | family):
  0..3  polynomial of that degree, coefficients f32 from +1 every 3 bytes
        (each overlaps the byte before it), highest degree first
  4     count raw f32 from +2
  5     segments: u16 prelude (layouts with prelude), then count child
        records sharing one end sample with the next
  6, 7  bias f32 at +1, scale f32 at +4, then u8 / u16 codes from +8
  C, D  constant 0 / 1
The writer emits only families 4, 7, 0, C and D, all of which occur at the
top level of retail 0x80000004 clips.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field

FILLER = 0x5F
LEAF_NODE = 0x16
STANDARD_MARKERS = (0x0C, 0x0B)
SHIFTED_MARKER = 0x0A
COMPACT_MARKER = 0x09000000
LAYOUT_QLOG = 0x80000004
CHANNELS = {0x00000001: 10, 0x00000003: 10, 0x00000004: 10,
            0x80000001: 7, 0x80000003: 7, 0x80000004: 7}
PRELUDE = {0x00000001: 0, 0x80000001: 0,
           0x00000003: 2, 0x00000004: 2, 0x80000003: 2, 0x80000004: 2}
MAX_COUNT = 0xFFF


class BankError(ValueError):
    pass


def sdbm(text: str) -> int:
    value = 0
    for byte in text.lower().encode("ascii"):
        value = (value * 65599 + byte) & 0xFFFFFFFF
    return value


def f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def _u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _f32(data, offset):
    return struct.unpack_from("<f", data, offset)[0]


def _cstring(data, offset):
    end = data.index(b"\x00", offset)
    return data[offset:end].decode("ascii")


# ---------------------------------------------------------------- rig

@dataclass
class Rig:
    skeleton_hash: int
    name: str
    parents: list
    bone_hashes: list
    inverse_binds: list
    masks: dict          # rig mask hash -> active bone indices


def read_rig(data: bytes) -> Rig:
    count, table = _u32(data, 0x18), _u32(data, 0x1C)
    if count < 1:
        raise BankError("rig has no skeleton")
    skeleton_hash, name_offset, bones, parent_table, matrix_table, hash_table = struct.unpack_from("<6I", data, table)
    parents = [struct.unpack_from("<i", data, parent_table + 0x10 * i)[0] for i in range(bones)]
    binds = [struct.unpack_from("<16f", data, matrix_table + 0x40 * i) for i in range(bones)]
    hashes = [0] * bones
    for row in range(bones):
        bone_hash, index = _u32(data, hash_table + 0x14 * row), _u32(data, hash_table + 0x14 * row + 0x10)
        hashes[index] = bone_hash
    masks = {}
    definitions = _u32(data, 0x24)
    cursor, first_payload = definitions, len(data)
    while cursor + 8 <= first_payload:
        mask_hash, pointer = _u32(data, cursor), _u32(data, cursor + 4)
        first_payload = min(first_payload, pointer)
        offset, bits = _u32(data, pointer), _u32(data, pointer + 4)
        raw = data[offset:offset + (bits + 7) // 8]
        masks[mask_hash] = [i for i in range(bits) if raw[i // 8] & (1 << (i % 8))]
        cursor += 8
    return Rig(skeleton_hash, _cstring(data, name_offset), parents, hashes, binds, masks)


# ---------------------------------------------------------------- bank

@dataclass
class Leaf:
    action: str
    leaf_id: int
    node: int
    owner: int
    timing: tuple


@dataclass
class Action:
    index: int
    name: str
    header: int
    root: int
    leaves: list


@dataclass
class Bank:
    data: bytes
    body_end: int
    relocations: list
    skeleton_hash: int
    actions: list
    owners: list

    def action(self, name: str) -> Action:
        for action in self.actions:
            if action.name == name:
                return action
        raise BankError("no action %r" % name)


def read_bank(data: bytes) -> Bank:
    if len(data) < 0x50 or _u32(data, 4) != 9:
        raise BankError("not a version 9 bank")
    body_end = _u32(data, 0)
    words = struct.unpack_from("<%dI" % ((len(data) - body_end) // 4), data, body_end)
    if not words or words[-1] != 0:
        raise BankError("relocation list has no terminator")
    relocations = list(words[:-1])
    owners = [_u32(data, _u32(data, 0x30) + 4 * i) for i in range(_u32(data, 0x2C))]
    actions = []
    table = _u32(data, 0x28)
    for index in range(_u32(data, 0x24)):
        header = _u32(data, table + 4 * index)
        name, root = _cstring(data, _u32(data, header)), _u32(data, header + 4)
        leaves, seen = [], set()

        def visit(node):
            if node in seen:
                return
            seen.add(node)
            kind, children = data[node], data[node + 1]
            if kind == LEAF_NODE and children == 0:
                timing = _u32(data, node + 4)
                leaves.append(Leaf(name, struct.unpack_from("<h", data, node + 2)[0], node, _u32(data, node + 0x0C),
                                   struct.unpack_from("<3f", data, timing)))
                return
            for child in range(children):
                visit(_u32(data, node + 0x0C + 4 * child))

        visit(root)
        actions.append(Action(index, name, header, root, sorted(leaves, key=lambda leaf: leaf.leaf_id)))
    return Bank(data, body_end, relocations, _u32(data, 0x0C), actions, owners)


@dataclass
class OwnerInfo:
    marker: int
    frames: int
    mask: int
    extra: int
    layout: int
    stream: int
    events: int


def owner_info(data: bytes, owner: int) -> OwnerInfo:
    marker = _u32(data, owner + 4)
    if marker in STANDARD_MARKERS:
        return OwnerInfo(marker, _u32(data, owner + 8), _u32(data, owner + 0x18), _u32(data, owner + 0x10),
                         _u32(data, owner + 0x2C), owner + 0x40, _u32(data, owner + 0x34))
    if marker == SHIFTED_MARKER:
        return OwnerInfo(marker, _u32(data, owner + 8), _u32(data, owner + 0x14), _u32(data, owner + 0x10),
                         _u32(data, owner + 0x28), owner + 0x40, _u32(data, owner + 0x30))
    if marker == COMPACT_MARKER:
        return OwnerInfo(marker, _u32(data, owner + 0x18), _u32(data, owner + 0x14), 0,
                         _u32(data, owner + 0x20), owner + 0x30, _u32(data, owner + 0x28))
    raise BankError("unknown owner marker 0x%08X at 0x%X" % (marker, owner))


# ---------------------------------------------------------------- channels

def _direct_size(family: int, count: int) -> int:
    if family in (0xC, 0xD):
        return 2
    if 0 <= family <= 3:
        return 5 + 3 * family
    if family == 4:
        return 2 + 4 * count
    if family == 6:
        return 8 + count
    if family == 7:
        return 8 + 2 * count
    raise BankError("unknown channel family 0x%X" % family)


def _decode_direct(data: bytes, start: int, family: int, count: int) -> list:
    if family == 0xC:
        return [0.0] * count
    if family == 0xD:
        return [1.0] * count
    if 0 <= family <= 3:
        coefficients = [_f32(data, start + 1 + 3 * i) for i in range(family + 1)]
        out = []
        for x in range(count):
            value = 0.0
            for coefficient in coefficients:
                value = value * x + coefficient
            out.append(value)
        return out
    if family == 4:
        return list(struct.unpack_from("<%df" % count, data, start + 2))
    bias, scale = _f32(data, start + 1), _f32(data, start + 4)
    codes = data[start + 8:start + 8 + count] if family == 6 else struct.unpack_from("<%dH" % count, data, start + 8)
    return [bias + scale * code for code in codes]


def decode_channel(data: bytes, start: int, frames: int, prelude: int):
    """One channel record: (samples, end offset, family)."""
    command = _u16(data, start)
    count, family = command >> 4, command & 0xF
    if family != 5:
        if count != frames:
            raise BankError("record at 0x%X has %d samples, owner has %d" % (start, count, frames))
        return _decode_direct(data, start, family, count), start + _direct_size(family, count), family
    cursor = start + 2 + prelude
    samples = []
    for child in range(count):
        child_command = _u16(data, cursor)
        child_count, child_family = child_command >> 4, child_command & 0xF
        values = _decode_direct(data, cursor, child_family, child_count)
        samples.extend(values if child == 0 else values[1:])
        cursor += _direct_size(child_family, child_count)
    if len(samples) != frames:
        raise BankError("segments at 0x%X cover %d samples, owner has %d" % (start, len(samples), frames))
    return samples, cursor, 5


@dataclass
class Clip:
    frames: int
    layout: int
    mask: int
    bones: dict                       # bone index -> list of channels -> samples
    extras: list = field(default_factory=list)
    step: float = 2.0
    # the encoded record of each channel as read from a bank: (bone, channel)
    # or ("extra", index) -> bytes; reused verbatim while the samples match
    records: dict = field(default_factory=dict)


def decode_owner(bank: Bank, owner: int, rig: Rig) -> Clip:
    info = owner_info(bank.data, owner)
    if info.layout not in CHANNELS:
        raise BankError("layout 0x%08X is not supported" % info.layout)
    bones = rig.masks[info.mask]
    per_bone, prelude = CHANNELS[info.layout], PRELUDE[info.layout]
    cursor, channels, raw = info.stream, [], []
    for _ in range(len(bones) * per_bone + info.extra):
        start = cursor
        samples, cursor, _family = decode_channel(bank.data, cursor, info.frames, prelude)
        channels.append(samples)
        raw.append(bank.data[start:cursor])
    records = {}
    for i, bone in enumerate(bones):
        for channel in range(per_bone):
            records[(bone, channel)] = raw[i * per_bone + channel]
    for i in range(info.extra):
        records[("extra", i)] = raw[len(bones) * per_bone + i]
    clip = Clip(info.frames, info.layout, info.mask,
                {bone: channels[i * per_bone:(i + 1) * per_bone] for i, bone in enumerate(bones)},
                channels[len(bones) * per_bone:], records=records)
    return clip


# ---------------------------------------------------------------- rotations

def qlog_to_quaternion(x: float, y: float, z: float):
    """WXYZ from the stored quaternion log (rotation angle is twice its length)."""
    angle = math.sqrt(x * x + y * y + z * z)
    if angle < 1e-12:
        return (1.0, 0.0, 0.0, 0.0)
    s = math.sin(angle) / angle
    return (math.cos(angle), x * s, y * s, z * s)


QLOG_LAYOUTS = (0x80000004, 0x00000004)


def pose(clip: Clip, bone: int, frame: int):
    """(translation, WXYZ rotation, scale) of one bone in one frame, for every
    layout: 0x80000004/0x00000004 store a quaternion log and a zero channel,
    the others a quaternion XYZW; ten-channel layouts add scale x, y, z."""
    ch = clip.bones[bone]
    if clip.layout in QLOG_LAYOUTS:
        q = qlog_to_quaternion(ch[0][frame], ch[1][frame], ch[2][frame])
    else:
        x, y, z, w = (ch[i][frame] for i in range(4))
        n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
        q = (w / n, x / n, y / n, z / n)
    t = (ch[4][frame], ch[5][frame], ch[6][frame])
    s = (ch[7][frame], ch[8][frame], ch[9][frame]) if len(ch) >= 10 else (1.0, 1.0, 1.0)
    return t, q, s


def quaternion_to_qlog(w: float, x: float, y: float, z: float, previous=None):
    """Quaternion log of a WXYZ rotation, kept on the side of `previous` (a
    WXYZ tuple) so consecutive samples do not jump between q and -q."""
    norm = math.sqrt(w * w + x * x + y * y + z * z)
    w, x, y, z = w / norm, x / norm, y / norm, z / norm
    if previous is not None:
        if w * previous[0] + x * previous[1] + y * previous[2] + z * previous[3] < 0:
            w, x, y, z = -w, -x, -y, -z
    elif w < 0:
        w, x, y, z = -w, -x, -y, -z
    vector = math.sqrt(x * x + y * y + z * z)
    if vector < 1e-12:
        return (0.0, 0.0, 0.0), (w, x, y, z)
    angle = math.atan2(vector, w)
    return (x / vector * angle, y / vector * angle, z / vector * angle), (w, x, y, z)


# ---------------------------------------------------------------- encoding

def _force_low_byte(value: float, byte: int) -> float:
    raw = bytearray(struct.pack("<f", value))
    raw[0] = byte
    return struct.unpack("<f", bytes(raw))[0]


def _with_low_byte(value: float, byte: int, upward: bool) -> float:
    """The nearest float32 at or below (or above) `value` whose lowest byte is
    `byte`. Retail records overlap that byte with the one before it."""
    bits = struct.unpack("<I", struct.pack("<f", value))[0]
    if value == 0.0:
        bits = 0 if upward else 0x80000000   # tiny denormal on the wanted side
    candidate = (bits & ~0xFF) | byte
    result = struct.unpack("<f", struct.pack("<I", candidate))[0]
    negative = bits >> 31
    if (result > value and not upward) or (result < value and upward):
        step = 0x100 if (negative == 0) == upward else -0x100
        candidate += step
        result = struct.unpack("<f", struct.pack("<I", candidate & 0xFFFFFFFF))[0]
    return result


def _quantized_head(command: int, family: int, bias: float, scale: float):
    head = bytearray(8)
    struct.pack_into("<H", head, 0, command | family)
    high = head[1]
    head[1:5] = struct.pack("<f", bias)
    top = head[4]
    head[4:8] = struct.pack("<f", scale)
    if head[1] != high or head[4] != top:
        return None
    return bytes(head)


def _constant_u8(value: float, high: int, count: int, command: int):
    """Family 6 with one repeated code: bias just below the value, scale
    bridging the rest, so the forced bytes cost well under one ulp."""
    bias = _with_low_byte(value, high, upward=False)
    rest = value - bias
    top = struct.pack("<f", bias)[3]
    best = None
    for code in (255, 254, 253, 252, 128):
        for upward in (False, True):
            scale = _with_low_byte(rest / code, top, upward)
            if not math.isfinite(scale) or scale <= 0.0:
                continue
            error = abs(f32(bias + f32(scale * code)) - value)
            if best is None or error < best[0]:
                best = (error, scale, code)
        if best and best[0] == 0.0:
            break
    if best is None:
        return None
    head = _quantized_head(command, 0x6, bias, best[1])
    return head + bytes([best[2]]) * count if head else None


def encode_channel(samples, mode: str = "quantized") -> bytes:
    """One top-level record. mode: 'raw' keeps every float32 exactly,
    'quantized' stores u16 codes (error at most range/131070)."""
    count = len(samples)
    if not 1 <= count <= MAX_COUNT:
        raise BankError("a channel needs 1..%d samples, got %d" % (MAX_COUNT, count))
    values = [f32(v) for v in samples]
    if any(not math.isfinite(v) for v in values):
        raise BankError("channel has a non-finite sample")
    command = count << 4
    high = command >> 8
    if all(v == 0.0 for v in values):
        return struct.pack("<H", command | 0xC)
    if all(v == 1.0 for v in values):
        return struct.pack("<H", command | 0xD)
    raw = struct.pack("<H", command | 0x4) + struct.pack("<%df" % count, *values)
    if mode == "raw":
        return raw
    if all(v == values[0] for v in values):
        # a degree 0 polynomial is exact only when the float's first byte
        # already equals the command's high byte; otherwise u8 codes absorb it
        value = values[0]
        if _with_low_byte(value, high, upward=False) == value:
            return struct.pack("<H", command)[:1] + struct.pack("<f", value)
        constant = _constant_u8(value, high, count, command)
        return constant if constant is not None and len(constant) < len(raw) else raw
    if count < 4:
        return raw
    low, high_value = min(values), max(values)
    # bias occupies bytes 1..4 and scale 4..7: bias's low byte is the command's
    # high byte and its top byte is the scale's low byte
    bias = _with_low_byte(low, high, upward=False)
    scale = _with_low_byte(max((high_value - bias) / 65000.0, 1e-30), struct.pack("<f", bias)[3], upward=True)
    if not (math.isfinite(bias) and math.isfinite(scale)) or scale <= 0.0:
        return raw
    codes = [int(round((v - bias) / scale)) for v in values]
    if min(codes) < 0 or max(codes) > 0xFFFF:
        return raw
    head = _quantized_head(command, 0x7, bias, scale)
    if head is None:
        raise BankError("overlapping bytes do not agree")
    return head + struct.pack("<%dH" % count, *codes)


def _reusable(record, samples, frames) -> bool:
    if record is None:
        return False
    try:
        values, end, _family = decode_channel(record, 0, frames, PRELUDE[LAYOUT_QLOG])
    except (BankError, struct.error, ValueError):
        return False
    return end == len(record) and values == list(samples)


def encode_stream(clip: Clip, rig: Rig, mode: str = "quantized") -> bytes:
    """The channel stream. A channel whose samples still equal the record it
    was read from keeps that record byte for byte."""
    if clip.layout != LAYOUT_QLOG:
        raise BankError("the writer only emits layout 0x80000004")
    bones = rig.masks[clip.mask]
    if sorted(clip.bones) != bones:
        raise BankError("clip bones do not match rig mask 0x%08X" % clip.mask)
    out = bytearray()
    keys = [((bone, channel), samples) for bone in bones for channel, samples in enumerate(clip.bones[bone])]
    if any(len(clip.bones[bone]) != 7 for bone in bones):
        raise BankError("every bone needs 7 channels")
    keys += [(("extra", i), samples) for i, samples in enumerate(clip.extras)]
    for key, samples in keys:
        record = clip.records.get(key)
        out += record if _reusable(record, samples, clip.frames) else encode_channel(samples, mode)
    return bytes(out)


# ---------------------------------------------------------------- bank edits

class _Appender:
    def __init__(self, bank: Bank):
        self.body = bytearray(bank.data[:bank.body_end])
        self.relocations = set(bank.relocations)
        self.align()

    def align(self, boundary: int = 16):
        while len(self.body) % boundary:
            self.body.append(FILLER)

    def put(self, raw: bytes) -> int:
        self.align()
        offset = len(self.body)
        self.body += raw
        return offset

    def pointer(self, at: int, target: int):
        struct.pack_into("<I", self.body, at, target)
        self.relocations.add(at)

    def finish(self) -> bytes:
        self.align()
        struct.pack_into("<I", self.body, 0, len(self.body))
        fields = sorted(self.relocations, reverse=True)
        return bytes(self.body) + struct.pack("<%dI" % (len(fields) + 1), *fields, 0)


def _owner_header(name_hash: int, clip: Clip) -> bytes:
    """A marker 0x0C owner with no events. +0x20 (-1.0), +0x24 (0x7E3F) and
    +0x28 (1) carry the values of 92 of the 98 retail player cutscene owners;
    their meaning is unknown."""
    header = struct.pack("<IIIIIIIIfIIIIII", name_hash, 0x0C, clip.frames, 0, len(clip.extras), 0, clip.mask, 0,
                         -1.0, 0x7E3F, 1, clip.layout, 0, 0, 0)
    return header + bytes([FILLER]) * 4


def _put_owner(appender: _Appender, name_hash: int, clip: Clip, rig: Rig, mode: str) -> int:
    return appender.put(_owner_header(name_hash, clip) + encode_stream(clip, rig, mode))


def _put_leaf(appender: _Appender, owner: int, clip: Clip, leaf_id: int = 1) -> int:
    timing = appender.put(struct.pack("<3f", 0.0, (clip.frames - 1) * clip.step, clip.step) + bytes([FILLER]) * 4)
    leaf = appender.put(struct.pack("<BBhIII", LEAF_NODE, 0, leaf_id, 0, 0, 0))
    appender.pointer(leaf + 4, timing)
    appender.pointer(leaf + 0x0C, owner)
    return leaf


def add_action(bank_data: bytes, name: str, clip: Clip, rig: Rig, mode: str = "quantized") -> bytes:
    """A copy of the bank with one more action holding `clip`."""
    bank = read_bank(bank_data)
    if any(action.name == name or sdbm(action.name) == sdbm(name) for action in bank.actions):
        raise BankError("action %r (or its hash) exists already" % name)
    appender = _Appender(bank)
    owner = _put_owner(appender, sdbm(name), clip, rig, mode)
    leaf = _put_leaf(appender, owner, clip)
    # root: a one-child node with an empty parameter block, as in retail
    block = appender.put(bytes(8) + bytes([FILLER]) * 8)
    root = appender.put(struct.pack("<BBhIII", 0x02, 1, 0, 0, 0, 0))
    appender.pointer(root + 4, block)
    appender.pointer(root + 0x0C, leaf)
    name_bytes = name.encode("ascii") + b"\x00"
    header = appender.put(bytes(8) + bytes([FILLER]) * 8 + name_bytes)
    appender.pointer(header, header + 0x10)
    appender.pointer(header + 4, root)
    actions = [_u32(bank.data, _u32(bank.data, 0x28) + 4 * i) for i in range(len(bank.actions))] + [header]
    table = appender.put(bytes(4 * len(actions)))
    for i, target in enumerate(actions):
        appender.pointer(table + 4 * i, target)
    owners = bank.owners + [owner]
    owner_table = appender.put(bytes(4 * len(owners)))
    for i, target in enumerate(owners):
        appender.pointer(owner_table + 4 * i, target)
    struct.pack_into("<I", appender.body, 0x24, len(actions))
    appender.pointer(0x28, table)
    struct.pack_into("<I", appender.body, 0x2C, len(owners))
    appender.pointer(0x30, owner_table)
    struct.pack_into("<I", appender.body, 0x20, max(_u32(bank.data, 0x20), 2))
    return appender.finish()


def set_priority(bank_data: bytes, action_name: str, priority: int) -> bytes:
    """A copy where `action_name` overrides a state of the same name from
    another bank: the state table keeps the higher priority (u32 at the root
    node's parameter block, _ResolveStateOverride 0x013B8FE0; retail uses 0, and
    with equal priorities the bank attached last wins)."""
    bank = read_bank(bank_data)
    root = bank.action(action_name).root
    if bank_data[root] != 0x02:
        raise BankError("action %r has no type 2 root" % action_name)
    block = _u32(bank_data, root + 4)
    out = bytearray(bank_data)
    struct.pack_into("<I", out, block, priority)
    return bytes(out)


BANK_SET_HASH = 0x805E58C4   # +0x08 of every retail player bank


def new_bank(clips: dict, rig: Rig, mode: str = "quantized", bank_set: int = BANK_SET_HASH) -> bytes:
    """A bank holding only `clips` (action name -> Clip) for `rig`'s skeleton,
    shaped like a retail player bank without transitions."""
    if not clips:
        raise BankError("a bank needs at least one clip")
    if len({sdbm(name) for name in clips}) != len(clips):
        raise BankError("two clip names share one hash")
    header = bytearray(struct.pack("<20I", 0, 9, bank_set, rig.skeleton_hash, 0, 0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    header[0x44:0x50] = bytes([FILLER]) * 12
    appender = _Appender(Bank(bytes(header) + struct.pack("<I", 0), 0x50, [], rig.skeleton_hash, [], []))
    appender.pointer(0x40, 0x44)   # empty table, as in retail
    headers, owners = [], []
    for name, clip in clips.items():
        owner = _put_owner(appender, sdbm(name), clip, rig, mode)
        leaf = _put_leaf(appender, owner, clip)
        block = appender.put(bytes(8) + bytes([FILLER]) * 8)
        root = appender.put(struct.pack("<BBhIII", 0x02, 1, 0, 0, 0, 0))
        appender.pointer(root + 4, block)
        appender.pointer(root + 0x0C, leaf)
        action = appender.put(bytes(8) + bytes([FILLER]) * 8 + name.encode("ascii") + b"\x00")
        appender.pointer(action, action + 0x10)
        appender.pointer(action + 4, root)
        headers.append(action)
        owners.append(owner)
    for count_at, table_at, targets in ((0x24, 0x28, headers), (0x2C, 0x30, owners)):
        table = appender.put(bytes(4 * len(targets)))
        for i, target in enumerate(targets):
            appender.pointer(table + 4 * i, target)
        struct.pack_into("<I", appender.body, count_at, len(targets))
        appender.pointer(table_at, table)
    appender.pointer(0x38, _u32(appender.body, 0x28))   # no transitions; a valid empty array
    return appender.finish()


def replace_leaf(bank_data: bytes, action_name: str, clip: Clip, rig: Rig, mode: str = "quantized",
                 leaf_id: int | None = None) -> bytes:
    """A copy of the bank where one leaf of an existing action plays `clip`.
    The old owner stays in place, unreferenced by that leaf."""
    bank = read_bank(bank_data)
    action = bank.action(action_name)
    leaves = [leaf for leaf in action.leaves if leaf_id is None or leaf.leaf_id == leaf_id]
    if len(leaves) != 1:
        raise BankError("action %r has %d matching leaves" % (action_name, len(leaves)))
    old = leaves[0]
    appender = _Appender(bank)
    owner = _put_owner(appender, _u32(bank.data, old.owner), clip, rig, mode)
    timing = appender.put(struct.pack("<3f", 0.0, (clip.frames - 1) * clip.step, clip.step) + bytes([FILLER]) * 4)
    appender.pointer(old.node + 4, timing)
    appender.pointer(old.node + 0x0C, owner)
    owners = bank.owners + [owner]
    owner_table = appender.put(bytes(4 * len(owners)))
    for i, target in enumerate(owners):
        appender.pointer(owner_table + 4 * i, target)
    struct.pack_into("<I", appender.body, 0x2C, len(owners))
    appender.pointer(0x30, owner_table)
    return appender.finish()
