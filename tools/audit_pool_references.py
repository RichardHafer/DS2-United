"""List GUID references inside the runtime pool that nothing in it satisfies.

guid_redirect only counts its misses (twin_missing=N), so a playtest reports a
number and no names. This reads the built pool directly: every record identity it
carries, every typed GUID reference (parameter fields with flags 12), and the
difference. A reference is reported as dangling when the pool has no record with
that identity AND the target is not resident elsewhere - global_assets is always
loaded, so a retail GUID pointing there resolves fine.
"""
import json, struct, sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")
from ds2_assets import assets, simgroup, loose_images  # noqa: E402
import ds2str  # noqa: E402

def u32(data, at):
    return struct.unpack_from("<I", data, at)[0]

pool = ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str"
stream = ds2str.StreamSet(str(pool))
identities, refs = set(), defaultdict(set)
for meta, chunks in assets(stream):
    if meta["type"] != "SimGroup":
        continue
    data = stream.asset_payload(chunks, meta["size"])
    records, base = simgroup(data)
    for record in records:
        identities.add(record["guid"])
    images = loose_images(data, base)
    image_end = base + u32(data, base + 4)
    owner = {r["offset"]: r["guid"] for r in records}
    for image in images.values():
        for field in image["fields"]:
            if field["flags"] != 12:
                continue
            rel = struct.unpack("<i", struct.pack("<I", field["value"]))[0]
            at = field["offset"] + 8 + rel
            if at < base or at + 16 > image_end:
                continue
            guid = "-".join("%08X" % w for w in struct.unpack_from("<4I", data, at))
            if guid == "00000000-00000000-00000000-00000000":
                continue
            refs[guid].add(image.get("class", "?"))

# where does each dangling target live in retail?
home = defaultdict(list)
with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as fh:
    for line in fh:
        node = json.loads(line)
        if node.get("guid") in refs:
            home[node["guid"]].append((node.get("class", "?"), node["archive"]))

print("pool records: %d, distinct referenced GUIDs: %d" % (len(identities), len(refs)))
dangling = {g: c for g, c in refs.items() if g not in identities}
globals_ok = chapter_only = unknown = 0
rows = []
for guid, classes in sorted(dangling.items()):
    places = home.get(guid, [])
    archives = {a for _c, a in places}
    in_global = any(a.rsplit("\\", 1)[-1].lower() == "global_assets.str" for a in archives)
    cls = places[0][0] if places else "?"
    if in_global:
        globals_ok += 1
        continue                      # resident everywhere, resolves by retail GUID
    if not places:
        unknown += 1
        rows.append((guid, cls, "not in catalog", 0, sorted(classes)))
    else:
        chapter_only += 1
        rows.append((guid, cls, sorted(archives)[0].rsplit("\\", 1)[-1], len(archives), sorted(classes)))
print("dangling: %d chapter-only, %d unknown, %d resolve via global_assets\n"
      % (chapter_only, unknown, globals_ok))
for guid, cls, where, count, by in rows:
    print("  %s  %-22s copies=%-3d %-28s referenced by %s" % (guid, cls, count, where, ",".join(by[:3])))
