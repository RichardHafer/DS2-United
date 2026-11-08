"""Checks for tools/export_scene.py output and the UE5 way back.

    python -I tools/test_scene_export.py <scene .glb> [actor] [part]

- The .glb: header, chunks, every accessor inside its buffer view, every
  animation sampler's input and output of equal length, the skins' joints.
- The axis map (tools/ds2_axis_calibration.py, used by tools/ue5/ds2_export_anim.py):
  two simulated importers, one mapping every bone's axes (a mirror, metres to
  centimetres), one converting only above the root, must both give the actor's
  clip back from the imported reference pose.
"""
import json
import math
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ds2_axis_calibration as axes  # noqa: E402
import ds2_bnk as bnk  # noqa: E402

GLB = Path(sys.argv[1])
ACTOR = sys.argv[2] if len(sys.argv) > 2 else "player"
PART = sys.argv[3] if len(sys.argv) > 3 else "010"
failures = []

data = GLB.read_bytes()
magic, version, length = struct.unpack_from("<III", data, 0)
json_length, json_type = struct.unpack_from("<II", data, 12)
doc = json.loads(data[20:20 + json_length])
bin_length, bin_type = struct.unpack_from("<II", data, 20 + json_length)
blob = data[28 + json_length:28 + json_length + bin_length]
if magic != 0x46546C67 or version != 2 or length != len(data) or json_type != 0x4E4F534A or bin_type != 0x004E4942:
    failures.append("glb header or chunks")
sizes = {5126: 4, 5123: 2, 5125: 4, 5121: 1}
widths = {"SCALAR": 1, "VEC3": 3, "VEC4": 4, "MAT4": 16}
for i, accessor in enumerate(doc["accessors"]):
    view = doc["bufferViews"][accessor["bufferView"]]
    need = accessor["count"] * widths[accessor["type"]] * sizes[accessor["componentType"]]
    if need > view["byteLength"] or view["byteOffset"] + view["byteLength"] > len(blob):
        failures.append("accessor %d outside its view" % i)
for animation in doc["animations"]:
    for sampler in animation["samplers"]:
        if doc["accessors"][sampler["input"]]["count"] != doc["accessors"][sampler["output"]]["count"]:
            failures.append("animation %s: sampler input and output differ" % animation["name"])
            break
for skin in doc["skins"]:
    if doc["accessors"][skin["inverseBindMatrices"]]["count"] != len(skin["joints"]):
        failures.append("skin %s: inverse bind matrices do not match its joints" % skin["name"])
print("glb: %d bytes, %d nodes, %d skins, %d animations, %d accessors" % (len(data), len(doc["nodes"]), len(doc["skins"]), len(doc["animations"]), len(doc["accessors"])))

sidecar = json.loads(GLB.with_suffix(".ds2scene.json").read_text(encoding="utf-8"))
actor = [a for a in sidecar["actors"] if a["name"] == ACTOR][0]
cache = TOOLS.parent / "variants" / "build" / "scenes" / "retail"
rig = bnk.read_rig((cache / actor["rig"]).read_bytes())
bank = bnk.read_bank((cache / actor["bank"]).read_bytes())
part = actor["parts"][PART]
leaf = [l for l in bank.action(part["action"]).leaves if l.leaf_id == part["leaf"]][0]
clip = bnk.decode_owner(bank, leaf.owner, rig)
scale = actor["base_scale"]


def gltf_local(bone, frame):
    if frame is None or bone["index"] not in clip.bones:
        w, x, y, z = bone["bind_q"]
        return [v * scale for v in bone["bind_t"]], [x, y, z, w]
    t, q, _s = bnk.pose(clip, bone["index"], frame)
    return [v * scale for v in t], [q[1], q[2], q[3], q[0]]


def simulate(kind, bone, frame):
    t, q = gltf_local(bone, frame)
    if kind == "every bone":
        p = [[1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, 1.0, 0.0]]   # x, z, y: a mirror
        return axes.from_gltf(p, 100.0, -1.0, t, q)
    if bone["parent"] < 0:   # above the root: Y up to Z up, turned 90 degrees about X
        c = axes.matrix([0.0, 0.0, 0.0], [math.sin(math.pi / 4), 0.0, 0.0, math.cos(math.pi / 4)])
        return axes.decompose(axes.mul(c, axes.matrix(t, q)))
    return t, q


for kind in ("every bone", "above the root"):
    pairs_t, pairs_q, root = [], [], None
    for bone in actor["bones"]:
        imported = simulate(kind, bone, None)
        gltf = gltf_local(bone, None)
        if bone["parent"] < 0:
            root = (gltf, imported)
            continue
        if sum(v * v for v in gltf[0]) > 1e-8:
            pairs_t.append((gltf[0], imported[0]))
        pairs_q.append((gltf[1], imported[1]))
    p, s, flip, error = axes.calibrate(pairs_t, pairs_q)
    root_a = axes.root_map(p, s, flip, root[0], root[1])
    worst_t = worst_q = 0.0
    for frame in range(0, clip.frames, max(1, clip.frames // 40)):
        for bone in actor["bones"]:
            if bone["index"] not in clip.bones:
                continue
            imported = simulate(kind, bone, frame)
            t, q = axes.root_to_gltf(root_a, *imported) if bone["parent"] < 0 and root_a is not None else axes.to_gltf(p, s, flip, *imported)
            native_t = [v / scale for v in t]
            want_t, want_q, _s = bnk.pose(clip, bone["index"], frame)
            worst_t = max(worst_t, max(abs(a - b) for a, b in zip(native_t, want_t)))
            worst_q = max(worst_q, 1.0 - abs(sum(a * b for a, b in zip([q[3], q[0], q[1], q[2]], want_q))))
    ok = worst_t < 1e-4 and worst_q < 1e-6
    print("importer mapping %-14s: scale %.1f mirror %s root %s, back to the clip: translation %.2e rotation %.2e  %s" % (
        kind, s, flip < 0, "own" if root_a is not None else "shared", worst_t, worst_q, "ok" if ok else "FAILED"))
    if not ok:
        failures.append("axis map " + kind)
print("PASS" if not failures else "FAILED: " + ", ".join(failures))
sys.exit(1 if failures else 0)
