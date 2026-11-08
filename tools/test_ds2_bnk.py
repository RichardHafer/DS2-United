"""Round-trip test for tools/ds2_bnk.py on extracted player cutscene banks.

    python -I tools/test_ds2_bnk.py <bank dir with player.rcb> <scratch dir> [folder with ds2decode]

For every clip of layout 0x80000004:
  * our reader matches the external DS2Import decoder sample for sample (when given)
  * add_action in raw mode decodes back to the float32 samples bit for bit, the
    old actions still decode unchanged, and the external decoder accepts the new bank
  * quantized mode stays within range/65000 per channel
  * replace_leaf points the old action at the new owner
"""
import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ds2_bnk as bnk  # noqa: E402

BANKS, SCRATCH = Path(sys.argv[1]), Path(sys.argv[2])
EXTERNAL = Path(sys.argv[3]) if len(sys.argv) > 3 else None
core = None
if EXTERNAL:
    sys.path.insert(0, str(EXTERNAL))
    from ds2decode import ds2_bnk_core as core  # noqa: E402
SCRATCH.mkdir(parents=True, exist_ok=True)

rig_data = (BANKS / "player.rcb").read_bytes()
rig = bnk.read_rig(rig_data)
ext_rig = core.read_rcb(BANKS / "player.rcb") if core else None
failures, checked, new_actions, quant_worst = [], 0, 0, [0.0, 0.0]


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


def same(a, b):
    return all(x == y for x, y in zip(a, b)) and len(a) == len(b)


def external_channels(path, action_name):
    bank = core.read_bnk(path)
    skeleton = ext_rig.skeleton_by_hash(bank.skeleton_hash)
    out = []
    for leaf in bank.leaves:
        if leaf.action_name == action_name:
            clip = core.decode_clip_for_skeleton(bank, ext_rig, leaf, skeleton)
            out.append(clip)
    return out


def flat(clip):
    rows = [ch for bone in sorted(clip.bones) for ch in clip.bones[bone]]
    return rows + list(clip.extras)


for path in sorted(BANKS.glob("*.bnk")):
    data = path.read_bytes()
    try:
        bank = bnk.read_bank(data)
    except bnk.BankError as exc:
        failures.append("%s: read %s" % (path.name, exc))
        continue
    for action in bank.actions:
        for leaf in action.leaves:
            info = bnk.owner_info(data, leaf.owner)
            if info.layout != bnk.LAYOUT_QLOG:
                continue
            try:
                clip = bnk.decode_owner(bank, leaf.owner, rig)
            except bnk.BankError as exc:
                failures.append("%s %s: decode %s" % (path.name, action.name, exc))
                continue
            checked += 1
            ours = flat(clip)
            external_ok = core is not None
            if core:
                try:
                    theirs = [c for c in external_channels(path, action.name) if c.leaf.owner_offset == leaf.owner]
                except Exception as exc:  # their guards refuse a few retail banks
                    theirs, external_ok = [], False
                    print("  external decoder refuses retail %s: %s" % (path.name, str(exc)[:90]))
                if theirs:
                    t = theirs[0]
                    t_rows = [ch for bone in sorted(t.bone_channels) for ch in t.bone_channels[bone]] + list(t.extra_channels)
                    if len(t_rows) != len(ours) or any(not same(a, b) for a, b in zip(ours, t_rows)):
                        failures.append("%s %s: differs from external decoder" % (path.name, action.name))
            if clip.frames > bnk.MAX_COUNT:
                continue
            name = (action.name + "_rt%d" % leaf.leaf_id)[:60]
            # unchanged channels keep their records: same samples, same stream bytes
            verbatim = bnk.read_bank(bnk.add_action(data, name, clip, rig, mode="quantized"))
            v_owner = verbatim.action(name).leaves[0].owner
            v_info, o_info = bnk.owner_info(verbatim.data, v_owner), bnk.owner_info(data, leaf.owner)
            if any(not same(a, b) for a, b in zip(ours, flat(bnk.decode_owner(verbatim, v_owner, rig)))):
                failures.append("%s %s: verbatim records changed samples" % (path.name, action.name))
            v_stream = bnk.decode_owner(verbatim, v_owner, rig)
            if b"".join(v_stream.records[k] for k in sorted(v_stream.records, key=str)) != b"".join(clip.records[k] for k in sorted(clip.records, key=str)):
                failures.append("%s %s: verbatim records differ in bytes" % (path.name, action.name))
            # the encoder itself: the same samples without their records
            clip = bnk.Clip(clip.frames, clip.layout, clip.mask, clip.bones, clip.extras, clip.step)
            out = bnk.add_action(data, name, clip, rig, mode="raw")
            new_bank = bnk.read_bank(out)
            back = bnk.decode_owner(new_bank, new_bank.action(name).leaves[0].owner, rig)
            if any(not same([f32(v) for v in a], b) for a, b in zip(ours, flat(back))):
                failures.append("%s %s: raw round trip changed samples" % (path.name, action.name))
            for old_action in bank.actions:
                for old_leaf in old_action.leaves:
                    if bnk.owner_info(data, old_leaf.owner).layout != bnk.LAYOUT_QLOG:
                        continue
                    a = flat(bnk.decode_owner(bank, old_leaf.owner, rig))
                    b = flat(bnk.decode_owner(new_bank, new_bank.action(old_action.name).leaves[[l.leaf_id for l in old_action.leaves].index(old_leaf.leaf_id)].owner, rig))
                    if any(not same(x, y) for x, y in zip(a, b)):
                        failures.append("%s %s: old action changed" % (path.name, old_action.name))
            if external_ok:
                target = SCRATCH / ("rt_" + path.name)
                target.write_bytes(out)
                try:
                    clips = external_channels(target, name)
                    rows = [ch for bone in sorted(clips[0].bone_channels) for ch in clips[0].bone_channels[bone]] + list(clips[0].extra_channels)
                    if any(not same([f32(v) for v in a], b) for a, b in zip(ours, rows)):
                        failures.append("%s %s: external decoder reads other samples" % (path.name, name))
                except Exception as exc:
                    failures.append("%s %s: external decoder refuses the new bank: %s" % (path.name, name, str(exc)[:120]))
            new_actions += 1
            # quantized
            qout = bnk.add_action(data, name, clip, rig, mode="quantized")
            qbank = bnk.read_bank(qout)
            qback = flat(bnk.decode_owner(qbank, qbank.action(name).leaves[0].owner, rig))
            for index, (a, b) in enumerate(zip(ours, qback)):
                span = max(a) - min(a)
                error = max(abs(x - y) for x, y in zip(a, b))
                limit = span / 65000.0 + 1e-5 * max(1.0, max(abs(v) for v in a))
                if error > limit:
                    failures.append("%s %s: quantized channel %d error %.3g > %.3g" % (path.name, action.name, index, error, limit))
                kind = 0 if index < len(clip.bones) * 7 and index % 7 < 3 else 1
                quant_worst[kind] = max(quant_worst[kind], error)
            # replace the original leaf with the same clip
            rout = bnk.replace_leaf(data, action.name, clip, rig, mode="raw", leaf_id=leaf.leaf_id)
            rbank = bnk.read_bank(rout)
            rleaf = [l for l in rbank.action(action.name).leaves if l.leaf_id == leaf.leaf_id][0]
            if rleaf.owner == leaf.owner or any(not same([f32(v) for v in a], b) for a, b in zip(ours, flat(bnk.decode_owner(rbank, rleaf.owner, rig)))):
                failures.append("%s %s: replace_leaf did not repoint" % (path.name, action.name))
    print("%-30s ok" % path.name if not any(f.startswith(path.name) for f in failures) else "%-30s FAILED" % path.name)

try:
    bnk.encode_channel([0.5] * (bnk.MAX_COUNT + 1))
    failures.append("a channel over %d samples was accepted" % bnk.MAX_COUNT)
except bnk.BankError:
    pass

print("clips checked %d, new actions written %d, worst quantized error rotation %.3g translation/extra %.3g" % (
    checked, new_actions, quant_worst[0], quant_worst[1]))
for failure in failures[:40]:
    print("FAIL", failure)
print("PASS" if not failures else "FAILED (%d)" % len(failures))
sys.exit(1 if failures else 0)
