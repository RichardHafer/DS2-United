"""Summarise the player's cutscene banks (igc_*/se_* *_player*.bnk).

Pulls each bank and player.rcb out of their streams into OUT and decodes every
leaf with an external read-only BNK decoder (the ds2decode package of the
DS2Import UE5 plugin, passed as its parent folder; it is not part of this
repo). Prints frames per clip, C when the m_camera bone (index 4) is animated
and F<n> for extra scalar channels (the camera FOV).

    python -I tools/probe_igc_banks.py <folder with ds2decode> <out dir> [name regex]
"""
import json
import re
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DECODER = Path(sys.argv[1])
OUT = Path(sys.argv[2])
PATTERN = re.compile(sys.argv[3] if len(sys.argv) > 3 else r"^(igc|se)_.*_player.*\.bnk$")
RIG = ("player.rcb", r"ds2dat10\global_assets\global_assets.str")
CAMERA_BONE = 4

sys.path.insert(0, str(REPO / "tools"))
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))
import ds2str  # noqa: E402
from ds2_assets import assets  # noqa: E402
sys.path.insert(0, str(DECODER))
from ds2decode import ds2_bnk_core as core  # noqa: E402

wanted = {RIG[0]: RIG[1]}
with open(REPO / "analysis" / "catalog" / "assets.jsonl", encoding="utf-8") as f:
    for line in f:
        if '"BNK"' not in line:
            continue
        row = json.loads(line)
        if row["type"] == "BNK" and PATTERN.match(row["name"]) and row["name"] not in wanted:
            wanted[row["name"]] = row["archive"]
print("banks", len(wanted) - 1)

OUT.mkdir(parents=True, exist_ok=True)
by_archive = {}
for name, archive in wanted.items():
    if not (OUT / name).exists():
        by_archive.setdefault(archive, set()).add(name)
for archive, left in by_archive.items():
    stream = ds2str.StreamSet(str(WORK / archive))
    for meta, chunks in assets(stream):
        if meta.get("name") in left:
            (OUT / meta["name"]).write_bytes(stream.asset_payload(chunks, meta["size"]))
            left.discard(meta["name"])
        if not left:
            break

rcb = core.read_rcb(OUT / RIG[0])
skeleton = rcb.skeletons[0]
stats = Counter()
for name in sorted(n for n in wanted if n != RIG[0]):
    path = OUT / name
    if not path.exists():
        print("%-34s not extracted" % name)
        stats["missing"] += 1
        continue
    try:
        bank = core.read_bnk(path)
    except Exception as exc:
        print("%-34s read failed: %s" % (name, str(exc)[:140]))
        stats["read_failed"] += 1
        continue
    if bank.skeleton_hash != skeleton.skeleton_hash:
        print("%-34s other skeleton %08X" % (name, bank.skeleton_hash))
        stats["other_skeleton"] += 1
        continue
    parts = []
    for leaf in bank.leaves:
        try:
            clip = core.decode_clip_for_skeleton(bank, rcb, leaf, skeleton)
        except Exception as exc:
            parts.append("%s:failed(%s)" % (leaf.action_name, str(exc)[:60]))
            stats["leaf_failed"] += 1
            continue
        camera = CAMERA_BONE in clip.bone_channels
        parts.append("%s:%d%s%s" % (leaf.action_name, leaf.frame_count, "C" if camera else "",
                                    "F%d" % len(clip.extra_channels) if clip.extra_channels else ""))
        stats["leaf_ok"] += 1
        stats["layout_%08X" % leaf.owner_channel_layout] += 1
        stats["leaf_with_camera"] += camera
    print("%-34s %s" % (name, " ".join(parts)))
print(dict(stats))
