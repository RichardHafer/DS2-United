"""Export a whole cutscene as glTF 2.0: every actor's skeleton and clips, and the camera.

    python -I tools/export_scene.py <scene stem, e.g. igc_03_03> <out dir>

Writes <stem>.glb and <stem>.ds2scene.json.

- Every bank of the scene (igc_03_03_player.bnk, igc_03_03_npcfunitologist.bnk, ...)
  is an actor. Its rig is the RCB whose name hashes to the bank's character hash.
- Each actor is a skin on its rig's bones, with a proxy mesh: one thin spike from
  every bone to each child, skinned to the parent, so UE5 and Blender import a
  skeletal mesh with the real skeleton.
- One glTF animation per scene part (010, 020, ...), holding every actor's clip of
  that part, so they play together. All actors share the scene's origin, as the
  game aligns them to the sequence's reference.
- The camera is a glTF camera under Isaac's m_camera bone, turned to look along the
  bone's +Z like the game's BoneBoundCamera. Its FOV per frame (the clip's extra
  channel, horizontal degrees) is in the animation's extras and in the sidecar.
- A second camera, ds2_camera_world, carries the same path baked into world space as
  its own animation: a free camera to edit in UE5 or Blender without the bone.
- Axes are the game's (right-handed, Y up, as glTF). Translations are scaled by the
  rig's base scale (player.rcb 1.818) into metres.

The sidecar (ds2scene.v1) keeps what the way back needs: every bone's hash, parent
and native bind pose, the base scales and which clip each part came from.
tools/blender/ds2_scene.py imports the .glb and exports an edited action as ds2anim.v1.
"""
import json
import math
import re
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import ds2_anim_json as aj  # noqa: E402
import ds2_bnk as bnk  # noqa: E402
import ds2_bone_names as bone_names  # noqa: E402
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402

FPS = 30.0
GLOBAL_RIG_ARCHIVE = r"ds2dat10\global_assets\global_assets.str"


# ---------------------------------------------------------------- the scene's files

def catalog_rows(stem):
    banks, rigs = {}, {}
    pattern = re.compile(r"^%s_(.+)\.bnk$" % re.escape(stem.lower()))
    with (ROOT / "analysis" / "catalog" / "assets.jsonl").open(encoding="utf-8") as f:
        for line in f:
            if '"BNK"' not in line and '"RCB"' not in line:
                continue
            row = json.loads(line)
            name = row["name"].lower()
            if row["type"] == "BNK" and pattern.match(name):
                banks.setdefault(name, row["archive"])
            elif row["type"] == "RCB":
                rigs.setdefault(bnk.sdbm(name[:-4]), []).append((name, row["archive"], row["path"]))
    return banks, rigs


def extract(wanted, cache):
    """name -> bytes for (name, archive) pairs, pulled out of their streams once."""
    cache.mkdir(parents=True, exist_ok=True)
    out, by_archive = {}, {}
    for name, archive in wanted:
        path = cache / name
        if path.exists():
            out[name] = path.read_bytes()
        else:
            by_archive.setdefault(archive, set()).add(name)
    if by_archive:
        sys.path.insert(0, str(MODDING_TOOLS))
        import ds2str
        from ds2_assets import assets
        for archive, left in by_archive.items():
            stream = ds2str.StreamSet(str(WORK / archive))
            for meta, chunks in assets(stream):
                name = (meta.get("name") or "").lower()
                if name in left:
                    data = stream.asset_payload(chunks, meta["size"])
                    (cache / name).write_bytes(data)
                    out[name] = data
                    left.discard(name)
            if left:
                raise SystemExit("not found in %s: %s" % (archive, ", ".join(sorted(left))))
    return out


def scene_sequences(stem):
    """Every sequence of the scene: its Isaac/actor clip name (lowered) -> its level matrix
    (16 floats, rows right, up, forward, position, in the game's space)."""
    rows = []
    with (ROOT / "analysis" / "catalog" / "entities.jsonl").open(encoding="utf-8") as f:
        for line in f:
            if '"CMD_SetActionAnimationString"' not in line or stem not in line.lower():
                continue
            record = json.loads(line)
            for field in record["images"][0]["fields"]:
                if field["name"] == "CMD_SetActionAnimationString" and str(field.get("string_candidate", "")).lower().startswith(stem + "_"):
                    rows.append((field["string_candidate"].lower(), record))
    out, payloads = {}, {}
    if not rows:
        return out
    sys.path.insert(0, str(MODDING_TOOLS))
    import ds2str
    from ds2_assets import assets
    for anim, record in rows:
        key = (record["archive"], record["asset"])
        if key not in payloads:
            stream = ds2str.StreamSet(str(WORK / record["archive"]))
            payloads[key] = next((stream.asset_payload(c, m["size"]) for m, c in assets(stream) if m["path"] == record["asset"]), None)
        data, offset = payloads[key], record.get("transform_offset", 0)
        if data and offset and offset + 64 <= len(data):
            out.setdefault(anim, list(struct.unpack_from("<16f", data, offset)))
    return out


def clip_centre(clip, rig, scale):
    """Where the clip's bones are on its first frame, on average (glTF units, its own space)."""
    bind = aj.local_bind(rig)
    world = []
    for bone, parent in enumerate(rig.parents):
        t, q, _s = bnk.pose(clip, bone, 0) if bone in clip.bones else (bind[bone][0], bind[bone][1], None)
        local = mat_from_tq([v * scale for v in t], q)
        world.append(local if parent < 0 else mul(world[parent], local))
    points = [[m[i][3] for i in range(3)] for m in world]
    return [sum(p[i] for p in points) / len(points) for i in range(3)]


def pick_rig(candidates, stem):
    """The scene's own export of a rig before any other copy, the global player rig first."""
    for name, archive, path in candidates:
        if archive.lower() == GLOBAL_RIG_ARCHIVE:
            return name, archive
    for name, archive, path in candidates:
        if ("\\%s\\" % stem.lower()) in path.lower():
            return name, archive
    return candidates[0][0], candidates[0][1]


# ---------------------------------------------------------------- math

def mat_from_tq(t, q):
    w, x, y, z = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), t[0]],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), t[1]],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), t[2]],
            [0.0, 0.0, 0.0, 1.0]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def inverse_rigid(m):
    r = [[m[j][i] for j in range(3)] for i in range(3)]
    t = [-sum(r[i][k] * m[k][3] for k in range(3)) for i in range(3)]
    return [r[0] + [t[0]], r[1] + [t[1]], r[2] + [t[2]], [0.0, 0.0, 0.0, 1.0]]


def column_major(m):
    return [m[r][c] for c in range(4) for r in range(4)]


# ---------------------------------------------------------------- glTF

class Gltf:
    def __init__(self):
        self.doc = {"asset": {"version": "2.0", "generator": "DS2Coop tools/export_scene.py"}, "scene": 0, "scenes": [{"nodes": []}],
                    "nodes": [], "meshes": [], "skins": [], "animations": [], "accessors": [], "bufferViews": [], "buffers": [],
                    "cameras": [], "materials": [{"name": "ds2_proxy", "pbrMetallicRoughness": {"baseColorFactor": [0.8, 0.75, 0.6, 1.0]}}]}
        self.blob = bytearray()

    def view(self, data, target=None):
        while len(self.blob) % 4:
            self.blob.append(0)
        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        self.blob += data
        self.doc["bufferViews"].append(view)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, kind, component=5126, target=None, minmax=False):
        width = {"SCALAR": 1, "VEC3": 3, "VEC4": 4, "MAT4": 16}[kind]
        fmt = {5126: "f", 5123: "H", 5125: "I"}[component]
        flat = [v for row in values for v in (row if width > 1 else [row])]
        accessor = {"bufferView": self.view(struct.pack("<%d%s" % (len(flat), fmt), *flat), target), "componentType": component,
                    "count": len(values), "type": kind}
        if minmax:
            rows = [row if width > 1 else [row] for row in values]
            accessor["min"] = [min(r[i] for r in rows) for i in range(width)]
            accessor["max"] = [max(r[i] for r in rows) for i in range(width)]
        self.doc["accessors"].append(accessor)
        return len(self.doc["accessors"]) - 1

    def node(self, **fields):
        self.doc["nodes"].append(fields)
        return len(self.doc["nodes"]) - 1

    def write(self, path):
        self.doc["buffers"] = [{"byteLength": len(self.blob)}]
        for key in ("meshes", "skins", "animations", "cameras"):
            if not self.doc[key]:
                del self.doc[key]
        text = json.dumps(self.doc, separators=(",", ":")).encode("utf-8")
        text += b" " * ((-len(text)) % 4)
        blob = bytes(self.blob) + bytes((-len(self.blob)) % 4)
        out = struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(text) + 8 + len(blob))
        out += struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(blob), 0x004E4942) + blob
        Path(path).write_bytes(out)


def proxy_mesh(gltf, world, parents, joint_of, longest=2.5):
    """A thin spike from every bone to each of its children, skinned to the bone. Links
    longer than `longest` metres (a vehicle's parts hanging off its root) are left out."""
    positions, normals, joints, weights = [], [], [], []
    for child, parent in enumerate(parents):
        if parent < 0:
            continue
        a = [world[parent][i][3] for i in range(3)]
        b = [world[child][i][3] for i in range(3)]
        d = [b[i] - a[i] for i in range(3)]
        length = math.sqrt(sum(v * v for v in d))
        if length < 1e-5 or length > longest:
            continue
        axis = [v / length for v in d]
        helper = [0.0, 1.0, 0.0] if abs(axis[1]) < 0.9 else [1.0, 0.0, 0.0]
        u = [axis[1] * helper[2] - axis[2] * helper[1], axis[2] * helper[0] - axis[0] * helper[2], axis[0] * helper[1] - axis[1] * helper[0]]
        n = math.sqrt(sum(v * v for v in u))
        u = [v / n for v in u]
        v = [axis[1] * u[2] - axis[2] * u[1], axis[2] * u[0] - axis[0] * u[2], axis[0] * u[1] - axis[1] * u[0]]
        r = min(0.025, length * 0.15)
        mid = [a[i] + d[i] * 0.2 for i in range(3)]
        ring = [[mid[i] + r * (math.cos(k * math.pi / 2) * u[i] + math.sin(k * math.pi / 2) * v[i]) for i in range(3)] for k in range(4)]
        for k in range(4):
            for tip in (a, b):
                tri = [tip, ring[k], ring[(k + 1) % 4]] if tip is b else [tip, ring[(k + 1) % 4], ring[k]]
                e1 = [tri[1][i] - tri[0][i] for i in range(3)]
                e2 = [tri[2][i] - tri[0][i] for i in range(3)]
                nn = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
                nl = math.sqrt(sum(x * x for x in nn)) or 1.0
                for p in tri:
                    positions.append(p)
                    normals.append([x / nl for x in nn])
                    joints.append([joint_of[parent], 0, 0, 0])
                    weights.append([1.0, 0.0, 0.0, 0.0])
    if not positions:
        return None
    primitive = {"attributes": {"POSITION": gltf.accessor(positions, "VEC3", target=34962, minmax=True),
                                "NORMAL": gltf.accessor(normals, "VEC3", target=34962),
                                "JOINTS_0": gltf.accessor(joints, "VEC4", 5123, target=34962),
                                "WEIGHTS_0": gltf.accessor(weights, "VEC4", target=34962)},
                 "material": 0}
    gltf.doc["meshes"].append({"name": "proxy", "primitives": [primitive]})
    return len(gltf.doc["meshes"]) - 1


def part_of(action, prefix):
    return action[len(prefix):] if action.lower().startswith(prefix.lower()) else action


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    stem, out = sys.argv[1].lower(), Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    banks, rigs = catalog_rows(stem)
    if not banks:
        raise SystemExit("no banks named %s_*.bnk in the catalog" % stem)
    cache = ROOT / "variants" / "build" / "scenes" / "retail"
    bank_files = extract(sorted(banks.items()), cache)
    actors = []
    for bank_name in sorted(bank_files, key=lambda n: (not n.startswith(stem + "_player"), n)):
        bank = bnk.read_bank(bank_files[bank_name])
        candidates = rigs.get(bank.skeleton_hash)
        if not candidates:
            print("skipped %s: no rig named for character %08X" % (bank_name, bank.skeleton_hash))
            continue
        rig_name, rig_archive = pick_rig(candidates, stem)
        rig_data = extract([(rig_name, rig_archive)], cache)[rig_name]
        actors.append(dict(name=bank_name[len(stem) + 1:-4], bank_name=bank_name, bank=bank, rig_name=rig_name, rig_data=rig_data,
                           rig=bnk.read_rig(rig_data)))
    # Clips, and where each actor stands. Isaac's, the props' and the gunship's clips are
    # in level space; an NPC's clip is local to its own sequence, which teleports it there
    # (TELEPORTALL). A clip whose bones start next to its sequence's position is taken as
    # level space, one whose bones start next to the origin as local to that sequence.
    sequences = scene_sequences(stem)
    for actor in actors:
        rig = actor["rig"]
        actor["scale"] = scale = aj.rig_scale(actor["rig_data"])
        prefix = "%s_%s_" % (stem, actor["name"])
        decoded = []
        for action in actor["bank"].actions:
            clips = []
            for leaf in action.leaves:
                try:
                    clips.append((leaf, bnk.decode_owner(actor["bank"], leaf.owner, rig)))
                except (bnk.BankError, KeyError) as exc:
                    print("  %s %s: not decoded (%s)" % (actor["name"], action.name, exc))
            if not clips:
                continue
            main_leaf = max(clips, key=lambda c: c[1].frames)[0]   # the action's clip; its other leaves get #id
            for leaf, clip in clips:
                part = part_of(action.name, prefix) + ("" if leaf is main_leaf else "#%d" % leaf.leaf_id)
                decoded.append((part, action.name, leaf.leaf_id, clip))
        actor["decoded"] = decoded
        actor["node"], actor["placement"] = None, "no clip"
        placed = [(sequences[a.lower()], c) for p, a, l, c in sorted(decoded, key=lambda d: d[0]) if a.lower() in sequences and "#" not in p]
        if placed:
            sequence, clip = placed[0]
            centre = clip_centre(clip, rig, scale)
            far = math.sqrt(sum((centre[i] - sequence[12 + i]) ** 2 for i in range(3)))
            if far <= 15.0:
                actor["placement"], world_centre = "level space", centre
            elif math.sqrt(sum(v * v for v in centre)) <= 15.0:
                actor["node"] = sequence[:3] + [0.0] + sequence[4:7] + [0.0] + sequence[8:11] + [0.0] + sequence[12:15] + [1.0]
                actor["placement"] = "local to its sequence"
                m = [[actor["node"][c * 4 + r] for c in range(4)] for r in range(4)]
                world_centre = [sum(m[i][k] * (centre + [1.0])[k] for k in range(4)) for i in range(3)]
            else:
                actor["placement"], world_centre = "neither (left at the origin)", centre
            actor["world_centre"] = world_centre
        elif decoded:
            actor["world_centre"] = clip_centre(decoded[0][3], rig, scale)
            actor["placement"] = "no sequence found (left at the origin)"
    anchor = next((a for a in actors if a["name"] == "player" and a.get("world_centre")), None) or next((a for a in actors if a.get("world_centre")), None)
    centre = anchor["world_centre"] if anchor else [0.0, 1.0, 0.0]
    offset = [-centre[0], -(centre[1] - 1.0), -centre[2]]   # the scene around the origin, its floor near 0
    gltf = Gltf()
    root = gltf.node(name=stem, children=[], translation=offset)
    gltf.doc["scenes"][0]["nodes"].append(root)
    sidecar = {"schema": "ds2scene.v1", "scene": stem, "fps": FPS, "coordinates": "glTF axes = game axes; glTF translation = native * base_scale",
               "scene_offset": offset, "actors": [], "camera": None}
    animations = {}   # part -> list of (actor index, clip)
    camera_chain = None
    for index, actor in enumerate(actors):
        rig, rig_data, scale = actor["rig"], actor["rig_data"], actor["scale"]
        names = bone_names.names_for(rig.bone_hashes)
        bind = aj.local_bind(rig)
        world = []
        for bone, parent in enumerate(rig.parents):
            t = [v * scale for v in bind[bone][0]]
            local = mat_from_tq(t, bind[bone][1])
            world.append(local if parent < 0 else mul(world[parent], local))
        actor_node = gltf.node(name=actor["name"], children=[], **({"matrix": actor["node"]} if actor["node"] else {}))
        gltf.doc["nodes"][root]["children"].append(actor_node)
        # the actor node in scene space (column-vector rows), for the way back
        node_cv = [[actor["node"][c * 4 + r] for c in range(4)] for r in range(4)] if actor["node"] else \
            [[float(i == j) for j in range(4)] for i in range(4)]
        scene_cv = mul([[1.0, 0.0, 0.0, offset[0]], [0.0, 1.0, 0.0, offset[1]], [0.0, 0.0, 1.0, offset[2]], [0.0, 0.0, 0.0, 1.0]], node_cv)
        joint_nodes = []
        for bone in range(len(rig.parents)):
            t = [v * scale for v in bind[bone][0]]
            w, x, y, z = bind[bone][1]
            joint_nodes.append(gltf.node(name=bone_names.bone_name(rig.bone_hashes[bone], names), translation=t, rotation=[x, y, z, w]))
        for bone, parent in enumerate(rig.parents):
            target = gltf.doc["nodes"][joint_nodes[parent]] if parent >= 0 else gltf.doc["nodes"][actor_node]
            target.setdefault("children", []).append(joint_nodes[bone])
        joint_of = {bone: bone for bone in range(len(rig.parents))}
        ibm = gltf.accessor([column_major(inverse_rigid(m)) for m in world], "MAT4")
        gltf.doc["skins"].append({"name": actor["name"], "joints": joint_nodes, "inverseBindMatrices": ibm, "skeleton": joint_nodes[rig.parents.index(-1)]})
        mesh = proxy_mesh(gltf, world, rig.parents, joint_of)
        if mesh is not None:
            gltf.doc["nodes"][actor_node]["children"].append(gltf.node(name=actor["name"] + "_proxy", mesh=mesh, skin=len(gltf.doc["skins"]) - 1))
        parts = {}
        for part, action_name, leaf_id, clip in actor["decoded"]:
            parts[part] = {"action": action_name, "leaf": leaf_id, "frames": clip.frames, "layout": "%08X" % clip.layout,
                           "mask": "%08X" % clip.mask, "extras": len(clip.extras), "bones": sorted(clip.bones)}
            animations.setdefault(part, []).append((index, clip, joint_nodes, scale))
        sidecar["actors"].append({"name": actor["name"], "bank": actor["bank_name"], "rig": actor["rig_name"],
                                  "character": "%08X" % actor["bank"].skeleton_hash, "base_scale": scale,
                                  "placement": actor["placement"], "node_matrix": scene_cv,
                                  "bones": [{"index": b, "hash": "0x%08X" % rig.bone_hashes[b], "name": bone_names.bone_name(rig.bone_hashes[b], names),
                                             "parent": rig.parents[b], "bind_t": list(bind[b][0]), "bind_q": list(bind[b][1])}
                                            for b in range(len(rig.parents))],
                                  "parts": parts})
        print("%-24s rig %-24s %3d bones (%d named), scale %.3f, %d clips, %s" % (actor["name"], actor["rig_name"], len(rig.parents),
                                                                                 len(names), scale, len(parts), actor["placement"]))
        if actor["name"] == "player" and aj.CAMERA_BONE_HASH in rig.bone_hashes:
            camera_bone = rig.bone_hashes.index(aj.CAMERA_BONE_HASH)
            gltf.doc["cameras"].append({"name": "ds2_camera", "type": "perspective",
                                        "perspective": {"yfov": 0.6, "znear": 0.05, "aspectRatio": 16.0 / 9.0}})
            camera_node = gltf.node(name="ds2_camera", camera=0, rotation=[0.0, 1.0, 0.0, 0.0])
            gltf.doc["nodes"][joint_nodes[camera_bone]].setdefault("children", []).append(camera_node)
            world_camera = gltf.node(name="ds2_camera_world", camera=0)
            gltf.doc["nodes"][root]["children"].append(world_camera)
            sidecar["camera"] = {"actor": actor["name"], "bone": "m_camera", "node": "ds2_camera", "world_node": "ds2_camera_world", "fov_degrees": {},
                                 "look": "the camera node looks along the bone's +Z (glTF cameras look along -Z: turned 180 degrees about Y)"}
            camera_chain = (rig, scale, camera_bone, world_camera, node_cv)
    for part in sorted(animations):
        channels, samplers, extras = [], [], {}
        for actor_index, clip, joint_nodes, scale in animations[part]:
            times = gltf.accessor([f / FPS for f in range(clip.frames)], "SCALAR", minmax=True)
            for bone in sorted(clip.bones):
                translations, rotations, previous = [], [], None
                for frame in range(clip.frames):
                    t, q, _s = bnk.pose(clip, bone, frame)
                    if previous is not None and sum(a * b for a, b in zip(q, previous)) < 0:
                        q = tuple(-v for v in q)
                    previous = q
                    translations.append([v * scale for v in t])
                    rotations.append([q[1], q[2], q[3], q[0]])
                for path, values, kind in (("translation", translations, "VEC3"), ("rotation", rotations, "VEC4")):
                    samplers.append({"input": times, "output": gltf.accessor(values, kind), "interpolation": "LINEAR"})
                    channels.append({"sampler": len(samplers) - 1, "target": {"node": joint_nodes[bone], "path": path}})
            actor = sidecar["actors"][actor_index]
            if actor["name"] == "player" and clip.extras:
                fov = [round(v, 4) for v in clip.extras[0]]
                extras["ds2_fov_degrees"] = fov
                sidecar["camera"]["fov_degrees"][part] = fov
            if actor["name"] == "player" and camera_chain and camera_chain[2] in clip.bones:
                c_rig, c_scale, c_bone, c_node, c_actor = camera_chain
                bind = aj.local_bind(c_rig)
                chain, b = [], c_bone
                while b >= 0:
                    chain.append(b)
                    b = c_rig.parents[b]
                turn = mat_from_tq([0.0, 0.0, 0.0], (0.0, 0.0, 1.0, 0.0))
                translations, rotations, previous = [], [], None
                for frame in range(clip.frames):
                    m = c_actor
                    for b in reversed(chain):
                        t, q, _s = bnk.pose(clip, b, frame) if b in clip.bones else (bind[b][0], bind[b][1], None)
                        m = mul(m, mat_from_tq([v * c_scale for v in t], q))
                    m = mul(m, turn)
                    q = aj._quaternion(m)
                    if previous is not None and sum(a * b for a, b in zip(q, previous)) < 0:
                        q = [-v for v in q]
                    previous = q
                    translations.append([m[i][3] for i in range(3)])
                    rotations.append([q[1], q[2], q[3], q[0]])
                for path, values, kind in (("translation", translations, "VEC3"), ("rotation", rotations, "VEC4")):
                    samplers.append({"input": times, "output": gltf.accessor(values, kind), "interpolation": "LINEAR"})
                    channels.append({"sampler": len(samplers) - 1, "target": {"node": c_node, "path": path}})
        gltf.doc["animations"].append({"name": part, "channels": channels, "samplers": samplers, "extras": extras})
        print("part %-12s %d actors, %d channels" % (part, len(animations[part]), len(channels)))
    glb = out / (stem + ".glb")
    gltf.write(glb)
    (out / (stem + ".ds2scene.json")).write_text(json.dumps(sidecar, indent=1) + "\n", encoding="utf-8", newline="\n")
    print("wrote %s (%d bytes) and %s.ds2scene.json" % (glb, glb.stat().st_size, stem))
    return 0


if __name__ == "__main__":
    sys.exit(main())
