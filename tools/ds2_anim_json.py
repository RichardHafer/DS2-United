"""Animation clips between DS2 banks and a plain JSON format (ds2anim.v1).

    python -I tools/ds2_anim_json.py info   <bank> <rig>
    python -I tools/ds2_anim_json.py export <bank> <rig> <action> <out.json> [--leaf N] [--ue] [--unit-scale 100] [--no-rig-scale]
    python -I tools/ds2_anim_json.py build  <base bank> <rig> <clip.json> <out bank> --action NAME [--replace [--leaf N]] [--raw] [--mask HEX]

With --replace the named action's clip is edited: bones and extra channels
(the camera FOV) missing from the document stay as they are in that clip, so a
document with only m_camera changes the cutscene camera and nothing else.

ds2anim.v1:
  {"schema": "ds2anim.v1", "coordinates": "native" | "ue", "unit_scale": 100.0,
   "rig_scale_baked": true, "sample_rate": 30.0, "frames": N,
   "bones": [{"name": "m_camera" or "hash_39C5E697", "hash": "0x39C5E697",
              "translations": [[x, y, z], ...], "rotations": [[...], ...]}, ...],
   "extras": [[...], ...]}
Native rotations are WXYZ in the rig's local space; translations are in rig
units (the rig's base scale, player.rcb +0x38 = 1.818, turns them into world
metres). "ue" documents use the DS2Import convention: UE = (x, z, y) * scale
with scale = unit_scale (times the rig scale when rig_scale_baked), and
rotations XYZW = (-x, -z, -y, w). A bone is found by its hash: the "hash"
field, a name of the form hash_XXXXXXXX or bone_NNN_XXXXXXXX, or sdbm(name).
Bones of the rig mask that a document leaves out keep the rig's bind pose.
"""
from __future__ import annotations

import json
import math
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ds2_bnk as bnk  # noqa: E402

SCHEMA = "ds2anim.v1"
FULL_BODY_MASK = 0xFA896472   # the 131-bone mask of the player's cutscene clips
CAMERA_BONE_HASH = 0x39C5E697  # m_camera


# ---------------------------------------------------------------- matrices

def _matrix(rows):
    return [list(rows[i * 4:i * 4 + 4]) for i in range(4)]


def _mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _inverse(a):
    rows = [list(r) + [float(i == j) for j in range(4)] for i, r in enumerate(a)]
    for column in range(4):
        pivot = max(range(column, 4), key=lambda r: abs(rows[r][column]))
        if abs(rows[pivot][column]) < 1e-12:
            raise bnk.BankError("singular bind matrix")
        rows[column], rows[pivot] = rows[pivot], rows[column]
        div = rows[column][column]
        rows[column] = [v / div for v in rows[column]]
        for r in range(4):
            if r != column:
                factor = rows[r][column]
                rows[r] = [x - factor * y for x, y in zip(rows[r], rows[column])]
    return [r[4:] for r in rows]


def _transpose(a):
    return [list(r) for r in zip(*a)]


def _quaternion(m):
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        q = [s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s]
    else:
        i = max(range(3), key=lambda k: m[k][k])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = math.sqrt(max(0.0, 1 + m[i][i] - m[j][j] - m[k][k])) * 2
        q = [0.0] * 4
        q[0], q[i + 1] = (m[k][j] - m[j][k]) / s, s / 4
        q[j + 1], q[k + 1] = (m[j][i] + m[i][j]) / s, (m[k][i] + m[i][k]) / s
    norm = math.sqrt(sum(v * v for v in q))
    return [v / norm for v in q]


def local_bind(rig: bnk.Rig):
    """Per bone (translation, WXYZ rotation) of the bind pose, relative to the
    parent, in the same convention as the clip channels."""
    world = [_transpose(_inverse(_matrix(rows))) for rows in rig.inverse_binds]
    out = []
    for bone, parent in enumerate(rig.parents):
        m = world[bone] if parent < 0 else _mul(_inverse(world[parent]), world[bone])
        out.append(([m[i][3] for i in range(3)], _quaternion(m)))
    return out


def rig_scale(rig_data: bytes) -> float:
    return struct.unpack_from("<f", rig_data, 0x38)[0] if struct.unpack_from("<I", rig_data, 4)[0] == 26 else 1.0


# ---------------------------------------------------------------- conversions

def to_ue(t, q, scale):
    w, x, y, z = q
    return [t[0] * scale, t[2] * scale, t[1] * scale], [-x, -z, -y, w]


def from_ue(t, q, scale):
    x, y, z, w = q
    return [t[0] / scale, t[2] / scale, t[1] / scale], [w, -x, -z, -y]


def bone_hash(entry) -> int:
    if entry.get("hash"):
        return int(str(entry["hash"]), 16)
    name = str(entry.get("name", ""))
    match = re.fullmatch(r"(?:hash_|bone_\d+_)([0-9A-Fa-f]{8})", name)
    return int(match.group(1), 16) if match else bnk.sdbm(name)


def clip_to_document(clip: bnk.Clip, rig: bnk.Rig, ue: bool = False, unit_scale: float = 100.0,
                     rig_factor: float = 1.0, names: dict | None = None) -> dict:
    scale = unit_scale * rig_factor
    bones = []
    for bone in sorted(clip.bones):
        translations, rotations, previous = [], [], None
        for frame in range(clip.frames):
            t, q, _scale = bnk.pose(clip, bone, frame)
            if previous is not None and sum(a * b for a, b in zip(q, previous)) < 0:
                q = tuple(-v for v in q)
            previous = q
            t = list(t)
            if ue:
                t, q = to_ue(t, q, scale)
            translations.append([round(v, 7) for v in t])
            rotations.append([round(v, 8) for v in q])
        h = rig.bone_hashes[bone]
        bones.append({"name": (names or {}).get(h, "hash_%08X" % h), "hash": "0x%08X" % h, "index": bone,
                      "translations": translations, "rotations": rotations})
    return {"schema": SCHEMA, "coordinates": "ue" if ue else "native", "unit_scale": unit_scale,
            "rig_scale_baked": rig_factor != 1.0, "rig_scale": rig_factor, "sample_rate": 30.0 / (clip.step / 2.0),
            "frames": clip.frames, "mask": "0x%08X" % clip.mask, "bones": bones,
            "extras": [[round(v, 7) for v in e] for e in clip.extras]}


def _slerp(a, b, t):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0:
        b, dot = [-v for v in b], -dot
    if dot > 0.9995:
        out = [x + (y - x) * t for x, y in zip(a, b)]
    else:
        angle = math.acos(dot)
        sa, sb = math.sin((1 - t) * angle), math.sin(t * angle)
        out = [(x * sa + y * sb) / math.sin(angle) for x, y in zip(a, b)]
    norm = math.sqrt(sum(v * v for v in out))
    return [v / norm for v in out]


def resample(doc: dict, rate: float = 30.0) -> dict:
    """The document at `rate` frames per second (retail clips play at 30 with
    timing step 2; other steps are unproven)."""
    source = float(doc.get("sample_rate", rate))
    frames = int(doc["frames"])
    if abs(source - rate) < 1e-3 or frames < 2:
        return doc
    duration = (frames - 1) / source
    count = int(round(duration * rate)) + 1
    out = dict(doc, sample_rate=rate, frames=count, bones=[], extras=[])

    def at(index):
        x = min(index / rate * source, frames - 1)
        i = min(int(x), frames - 2)
        return i, x - i

    for entry in doc["bones"]:
        t_out, q_out = [], []
        for index in range(count):
            i, f = at(index)
            ta, tb = entry["translations"][i], entry["translations"][i + 1]
            t_out.append([x + (y - x) * f for x, y in zip(ta, tb)])
            q_out.append(_slerp(entry["rotations"][i], entry["rotations"][i + 1], f))
        out["bones"].append(dict(entry, translations=t_out, rotations=q_out))
    for extra in doc.get("extras", []):
        values = []
        for index in range(count):
            i, f = at(index)
            values.append(extra[i] + (extra[i + 1] - extra[i]) * f)
        out["extras"].append(values)
    return out


def document_to_clip(doc: dict, rig: bnk.Rig, rig_data: bytes, mask: int | None = None, base: bnk.Clip | None = None) -> bnk.Clip:
    """With `base` (the clip being replaced) the bones and extra channels the
    document leaves out come from that clip instead of the bind pose, so a
    document holding only m_camera edits the camera and nothing else."""
    if doc.get("schema") != SCHEMA:
        raise bnk.BankError("not a %s document" % SCHEMA)
    doc = resample(doc)
    frames = int(doc["frames"])
    if not 1 <= frames <= bnk.MAX_COUNT:
        raise bnk.BankError("frames must be 1..%d" % bnk.MAX_COUNT)
    if base is not None and base.frames != frames:
        raise bnk.BankError("the document has %d frames at 30 fps, the clip it edits has %d" % (frames, base.frames))
    if mask is None and base is not None:
        mask = base.mask
    mask = mask if mask is not None else int(str(doc.get("mask", "0x%08X" % FULL_BODY_MASK)), 16)
    if mask not in rig.masks:
        raise bnk.BankError("the rig has no mask 0x%08X" % mask)
    ue = doc.get("coordinates") == "ue"
    scale = float(doc.get("unit_scale", 100.0)) * (rig_scale(rig_data) if doc.get("rig_scale_baked", True) else 1.0)
    index_of = {h: i for i, h in enumerate(rig.bone_hashes)}
    given = {}
    for entry in doc["bones"]:
        h = bone_hash(entry)
        if h not in index_of:
            raise bnk.BankError("bone %r (0x%08X) is not in the rig" % (entry.get("name"), h))
        if len(entry["translations"]) != frames or len(entry["rotations"]) != frames:
            raise bnk.BankError("bone %r does not have %d frames" % (entry.get("name"), frames))
        given[index_of[h]] = entry
    bind = local_bind(rig)
    bones, records = {}, {}
    for bone in rig.masks[mask]:
        entry = given.get(bone)
        if entry is None and base is not None and bone in base.bones:
            bones[bone] = [list(channel) for channel in base.bones[bone]]
            records.update({key: value for key, value in base.records.items() if key[0] == bone})
            continue
        channels = [[] for _ in range(7)]
        previous = None
        for frame in range(frames):
            if entry is None:
                t, q = bind[bone]
            else:
                t, q = entry["translations"][frame], entry["rotations"][frame]
                if ue:
                    t, q = from_ue(t, q, scale)
            (lx, ly, lz), previous = bnk.quaternion_to_qlog(*q, previous=previous)
            for channel, value in enumerate((lx, ly, lz, 0.0, t[0], t[1], t[2])):
                channels[channel].append(value)
        bones[bone] = channels
    extras = [list(map(float, e)) for e in doc.get("extras", [])]
    if not extras and base is not None:
        extras = [list(e) for e in base.extras]
        records.update({key: value for key, value in base.records.items() if key[0] == "extra"})
    if any(len(e) != frames for e in extras):
        raise bnk.BankError("an extra channel does not have %d frames" % frames)
    return bnk.Clip(frames, bnk.LAYOUT_QLOG, mask, bones, extras, 2.0, records)


# ---------------------------------------------------------------- command line

def _option(args, name, default=None):
    if name in args:
        i = args.index(name)
        return args[i + 1]
    return default


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    command, bank_path, rig_path = argv[0], Path(argv[1]), Path(argv[2])
    rig_data = rig_path.read_bytes()
    rig = bnk.read_rig(rig_data)
    if command == "info":
        bank = bnk.read_bank(bank_path.read_bytes())
        for action in bank.actions:
            for leaf in action.leaves:
                info = bnk.owner_info(bank.data, leaf.owner)
                print("%-48s leaf %d frames %5d layout %08X mask %08X extra %d events %d" % (
                    action.name, leaf.leaf_id, info.frames, info.layout, info.mask, info.extra, info.events))
        return 0
    if command == "export":
        action, out = argv[3], Path(argv[4])
        bank = bnk.read_bank(bank_path.read_bytes())
        leaves = bank.action(action).leaves
        leaf_id = _option(argv, "--leaf")
        leaf = [l for l in leaves if leaf_id is None or l.leaf_id == int(leaf_id)][0]
        clip = bnk.decode_owner(bank, leaf.owner, rig)
        ue = "--ue" in argv
        factor = 1.0 if "--no-rig-scale" in argv or not ue else rig_scale(rig_data)
        doc = clip_to_document(clip, rig, ue=ue, unit_scale=float(_option(argv, "--unit-scale", 100.0)), rig_factor=factor)
        out.write_text(json.dumps(doc), encoding="utf-8")
        print("wrote %s: %d frames, %d bones, %d extra channels" % (out, clip.frames, len(clip.bones), len(clip.extras)))
        return 0
    if command == "build":
        doc_path, out, name = Path(argv[3]), Path(argv[4]), _option(argv, "--action")
        if not name:
            raise SystemExit("--action NAME is required")
        mask = _option(argv, "--mask")
        replace = "--replace" in argv
        leaf_id = int(_option(argv, "--leaf")) if _option(argv, "--leaf") else None
        base = bank_path.read_bytes()
        edited = None
        if replace:
            bank = bnk.read_bank(base)
            leaves = [l for l in bank.action(name).leaves if leaf_id is None or l.leaf_id == leaf_id]
            if len(leaves) != 1:
                raise SystemExit("action %r has %d matching leaves; pass --leaf N" % (name, len(leaves)))
            leaf_id = leaves[0].leaf_id
            edited = bnk.decode_owner(bank, leaves[0].owner, rig)
        doc = json.loads(doc_path.read_text(encoding="utf-8"))
        clip = document_to_clip(doc, rig, rig_data, int(mask, 16) if mask else None, base=edited)
        mode = "raw" if "--raw" in argv else "quantized"
        data = bnk.replace_leaf(base, name, clip, rig, mode, leaf_id) if replace else bnk.add_action(base, name, clip, rig, mode)
        check = bnk.read_bank(data)
        leaf = [l for l in check.action(name).leaves if not replace or l.leaf_id == leaf_id][-1]
        back = bnk.decode_owner(check, leaf.owner, rig)
        out.write_bytes(data)
        print("wrote %s: %s %r, %d frames, %d bones (%d from the document), %d extra channels, %d bytes (was %d)" % (
            out, "replaced" if replace else "added", name, back.frames, len(back.bones), len(doc["bones"]), len(back.extras), len(data), len(base)))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
