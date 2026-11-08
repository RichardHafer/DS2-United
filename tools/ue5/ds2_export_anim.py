"""Export an AnimSequence on an imported DS2 skeleton as ds2anim.v1 JSON.

Run inside the Unreal editor (Output Log, Python):

    import sys
    sys.path.append(r"C:/path/to/DeadSpace2Randomizer/tools/ue5")
    sys.path.append(r"C:/path/to/DeadSpace2Randomizer/tools")
    import ds2_export_anim

Two kinds of skeleton:

- One imported from a scene .glb (tools/export_scene.py, UE's own glTF import):
      ds2_export_anim.export_scene_anim("/Game/igc_03_03/player_Anim_010", r"C:/out/partner.json",
                                        r"C:/scenes/igc_03_03.ds2scene.json", "player")
  The axis map of the import is read from the skeleton's reference pose against
  the sidecar's bind pose, so the result is in the game's own space ("native").
- One from the DS2Import plugin (bones named after the DS2 bones or bone_NNN_HASH):
      ds2_export_anim.export("/Game/DS2/Partner/partner_igc_01_02", r"C:/out/partner.json")
  Its default import bakes the rig scale (player.rcb 1.818) into translations:
  keep rig_scale_baked=True then.

Export clips with their root motion, not an _InPlace derivative. Then build a bank:

    python -I tools/ds2_anim_json.py build <base bank> player.rcb partner.json out.bnk --action NAME

Written for the UE5 AnimPose Python API; not run in this repo's tests (the axis map
math is, in tools/test_scene_export.py).
"""
import json

import unreal

import ds2_axis_calibration as axes


def _frame_count(anim):
    library = unreal.AnimationLibrary
    for getter in ("get_num_keys", "get_num_frames"):
        if hasattr(library, getter):
            count = int(getattr(library, getter)(anim))
            if getter == "get_num_frames" and not hasattr(library, "get_num_keys"):
                count += 1   # frames count the intervals between keys
            return count
    raise RuntimeError("this Unreal version has no AnimationLibrary.get_num_keys/get_num_frames")


def _frame_rate(anim):
    try:
        rate = unreal.AnimationLibrary.get_frame_rate(anim)
        if hasattr(rate, "numerator"):
            return float(rate.numerator) / float(rate.denominator or 1)
        return float(rate)
    except Exception:
        return 30.0


def _curve(anim, name, frames, rate):
    """Samples of a float curve at every frame, or None when it is missing."""
    try:
        times, values = unreal.AnimationLibrary.get_float_keys(anim, name)
    except Exception:
        return None
    times, values = list(times), list(values)
    if not times:
        return None
    out = []
    for frame in range(frames):
        t = frame / rate
        if t <= times[0]:
            out.append(values[0])
            continue
        if t >= times[-1]:
            out.append(values[-1])
            continue
        i = max(k for k in range(len(times)) if times[k] <= t)
        f = (t - times[i]) / (times[i + 1] - times[i])
        out.append(values[i] + (values[i + 1] - values[i]) * f)
    return out


def export(anim_path, out_path, unit_scale=100.0, rig_scale_baked=True, bones=None, fov_curve="ds2_fov"):
    """Write every frame of `anim_path` to `out_path`.

    `bones` limits the export to these bone names. For a cutscene camera edit
    pass bones=["m_camera"]: building with --replace keeps every other bone of
    the scene clip. A float curve named `fov_curve` (degrees, like the scene
    clip's own FOV channel) replaces the FOV; without it the scene keeps its FOV."""
    anim = unreal.load_asset(anim_path)
    if not isinstance(anim, unreal.AnimSequence):
        raise RuntimeError("%s is not an AnimSequence" % anim_path)
    frames = _frame_count(anim)
    rate = _frame_rate(anim)
    options = unreal.AnimPoseEvaluationOptions()
    poses = [unreal.AnimPoseExtensions.get_anim_pose_at_frame(anim, frame, options) for frame in range(frames)]
    names = [str(n) for n in unreal.AnimPoseExtensions.get_bone_names(poses[0])]
    if bones:
        wanted = set(bones)
        names = [n for n in names if n in wanted]
    tracks = []
    for name in names:
        translations, rotations = [], []
        for pose in poses:
            transform = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.LOCAL)
            t, q = transform.translation, transform.rotation
            translations.append([t.x, t.y, t.z])
            rotations.append([q.x, q.y, q.z, q.w])
        tracks.append({"name": name, "translations": translations, "rotations": rotations})
    fov = _curve(anim, fov_curve, frames, rate) if fov_curve else None
    document = {"schema": "ds2anim.v1", "coordinates": "ue", "unit_scale": float(unit_scale),
                "rig_scale_baked": bool(rig_scale_baked), "sample_rate": rate, "frames": frames,
                "source": anim_path, "bones": tracks, "extras": [fov] if fov else []}
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(document, handle)
    unreal.log("ds2_export_anim: %s -> %s (%d frames at %.2f fps, %d bones, FOV curve %s)" % (
        anim_path, out_path, frames, rate, len(tracks), "exported" if fov else "absent"))
    return document


def _local(pose, name):
    transform = unreal.AnimPoseExtensions.get_bone_pose(pose, name, unreal.AnimPoseSpaces.LOCAL)
    t, q = transform.translation, transform.rotation
    return [t.x, t.y, t.z], [q.x, q.y, q.z, q.w]


def export_scene_anim(anim_path, out_path, sidecar_path, actor_name, bones=None, fov_curve="ds2_fov"):
    """An AnimSequence of an actor imported from a scene .glb, back in the game's space."""
    with open(sidecar_path, encoding="utf-8") as handle:
        sidecar = json.load(handle)
    actor = [a for a in sidecar["actors"] if a["name"] == actor_name]
    if not actor:
        raise RuntimeError("no actor %r in %s" % (actor_name, sidecar_path))
    actor = actor[0]
    anim = unreal.load_asset(anim_path)
    if not isinstance(anim, unreal.AnimSequence):
        raise RuntimeError("%s is not an AnimSequence" % anim_path)
    skeleton = anim.get_editor_property("skeleton")
    reference = unreal.AnimPoseExtensions.get_reference_pose(skeleton)
    present = set(str(n) for n in unreal.AnimPoseExtensions.get_bone_names(reference))
    scale = actor["base_scale"]
    # the import's axis map, from the rest pose: glTF local = native bind * base scale
    pairs_t, pairs_q, root = [], [], None
    for bone in actor["bones"]:
        if bone["name"] not in present:
            continue
        t, q = _local(reference, bone["name"])
        gltf_t = [v * scale for v in bone["bind_t"]]
        w, x, y, z = bone["bind_q"]
        if bone["parent"] < 0:
            root = (bone["name"], (gltf_t, [x, y, z, w]), (t, q))
            continue
        if sum(v * v for v in gltf_t) > 1e-8:
            pairs_t.append((gltf_t, t))
        pairs_q.append(([x, y, z, w], q))
    p, s, flip, error = axes.calibrate(pairs_t, pairs_q)
    root_a = axes.root_map(p, s, flip, root[1], root[2]) if root else None
    unreal.log("ds2_export_anim: axis map %s scale %.4f mirror %s, rest pose error %.4g; root %s" % (
        p, s, flip < 0, error, "follows it" if root_a is None else "has a conversion of its own"))
    frames = _frame_count(anim)
    rate = _frame_rate(anim)
    options = unreal.AnimPoseEvaluationOptions()
    poses = [unreal.AnimPoseExtensions.get_anim_pose_at_frame(anim, frame, options) for frame in range(frames)]
    wanted = set(bones) if bones else None
    tracks = []
    for bone in actor["bones"]:
        if bone["name"] not in present or (wanted and bone["name"] not in wanted):
            continue
        translations, rotations = [], []
        for pose in poses:
            local = _local(pose, bone["name"])
            if bone["parent"] < 0 and root_a is not None:
                t, q = axes.root_to_gltf(root_a, *local)
            else:
                t, q = axes.to_gltf(p, s, flip, *local)
            translations.append([v / scale for v in t])
            rotations.append([q[3], q[0], q[1], q[2]])
        tracks.append({"name": bone["name"], "hash": bone["hash"], "translations": translations, "rotations": rotations})
    fov = _curve(anim, fov_curve, frames, rate) if fov_curve else None
    document = {"schema": "ds2anim.v1", "coordinates": "native", "sample_rate": rate, "frames": frames, "source": anim_path,
                "bones": tracks, "extras": [fov] if fov else [],
                "axis_map": {"p": p, "scale": s, "mirror": flip < 0, "rest_error": error, "root_own_conversion": root_a is not None}}
    with open(out_path, "w", encoding="utf-8") as handle:
        json.dump(document, handle)
    unreal.log("ds2_export_anim: %s -> %s (%d frames at %.2f fps, %d bones)" % (anim_path, out_path, frames, rate, len(tracks)))
    return document
