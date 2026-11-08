"""Build the mod's own animation banks from ds2anim.v1 clips in variants/anims.

    python -I tools/build_anim_banks.py

variants/anims/partner/<state>.json
    New player clips, all in one bank (ds2coop_partner.bnk). The file name is
    the state name and must start with coop_; the cutscene partner plays
    coop_partner_<Isaac's clip> in sync with that scene part ([ScenePartner]).
variants/anims/scenes/<retail bank>/<action>.json
    Edits of a retail cutscene clip, e.g. scenes/igc_03_03_player/
    igc_03_03_player_010.json with only m_camera in it: the bank is copied, the
    action gets the edited clip (the bones and the FOV the document leaves out
    stay as they are) and priority 1, so it wins over the retail state of the
    same name whatever loads first.

Writes variants/build/anims/*.bnk and manifest.json; variant_pack.py build
puts them into the pool stream with GUIDs of their own. No clips, no banks.
"""
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import ds2_anim_json as aj  # noqa: E402
import ds2_bnk as bnk  # noqa: E402
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402

SOURCE = ROOT / "variants" / "anims"
OUT = ROOT / "variants" / "build" / "anims"
RETAIL = OUT / "retail"
RIG = ("player.rcb", r"ds2dat10\global_assets\global_assets.str")
PARTNER_BANK = "ds2coop_partner"
PATHS_FILE = "ds2coop_partner_paths.bin"   # the partner clips' m_root per frame ([ScenePartner] SyncedPlacement=path)


def retail_assets(names):
    """Retail banks and the rig by asset name, pulled out of their streams once."""
    RETAIL.mkdir(parents=True, exist_ok=True)
    wanted = {name: None for name in names if not (RETAIL / name).exists()}
    if wanted:
        with (ROOT / "analysis" / "catalog" / "assets.jsonl").open(encoding="utf-8") as f:
            for line in f:
                if not any(name in line for name in wanted):
                    continue
                row = json.loads(line)
                if row["name"] in wanted and wanted[row["name"]] is None:
                    wanted[row["name"]] = row["archive"]
        if RIG[0] in wanted:
            wanted[RIG[0]] = RIG[1]
        sys.path.insert(0, str(MODDING_TOOLS))
        import ds2str
        from ds2_assets import assets
        by_archive = {}
        for name, archive in wanted.items():
            if archive is None:
                raise SystemExit("%s is in no stream of the catalog" % name)
            by_archive.setdefault(archive, set()).add(name)
        for archive, left in by_archive.items():
            stream = ds2str.StreamSet(str(WORK / archive))
            for meta, chunks in assets(stream):
                if meta.get("name") in left:
                    (RETAIL / meta["name"]).write_bytes(stream.asset_payload(chunks, meta["size"]))
                    left.discard(meta["name"])
            if left:
                raise SystemExit("not found in %s: %s" % (archive, ", ".join(sorted(left))))
    return {name: (RETAIL / name).read_bytes() for name in names}


def resource_guid(name):
    """Four words in the retail pattern (id, id, id, check), ids of our own namespace."""
    seed = ("ds2coop/anims/" + name).encode()
    rid = zlib.crc32(seed) & 0xFFFFFFFF
    check = zlib.crc32(seed[::-1]) & 0xFFFFFFFF
    return [rid, rid, rid, check]


def root_path(clip, rig, rig_data):
    """Per frame, where the entity stands when the clip's m_root motion is taken out of the
    pose (as the game does for Isaac): m_root's level matrix against its bind, rows right,
    up, forward, position in world units (rig units times the rig's base scale)."""
    import export_scene as es
    m_root = rig.bone_hashes.index(bnk.sdbm("m_root"))
    parent = rig.parents[m_root]
    scale = aj.rig_scale(rig_data)
    bind = aj.local_bind(rig)
    chain, b = [], parent
    while b >= 0:
        chain.append(b)
        b = rig.parents[b]
    above = [[float(i == j) for j in range(4)] for i in range(4)]
    for b in reversed(chain):
        above = es.mul(above, es.mat_from_tq([v * scale for v in bind[b][0]], bind[b][1]))
    rest = es.inverse_rigid(es.mat_from_tq([v * scale for v in bind[m_root][0]], bind[m_root][1]))
    rows = []
    for frame in range(clip.frames):
        t, q, _s = bnk.pose(clip, m_root, frame) if m_root in clip.bones else (bind[m_root][0], bind[m_root][1], None)
        m = es.mul(es.mul(above, es.mat_from_tq([v * scale for v in t], q)), rest)
        # an entity matrix: its rows are the column-vector matrix's columns
        rows.extend(m[r][c] for c in range(4) for r in range(4))
    return rows


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    partner = sorted((SOURCE / "partner").glob("*.json")) if (SOURCE / "partner").exists() else []
    scenes = sorted(p for p in (SOURCE / "scenes").glob("*") if p.is_dir()) if (SOURCE / "scenes").exists() else []
    names = [RIG[0]] + ["%s.bnk" % folder.name for folder in scenes]
    files = retail_assets(names)
    rig_data = files[RIG[0]]
    rig = bnk.read_rig(rig_data)
    banks = []
    for old in list(OUT.glob("*.bnk")) + [OUT / PATHS_FILE]:
        if old.exists():
            old.unlink()
    if partner:
        clips = {}
        for path in partner:
            state = path.stem
            if not state.startswith("coop_"):
                raise SystemExit("%s: a partner clip's name must start with coop_" % path.name)
            clips[state] = aj.document_to_clip(json.loads(path.read_text(encoding="utf-8")), rig, rig_data)
        data = bnk.new_bank(clips, rig)
        check = bnk.read_bank(data)
        for state, clip in clips.items():
            back = bnk.decode_owner(check, check.action(state).leaves[0].owner, rig)
            if back.frames != clip.frames or sorted(back.bones) != sorted(clip.bones):
                raise SystemExit("%s did not read back" % state)
        banks.append((PARTNER_BANK, data, sorted(clips)))
        print("partner bank: %d clips, %d bytes" % (len(clips), len(data)))
        paths = bytearray(struct.pack("<4sII", b"PPTH", 1, len(clips)))
        for state, clip in sorted(clips.items()):
            rows = root_path(clip, rig, rig_data)
            paths += struct.pack("<II", bnk.sdbm(state), clip.frames) + struct.pack("<%df" % len(rows), *rows)
        (OUT / PATHS_FILE).write_bytes(bytes(paths))
        print("root paths: %d clips, %d bytes -> %s (install as randomizer\\%s)" % (len(clips), len(paths), OUT / PATHS_FILE, PATHS_FILE))
    for folder in scenes:
        data = files["%s.bnk" % folder.name]
        bank = bnk.read_bank(data)
        edited = []
        for path in sorted(folder.glob("*.json")):
            action = path.stem
            leaves = bank.action(action).leaves
            if len(leaves) != 1:
                raise SystemExit("%s: %s has %d leaves; one per edited action" % (folder.name, action, len(leaves)))
            base = bnk.decode_owner(bank, leaves[0].owner, rig)
            clip = aj.document_to_clip(json.loads(path.read_text(encoding="utf-8")), rig, rig_data, base=base)
            data = bnk.replace_leaf(data, action, clip, rig, leaf_id=leaves[0].leaf_id)
            data = bnk.set_priority(data, action, 1)
            bank = bnk.read_bank(data)
            edited.append(action)
        if edited:
            banks.append(("ds2coop_" + folder.name, data, edited))
            print("scene bank %s: %s edited, %d bytes" % (folder.name, ", ".join(edited), len(data)))
    manifest = []
    for name, data, states in banks:
        (OUT / (name + ".bnk")).write_bytes(data)
        manifest.append({"name": name + ".bnk", "path": "ds2coop\\anims\\%s.bnk.win" % name, "file": "variants/build/anims/%s.bnk" % name,
                         "guid": ["%08X" % w for w in resource_guid(name)], "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest(),
                         "states": states})
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8", newline="\n")
    print("%d bank(s) for the pool: %s" % (len(manifest), ", ".join(m["name"] for m in manifest) or "none"))


if __name__ == "__main__":
    main()
