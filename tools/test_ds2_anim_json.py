"""Round trip bank -> ds2anim.v1 JSON (native and UE coordinates) -> bank.

    python -I tools/test_ds2_anim_json.py <bank dir with player.rcb> [bank name] [action]
"""
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ds2_anim_json as aj  # noqa: E402
import ds2_bnk as bnk  # noqa: E402

BANKS = Path(sys.argv[1])
BANK = sys.argv[2] if len(sys.argv) > 2 else "igc_01_02_player.bnk"
ACTION = sys.argv[3] if len(sys.argv) > 3 else "igc_01_02_player_010"
rig_data = (BANKS / "player.rcb").read_bytes()
rig = bnk.read_rig(rig_data)
data = (BANKS / BANK).read_bytes()
bank = bnk.read_bank(data)
clip = bnk.decode_owner(bank, bank.action(ACTION).leaves[0].owner, rig)
failures = []


def compare(a, b, label):
    worst_q = worst_t = worst_e = 0.0
    for bone in a.bones:
        ca, cb = a.bones[bone], b.bones[bone]
        for frame in range(a.frames):
            qa = bnk.qlog_to_quaternion(ca[0][frame], ca[1][frame], ca[2][frame])
            qb = bnk.qlog_to_quaternion(cb[0][frame], cb[1][frame], cb[2][frame])
            worst_q = max(worst_q, 1.0 - abs(sum(x * y for x, y in zip(qa, qb))))
            worst_t = max(worst_t, max(abs(ca[k][frame] - cb[k][frame]) for k in (4, 5, 6)))
    for ea, eb in zip(a.extras, b.extras):
        worst_e = max(worst_e, max(abs(x - y) for x, y in zip(ea, eb)))
    ok = worst_q < 1e-9 and worst_t < 1e-5 and worst_e < 1e-4
    print("%-28s rotation 1-|dot| %.2e, translation %.2e, extras %.2e  %s" % (label, worst_q, worst_t, worst_e, "ok" if ok else "FAILED"))
    if not ok:
        failures.append(label)


for ue in (False, True):
    factor = aj.rig_scale(rig_data) if ue else 1.0
    doc = json.loads(json.dumps(aj.clip_to_document(clip, rig, ue=ue, rig_factor=factor)))
    back = aj.document_to_clip(doc, rig, rig_data)
    compare(clip, back, "document (%s)" % ("ue" if ue else "native"))
    built = bnk.add_action(data, ACTION + "_json", back, rig, mode="raw")
    nb = bnk.read_bank(built)
    compare(clip, bnk.decode_owner(nb, nb.action(ACTION + "_json").leaves[0].owner, rig), "bank from document (%s)" % ("ue" if ue else "native"))

# a 60 fps document (midpoints added) comes back at 30 fps on the original samples
doc = aj.clip_to_document(clip, rig)
fast = dict(doc, sample_rate=60.0, frames=doc["frames"] * 2 - 1, bones=[])
for entry in doc["bones"]:
    t, q = [], []
    for i in range(doc["frames"]):
        t.append(entry["translations"][i])
        q.append(entry["rotations"][i])
        if i + 1 < doc["frames"]:
            t.append([(a + b) / 2 for a, b in zip(entry["translations"][i], entry["translations"][i + 1])])
            q.append(aj._slerp(entry["rotations"][i], entry["rotations"][i + 1], 0.5))
    fast["bones"].append(dict(entry, translations=t, rotations=q))
fast["extras"] = [[v for i, x in enumerate(e) for v in ((x, (x + e[i + 1]) / 2) if i + 1 < len(e) else (x,))] for e in doc["extras"]]
compare(clip, aj.document_to_clip(fast, rig, rig_data), "60 fps document resampled")

# a camera edit: a document with only m_camera, moved 0.5 along x, replaces the
# scene clip; every other bone and the FOV channel stay as they were
doc = aj.clip_to_document(clip, rig)
camera_doc = dict(doc, bones=[dict(b, translations=[[t[0] + 0.5, t[1], t[2]] for t in b["translations"]])
                              for b in doc["bones"] if b["hash"] == "0x%08X" % aj.CAMERA_BONE_HASH], extras=[])
edited = aj.document_to_clip(camera_doc, rig, rig_data, base=clip)
leaf = bank.action(ACTION).leaves[0]
out = bnk.replace_leaf(data, ACTION, edited, rig, mode="quantized", leaf_id=leaf.leaf_id)
eb = bnk.read_bank(out)
after = bnk.decode_owner(eb, eb.action(ACTION).leaves[0].owner, rig)
camera = rig.bone_hashes.index(aj.CAMERA_BONE_HASH)
moved = max(abs(after.bones[camera][4][f] - clip.bones[camera][4][f] - 0.5) for f in range(clip.frames))
body = max(abs(a - b) for bone in clip.bones if bone != camera for ca, cb in zip(clip.bones[bone], after.bones[bone]) for a, b in zip(ca, cb))
fov = max(abs(a - b) for ea, eb_ in zip(clip.extras, after.extras) for a, b in zip(ea, eb_))
ok = moved < 1e-3 and body < 1e-3 and fov < 1e-3 and len(after.extras) == len(clip.extras)
print("camera-only edit: camera moved off by %.2e, body changed %.2e, FOV changed %.2e  %s" % (moved, body, fov, "ok" if ok else "FAILED"))
if not ok:
    failures.append("camera edit")

# a document with only two bones keeps the bind pose everywhere else
doc = aj.clip_to_document(clip, rig)
doc["bones"] = [b for b in doc["bones"] if b["hash"] in ("0x%08X" % aj.CAMERA_BONE_HASH, doc["bones"][1]["hash"])]
partial = aj.document_to_clip(doc, rig, rig_data)
bind = aj.local_bind(rig)
filled = [b for b in partial.bones if "0x%08X" % rig.bone_hashes[b] not in {x["hash"] for x in doc["bones"]}]
off = max(abs(partial.bones[b][4][0] - bind[b][0][0]) for b in filled)
print("partial document: %d bones from the bind pose, worst offset %.2e  %s" % (len(filled), off, "ok" if off < 1e-6 else "FAILED"))
if off >= 1e-6:
    failures.append("partial")
for name, expect in (("m_camera", aj.CAMERA_BONE_HASH), ("hash_39C5E697", aj.CAMERA_BONE_HASH), ("bone_004_39C5E697", aj.CAMERA_BONE_HASH)):
    if aj.bone_hash({"name": name}) != expect:
        failures.append("bone name " + name)
print("PASS" if not failures else "FAILED: " + ", ".join(failures))
sys.exit(1 if failures else 0)
