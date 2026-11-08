"""DS2 cutscenes in Blender: import a scene from tools/export_scene.py, export edits back.

In Blender (Scripting tab, or blender -b --python):

    import sys, importlib; sys.path.append(r"C:/path/to/DeadSpace2Randomizer/tools/blender")
    import ds2_scene; importlib.reload(ds2_scene)   # in every script run
    ds2_scene.partner_setup("igc_03_03")            # the scene and a partner, whatever is already in the file
    ds2_scene.import_scene("igc_03_03")             # or step by step: from variants/build/scenes, or a full .glb path
    ds2_scene.show_part("010")                      # every actor and the camera on that part
    ds2_scene.export_action("player", "010", r"C:/out/igc_03_03_player_010.json")
    ds2_scene.export_camera("010", r"C:/out/igc_03_03_player_010_camera.json")

- import_scene sets 30 fps (the game's clip rate), imports the .glb with Blender's own
  glTF importer and keys the camera's FOV per part from the sidecar
  (<stem>.ds2scene.json next to the .glb).
- export_action writes one actor's action as ds2anim.v1 in the game's own space. The
  importer turns bones to its liking; each bone's turn is read back from the rest pose
  against the sidecar's bind pose, so what comes out is the game's local pose again.
- export_camera writes only m_camera and the FOV, from wherever the camera object is
  (still on the bone or animated freely), for a camera edit:
  python -I tools/ds2_anim_json.py build <scene bank> player.rcb camera.json out.bnk --action <clip> --replace
  or put it in variants/anims/scenes/<bank>/<clip>.json.
"""
import json
import math
from pathlib import Path

import bpy
from mathutils import Matrix, Quaternion, Vector

# Blender's glTF importer: glTF Y up -> Blender Z up
GLTF_TO_BLENDER = Matrix(((1.0, 0.0, 0.0, 0.0), (0.0, 0.0, -1.0, 0.0), (0.0, 1.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)))
# the camera node under m_camera: turned 180 degrees about Y (glTF cameras look along -Z)
CAMERA_TURN = Matrix.Rotation(math.pi, 4, "Y")
FPS = 30
_scene_data = {}


def _sidecar(path=None):
    """The scene's sidecar. The .blend remembers its path (scene["ds2_sidecar"]), so a
    fresh script run, a reload of this module or a reopened file finds it again, and
    partners added earlier come back from their objects."""
    if path:
        _scene_data["data"] = json.loads(Path(path).read_text(encoding="utf-8"))
        bpy.context.scene["ds2_sidecar"] = str(path)
    if "data" not in _scene_data:
        remembered = bpy.context.scene.get("ds2_sidecar")
        if not remembered or not Path(remembered).is_file():
            remembered = _found_scene()
        if not remembered or not Path(remembered).is_file():
            raise RuntimeError("no scene loaded: run ds2_scene.import_scene(\"igc_03_03\") first")
        bpy.context.scene["ds2_sidecar"] = remembered
        _scene_data["data"] = json.loads(Path(remembered).read_text(encoding="utf-8"))
        for actor in _scene_data["data"]["actors"]:   # an older import kept no place: the actors were not moved since
            obj = bpy.data.objects.get(actor["name"])
            if obj and obj.type == "ARMATURE" and "ds2_import_matrix" not in obj:
                obj["ds2_import_matrix"] = [v for row in obj.matrix_world for v in row]
        for obj in bpy.data.objects:
            if obj.get("ds2_partner_part"):
                _register_partner(_scene_data["data"], obj.name, obj["ds2_partner_part"], obj["ds2_partner_action"])
    return _scene_data["data"]


def _found_scene():
    """The sidecar of a scene already in this file that an older version of this script
    imported: its root empty is named after it (igc_03_03, or igc_03_03.001 when imported twice)."""
    import re
    for obj in bpy.data.objects:
        if obj.type != "EMPTY":
            continue
        stem = re.sub(r"\.\d{3}$", "", obj.name)
        candidate = SCENES / (stem + ".ds2scene.json")
        if candidate.is_file() and bpy.data.objects.get("player"):
            return str(candidate)
    return None


def _register_partner(data, name, part, action_name):
    player = _actor(data, "player")
    entry = json.loads(json.dumps(player))
    entry.update(name=name, partner=True, parts={part: dict(player["parts"][part])}, actions={part: action_name})
    data["actors"] = [a for a in data["actors"] if a["name"] != name] + [entry]
    return entry


def _actor(data, name):
    for actor in data["actors"]:
        if actor["name"] == name:
            return actor
    raise KeyError("no actor %r in the scene" % name)


def _space(actor):
    """glTF actor space -> Blender world: the importer's Y-up turn after the actor's
    place in the scene (the sidecar's node_matrix: its sequence and the scene offset)."""
    node = actor.get("node_matrix")
    return GLTF_TO_BLENDER @ (Matrix(node) if node else Matrix.Identity(4))


def _rest_world(actor):
    """glTF-space rest matrix per bone, from the sidecar's native bind pose."""
    scale = actor["base_scale"]
    out = []
    for bone in actor["bones"]:
        w, x, y, z = bone["bind_q"]
        local = Matrix.Translation(Vector([v * scale for v in bone["bind_t"]])) @ Quaternion((w, x, y, z)).to_matrix().to_4x4()
        out.append(local if bone["parent"] < 0 else out[bone["parent"]] @ local)
    return out


# where tools/export_scene.py output is looked for when import_scene gets a bare scene name
SCENES = Path(__file__).resolve().parents[2] / "variants" / "build" / "scenes"


def _scene_file(glb):
    """A .glb path, or a scene name (igc_03_03) looked up in variants/build/scenes."""
    path = Path(str(glb)).expanduser()
    if not path.suffix:
        path = SCENES / (path.name + ".glb")
    if not path.is_file():
        found = sorted(p.name for p in SCENES.glob("*.glb")) if SCENES.is_dir() else []
        raise FileNotFoundError("%s does not exist. Exported scenes in %s: %s (make one with tools/export_scene.py)" % (
            path, SCENES, ", ".join(found) or "none"))
    return path


def import_scene(glb, sidecar=None):
    """Import a scene: a path to its .glb, or just its name (import_scene("igc_03_03"))."""
    glb = _scene_file(glb)
    sidecar = Path(sidecar) if sidecar else glb.with_suffix(".ds2scene.json")
    if not sidecar.is_file():
        raise FileNotFoundError("%s is missing: it belongs next to %s" % (sidecar, glb.name))
    scene = bpy.context.scene
    scene.render.fps, scene.render.fps_base = FPS, 1.0
    options = {p.identifier for p in bpy.ops.import_scene.gltf.get_rna_type().properties}
    extra = {"disable_bone_shape": True} if "disable_bone_shape" in options else {}   # no icosphere per bone
    bpy.ops.import_scene.gltf(filepath=str(glb), **extra)
    data = _sidecar(str(sidecar))
    for actor in data["actors"]:
        obj = bpy.data.objects.get(actor["name"])
        if obj and obj.type == "ARMATURE":
            obj["ds2_import_matrix"] = [v for row in obj.matrix_world for v in row]
    for obj in _cameras(data):
        if obj.data.users > 1:
            obj.data = obj.data.copy()   # each camera keys its own lens
        obj.data.sensor_fit = "HORIZONTAL"
        obj.data.animation_data_create()
        for part, fov in data["camera"]["fov_degrees"].items():
            action = bpy.data.actions.get("fov_" + part) or bpy.data.actions.new("fov_" + part)
            obj.data.animation_data.action = action
            for frame, degrees in enumerate(fov):
                obj.data.angle = math.radians(degrees)
                obj.data.keyframe_insert("lens", frame=frame)
        obj.data.animation_data.action = None
    _tidy(data)   # also puts the first part on every actor
    return data


def _tidy(data, far=5.0):
    """A readable scene: nothing selected, bones as sticks, the dashed parent lines off,
    and a rig's root bone hidden when it stays far from the body: it sits where the clip's
    space begins (the level's origin for Isaac and the props), hundreds of metres away."""
    for obj in bpy.context.selected_objects:
        obj.select_set(False)
    hidden = 0
    main = main_part(data)
    if main:
        show_part(main)
        bpy.context.scene.frame_set(0)
    for actor in data["actors"]:
        obj = bpy.data.objects.get(actor["name"])
        if not obj or obj.type != "ARMATURE":
            continue
        obj.data.display_type = "STICK"
        for bone in actor["bones"]:
            if bone["parent"] >= 0:
                continue
            pose = obj.pose.bones.get(bone["name"])
            children = [obj.pose.bones[b["name"]] for b in actor["bones"] if b["parent"] == bone["index"]]
            if pose and children and min((c.head - pose.head).length for c in children) > far:
                obj.data.bones[bone["name"]].hide = True
                hidden += 1
    for window in getattr(bpy.context.window_manager, "windows", []):
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                space = area.spaces.active
                space.overlay.show_relationship_lines = False
                space.region_3d.view_location = (0.0, 0.0, 1.0)
                space.region_3d.view_distance = 12.0
                space.clip_end = max(space.clip_end, 2000.0)
    print("ds2_scene: %d actors, %d far root bones hidden" % (len(data["actors"]), hidden))


def _cameras(data):
    """The camera on Isaac's m_camera bone and the free one with the baked path."""
    camera = data.get("camera") or {}
    out = []
    for key in ("node", "world_node"):
        obj = bpy.data.objects.get(camera.get(key, ""))
        if obj and obj.type == "CAMERA":
            out.append(obj)
    return out


def _assign(owner, action_name, prefix="OB"):
    """Play `action_name` on owner alone (an object, or camera data with prefix CA):
    its slot for this owner, the NLA off."""
    ad = owner.animation_data or owner.animation_data_create()
    action = bpy.data.actions.get(action_name)
    if action is None:
        raise KeyError("no action %r" % action_name)
    previous = (ad.action, getattr(ad, "action_slot", None), ad.use_nla)
    ad.use_nla = False
    ad.action = action
    slots = [s for s in getattr(action, "slots", []) if s.identifier == prefix + owner.name]
    if slots:
        ad.action_slot = slots[0]
    return previous


def _has_slot(action_name, obj, prefix="OB"):
    action = bpy.data.actions.get(action_name)
    return bool(action) and any(s.identifier == prefix + obj.name for s in getattr(action, "slots", []))


def _restore(obj, previous):
    ad = obj.animation_data
    ad.action = previous[0]
    if previous[1] is not None:
        ad.action_slot = previous[1]
    ad.use_nla = previous[2]


def main_part(data=None):
    """The part most actors play, the longest of those: the scene itself (010 for igc_03_03)."""
    data = data or _sidecar()
    parts = {}
    for actor in data["actors"]:
        for part, info in actor["parts"].items():
            if "#" in part:
                continue
            count, frames = parts.get(part, (0, 0))
            parts[part] = (count + 1, max(frames, info["frames"]))
    return max(parts, key=lambda p: parts[p]) if parts else None


_view = {"bones": False, "part": None}


def bones(visible=True):
    """Show the skeletons (to pose and key them) or hide them: the proxy figures move either way."""
    _view["bones"] = bool(visible)
    if _view["part"]:
        show_part(_view["part"])


def show_part(part):
    """Every actor that has this part plays it and shows; the others are hidden (their rest
    pose stands where their clip space begins, far off); the cameras take its FOV."""
    data = _sidecar()
    _view["part"] = part
    for actor in data["actors"]:
        obj = bpy.data.objects.get(actor["name"])
        if not obj:
            continue
        playing = part in actor["parts"]
        own = actor.get("actions", {}).get(part)   # a partner plays its own copy
        if playing:
            _assign(obj, own or part)
        elif obj.animation_data:
            obj.animation_data.action = None
            obj.animation_data.use_nla = False
        obj.hide_set(not (playing and _view["bones"]))
        for child in obj.children:
            child.hide_set(not playing)
    for obj in _cameras(data):
        if bpy.data.actions.get("fov_" + part):
            _assign(obj.data, "fov_" + part, "CA")
        if obj.name == (data.get("camera") or {}).get("world_node") and _has_slot(part, obj):
            _assign(obj, part)
    frames = max((a["parts"][part]["frames"] for a in data["actors"] if part in a["parts"]), default=1)
    bpy.context.scene.frame_start, bpy.context.scene.frame_end = 0, frames - 1


def _frames(data, actor_name, part, frames):
    if frames is not None:
        return list(frames)
    return list(range(_actor(data, actor_name)["parts"][part]["frames"]))


def _import_matrix(obj):
    """The armature object's place right after the import: moving it later (a partner
    beside Isaac) moves its clip, it does not change the bones' turns."""
    stored = obj.get("ds2_import_matrix")
    return Matrix([stored[r * 4:r * 4 + 4] for r in range(4)]) if stored else obj.matrix_world


def _turns(obj, actor):
    """Per bone, the importer's own turn of it, undone: read from the rest pose."""
    rest = _rest_world(actor)
    space = _space(actor)
    placed = _import_matrix(obj)
    return [((space @ rest[i]).inverted() @ (placed @ obj.data.bones[bone["name"]].matrix_local)).inverted()
            for i, bone in enumerate(actor["bones"])]


def _posed(obj, actor, turns, index):
    """A bone's posed matrix in the actor's glTF (game) space, at the current frame."""
    return _space(actor).inverted() @ obj.matrix_world @ obj.pose.bones[actor["bones"][index]["name"]].matrix @ turns[index]


def _sample(obj, actor, frames, names=None, animated=None):
    """Per frame, per bone: the game-space local (translation, WXYZ) of the posed armature.
    A parent the clip does not animate (Isaac's root bone `character`) counts in its bind
    pose, as in the game: whatever moved it (the armature object moved aside) goes into
    its children, which the clip does carry."""
    bones = actor["bones"]
    turns = _turns(obj, actor)
    rest = _rest_world(actor)
    scale = actor["base_scale"]
    wanted = set(names) if names else None
    out = {i: ([], []) for i, b in enumerate(bones) if wanted is None or b["name"] in wanted}
    scene = bpy.context.scene
    for frame in frames:
        scene.frame_set(frame)
        posed = [_posed(obj, actor, turns, i) for i in range(len(bones))]
        for i in out:
            parent = bones[i]["parent"]
            base = posed[parent] if parent >= 0 and (animated is None or parent in animated) else (rest[parent] if parent >= 0 else None)
            local = posed[i] if base is None else base.inverted() @ posed[i]
            t, q, _s = local.decompose()
            out[i][0].append([v / scale for v in t])
            out[i][1].append([q.w, q.x, q.y, q.z])
    return out


def _document(actor, samples, frames, extras):
    bones = []
    for i, (translations, rotations) in samples.items():
        previous = None
        for k, q in enumerate(rotations):
            if previous is not None and sum(a * b for a, b in zip(q, previous)) < 0:
                rotations[k] = [-v for v in q]
            previous = rotations[k]
        bone = actor["bones"][i]
        bones.append({"name": bone["name"], "hash": bone["hash"], "index": i, "translations": translations, "rotations": rotations})
    return {"schema": "ds2anim.v1", "coordinates": "native", "sample_rate": float(FPS), "frames": len(frames),
            "source": "blender " + bpy.data.filepath, "bones": bones, "extras": extras}


def export_action(actor_name, action_name, out_path, sidecar=None, frames=None, bones=None):
    """One actor's action (all its bones, or `bones` by name) as ds2anim.v1."""
    data = _sidecar(sidecar)
    actor = _actor(data, actor_name)
    obj = bpy.data.objects[actor_name]
    part = action_name if action_name in actor["parts"] else \
        next((p for p, a in actor.get("actions", {}).items() if a == action_name), None)
    animated = set(actor["parts"][part].get("bones", [])) or None if part else None
    frames = _frames(data, actor_name, part, frames) if part else list(frames or range(1))
    previous = _assign(obj, action_name)
    try:
        doc = _document(actor, _sample(obj, actor, frames, bones, animated), frames, [])
    finally:
        _restore(obj, previous)
    Path(out_path).write_text(json.dumps(doc), encoding="utf-8")
    return doc


def export_camera(part, out_path, sidecar=None, frames=None, camera_name=None):
    """m_camera and the FOV for this part, from the camera object as it is now animated."""
    data = _sidecar(sidecar)
    info = data["camera"]
    actor = _actor(data, info["actor"])
    rig = bpy.data.objects[actor["name"]]
    camera = bpy.data.objects[camera_name or info["node"]]
    frames = _frames(data, actor["name"], part, frames)
    index = [b["name"] for b in actor["bones"]].index(info["bone"])
    parent = actor["bones"][index]["parent"]
    previous = _assign(rig, part)
    # the camera's lens keys: the user's, or the part's own from the import
    lens_previous = motion_previous = None
    if not (camera.data.animation_data and camera.data.animation_data.action) and bpy.data.actions.get("fov_" + part):
        lens_previous = _assign(camera.data, "fov_" + part, "CA")
    # a free camera without an action of its own plays the part's baked path
    if not (camera.animation_data and camera.animation_data.action) and _has_slot(part, camera):
        motion_previous = _assign(camera, part)
    try:
        turns = _turns(rig, actor)
        translations, rotations, fov = [], [], []
        scene = bpy.context.scene
        for frame in frames:
            scene.frame_set(frame)
            camera_world = _space(actor).inverted() @ camera.matrix_world @ CAMERA_TURN.inverted()
            local = _posed(rig, actor, turns, parent).inverted() @ camera_world
            t, q, _s = local.decompose()
            translations.append([v / actor["base_scale"] for v in t])
            rotations.append([q.w, q.x, q.y, q.z])
            fov.append(math.degrees(2.0 * math.atan(camera.data.sensor_width / (2.0 * camera.data.lens))))
    finally:
        _restore(rig, previous)
        if lens_previous is not None:
            _restore(camera.data, lens_previous)
        if motion_previous is not None:
            _restore(camera, motion_previous)
    bone = actor["bones"][index]
    doc = _document(actor, {index: (translations, rotations)}, frames, [fov])
    doc["bones"][0]["name"], doc["bones"][0]["hash"] = bone["name"], bone["hash"]
    Path(out_path).write_text(json.dumps(doc), encoding="utf-8")
    return doc


# ---------------------------------------------------------------- one call for the partner

def partner_setup(scene="igc_03_03", name="partner", motion="copy"):
    """Everything for animating the second player, whatever state this file is in:
    imports the scene when it is not here yet, adds the partner when it is missing, shows
    the skeletons and selects the partner. Safe to run again and again. motion="copy" keys
    Isaac's motion on every frame; "first_frame" keys only his first pose, for a motion of
    its own (only used when the partner is created)."""
    try:
        _sidecar()
        loaded = True
    except RuntimeError:
        loaded = False
    if not loaded:
        import_scene(scene)
    data = _sidecar()
    if bpy.data.objects.get(name) is None:
        add_partner(name, motion=motion)
    else:
        show_part(bpy.data.objects[name].get("ds2_partner_part") or _view["part"] or main_part(data))
    bones(True)
    arm = bpy.data.objects[name]
    for obj in bpy.context.selected_objects:
        obj.select_set(False)
    arm.hide_set(False)
    arm.select_set(True)
    bpy.context.view_layer.objects.active = arm
    print("ds2_scene: scene %s %s, partner %r ready: select it, Ctrl+Tab for Pose Mode, pose and key (I), then partner_export()" % (
        data["scene"], "imported now" if not loaded else "already here", name))
    return arm


def partner_export(name="partner", path=None):
    """The partner's clip into variants/anims/partner, named for the game."""
    return export_partner(name, path=path)


# ---------------------------------------------------------------- the second player

PARTNER_FOLDER = Path(__file__).resolve().parents[2] / "variants" / "anims" / "partner"


def _channelbag_fcurves(action, slot):
    try:
        from bpy_extras import anim_utils
        bag = anim_utils.action_get_channelbag_for_slot(action, slot)
        return list(bag.fcurves) if bag else []
    except (ImportError, AttributeError):
        return list(getattr(action, "fcurves", []))


def add_partner(name="partner", part=None, offset=(1.0, 0.0, 0.0), motion="copy"):
    """A second Isaac in the scene: Isaac's skeleton and figure copied, standing `offset`
    metres from him (Blender axes: x right, y back, z up), with an action of its own.
    motion="copy" starts from Isaac's whole motion in this part, "first_frame" from his
    pose on the part's first frame only. Animate it like any armature (bones(True) shows
    the skeleton), then export_partner()."""
    data = _sidecar()
    part = part or _view["part"] or main_part(data)
    player = _actor(data, "player")
    if part not in player["parts"]:
        raise KeyError("Isaac has no part %r; his parts: %s" % (part, ", ".join(sorted(player["parts"]))))
    if name in bpy.data.objects:
        raise ValueError("%r exists already: remove it or pick another name" % name)
    source = bpy.data.objects["player"]
    arm = source.copy()
    arm.data = source.data.copy()
    arm.name = name
    for collection in source.users_collection:
        collection.objects.link(arm)
    arm.animation_data_clear()
    # its bones turn like Isaac's: his place right after the import, not the partner's moved one
    arm["ds2_import_matrix"] = [v for row in _import_matrix(source) for v in row]
    arm.matrix_world = Matrix.Translation(Vector(offset)) @ source.matrix_world
    figure = bpy.data.objects.get("player_proxy")
    if figure:
        copy = figure.copy()
        copy.name = name + "_proxy"
        for collection in figure.users_collection:
            collection.objects.link(copy)
        copy.parent = arm
        copy.matrix_parent_inverse = figure.matrix_parent_inverse.copy()
        for modifier in copy.modifiers:
            if modifier.type == "ARMATURE":
                modifier.object = arm
        copy.color = (0.2, 0.6, 1.0, 1.0)
    action = bpy.data.actions[part].copy()
    action.name = "%s_%s" % (name, part)
    ad = arm.animation_data_create()
    ad.use_nla = False
    ad.action = action
    slot = next((s for s in getattr(action, "slots", []) if s.identifier == "OBplayer"), None)
    if slot is not None:
        ad.action_slot = slot
    if motion == "first_frame":
        for curve in _channelbag_fcurves(action, slot):
            keys = curve.keyframe_points
            while len(keys) > 1:
                keys.remove(keys[len(keys) - 1], fast=True)
            curve.update()
    arm["ds2_partner_part"], arm["ds2_partner_action"] = part, action.name   # kept in the .blend
    _register_partner(data, name, part, action.name)
    show_part(part)
    return arm


def export_partner(name="partner", part=None, path=None):
    """The partner's action for this part as coop_partner_<Isaac's clip>.json, in
    variants/anims/partner unless `path` says otherwise; every frame of Isaac's clip."""
    data = _sidecar()
    entry = _actor(data, name)
    obj = bpy.data.objects.get(name)
    part = part or (obj.get("ds2_partner_part") if obj else None) or _view["part"] or main_part(data)
    player = _actor(data, "player")
    clip = player["parts"][part]["action"].lower()
    out = Path(path) if path else PARTNER_FOLDER / ("coop_partner_%s.json" % clip)
    out.parent.mkdir(parents=True, exist_ok=True)
    frames = list(range(player["parts"][part]["frames"]))
    doc = export_action(name, entry["actions"][part], out, frames=frames)
    print("ds2_scene: partner %s -> %s (%d frames, %d bones)" % (name, out, len(frames), len(doc["bones"])))
    return out
