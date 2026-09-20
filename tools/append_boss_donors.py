"""Put the Divider and the Ubermorph into the resident pool.

The pool's roots were chosen from twelve common families. The Divider (core and
its five crawling parts) and the Ubermorph, called Hunter in the catalog, were
left out because their packages were never forced resident and their records
drag dependencies behind them: the core spawns its parts from templates in its
own package, both carry a DismemberableProp in their character archive, and
every one of them points at a NavAnimSettings in global_assets.

This appends those records the way the vent and direct donors were appended,
which is the only safe way to change a stream: existing chunks keep their
offsets, only the trailing padding is replaced. The difference is that a root
does not come alone. The entity closure is computed the same way the original
pool's was (resolve_pool_dependencies.closure), every record with authored
spawn flags of zero is copied, and every copied identity is rewritten into the
RNDM/POOL namespace with one mapping across all groups, so the core's spawn
references land on the copied parts rather than on a chapter that is not
loaded. Flagged records (NavAnimSettings) are left where they are: they live in
global_assets, which is always resident, and are referenced by retail GUID.

The two character packages are added to the pool TOC as forced streams so the
models are resident wherever the enemy shows up. Run
tools/embed_stream_requirements.py afterwards, then rebuild.
"""
import argparse
import hashlib
import json
import struct
import sys
import uuid
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")

from ds2_assets import assets, simgroup  # noqa: E402
from stream_framing import frame_stream  # noqa: E402
from remap_pool_guids import guid_hash, remap  # noqa: E402
from resolve_pool_dependencies import closure, compact  # noqa: E402
from build_isolated_pool import isolate_many  # noqa: E402
from build_research_toc import build, pool_root_name  # noqa: E402
from append_vent_donors import MAX_CHUNK, chunk, new_guid, trailing_padding  # noqa: E402
from append_direct_donors import refresh_manifest  # noqa: E402

# The roots offered as donors. One core, one of each crawling part, one hunter.
# All are direct arrivals (default_NONE_descriptor) without an authored
# animation, which is what the direct pool asks of a donor.
ROOTS = [
    ("4FCAE89F-2AF74004-5A544946-42424947", "DividerCore", "divider", "divider core, transport hub"),
    ("4FCB0B67-3249673E-5A544946-42424947", "DividerPart", "divider", "head, transport hub 3"),
    ("31CB2A00-FFFCDB2B-43495250-32303045", "DividerPart", "divider", "left hand, psec03 upper"),
    ("31CB2A00-FC99005A-43495250-32303045", "DividerPart", "divider", "left foot, psec03 upper"),
    ("37CAFB97-E6945DA7-484C4143-314E554F", "DividerPart", "divider", "right hand, living hall"),
    ("37CAFB97-C6B80397-484C4143-314E554F", "DividerPart", "divider", "right foot, living hall"),
    ("4FCB0C2B-E261A03C-4952414D-4C4C454E", "Hunter", "ubermorph", "ubermorph, mtb area 01"),
    # 15.09.2026: for the extra-spawn rules (a Guardian on a wall you picked, a
    # Tripod in a room of your choice). Never offered as replacements: their
    # classes are neither ordinary nor extended in the catalog.
    ("39CB757A-AD1E6541-4E415548-38343147", "Guardian", "guardian", "guardian, transport hub a"),
    ("58CA4940-D568C3ED-5A54414B-39383230", "Tripod", "tripod", "tripod, saferoom (zero-g authored)"),
]
PACKAGES = {
    "divider": r"global_assets\global_assets\ds_assets\char_str\npc\div_cct\divider",
    "ubermorph": r"global_assets\global_assets\ds_assets\char_str\npc\ubermorph_cct\ubermorph",
    "guardian": r"global_assets\global_assets\ds_assets\char_str\npc\guar_cct\guardian",
    "guardian_pod": r"global_assets\global_assets\ds_assets\char_str\npc\guarpd_cct\guardian_pod",
    "tripod": r"global_assets\global_assets\ds_assets\char_str\npc\tripod_cct\tripod",
    "tripodprojectile": r"global_assets\global_assets\ds_assets\char_str\npc\tripodprojectile_cct\tripodprojectile",
}


def pool_identities(pool_path):
    """Every record identity the pool already carries, to refuse a collision."""
    import ds2str
    stream = ds2str.StreamSet(pool_path)
    found = {}
    for meta, chunks in assets(stream):
        if meta["type"] != "SimGroup":
            continue
        data = stream.asset_payload(chunks, meta["size"])
        for record in simgroup(data)[0]:
            found[record["guid"]] = meta["path"]
    return found


def stock_identity(name):
    stock = json.loads((ROOT / "analysis/stock_toc_dependencies.json").read_text())
    values = set()
    for toc in stock["tocs"]:
        for entry in toc["entries"]:
            if entry["identity"]["name"] == name:
                values.add(int(entry["identity"]["hash"], 16))
    if len(values) != 1:
        raise ValueError("Missing or conflicting stock identity: " + name)
    return next(iter(values))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=Path(r"D:/SteamLibrary/steamapps/common/Dead Space 2/work"))
    parser.add_argument("--pool", type=Path, default=ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    import ds2str

    index = defaultdict(list)
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            node = compact(json.loads(line))
            index[node["guid"]].append(node)

    # Closure of every root; one record set across all of them.
    nodes, roots = {}, []
    existing = pool_identities(args.pool)
    for guid, family, package, label in ROOTS:
        matches = index.get(guid, [])
        if len(matches) != 1:
            raise SystemExit("root %s: %d catalog matches" % (guid, len(matches)))
        if new_guid(guid) in existing:
            # appended by an earlier run: stays in the header, nothing is copied twice
            roots.append({"guid": guid, "family": family, "package": package, "label": label,
                          "archive": matches[0]["archive"], "closure": 0, "present": True})
            print("%-12s %s  %-28s already in the pool" % (family, guid, label))
            continue
        result = closure({"guid": guid, "source_archive": matches[0]["archive"], "package": package}, index)
        if result["truncated"]:
            raise SystemExit("closure truncated for " + guid)
        copied = 0
        for node in result["nodes"]:
            if node["spawn_flags"]:
                continue        # NavAnimSettings and the like stay in global_assets
            key = node["archive"], node["asset"], node["index"]
            nodes.setdefault(key, node)
            copied += 1
        roots.append({"guid": guid, "family": family, "package": package, "label": label,
                      "archive": matches[0]["archive"], "closure": copied})
        print("%-12s %s  %-28s closure=%d records" % (family, guid, label, copied))

    mapping = {}
    for root in roots:
        if root.get("present"):
            mapping[root["guid"]] = new_guid(root["guid"])
    shared = []
    for key, node in nodes.items():
        target = new_guid(node["guid"])
        if node["guid"] in mapping and mapping[node["guid"]] != target:
            raise SystemExit("one identity, two targets: " + node["guid"])
        mapping[node["guid"]] = target
        if target in existing:
            # a dependency an earlier run already copied (a drop list both the
            # Divider and the Guardian name): references point at that copy
            shared.append(key)
    for key in shared:
        print("   shared with the pool already: %s" % nodes[key]["guid"])
        del nodes[key]
    hashes = [guid_hash(g) for g in mapping.values() if g not in existing] + [guid_hash(g) for g in existing]
    if len(set(hashes)) != len(hashes):
        raise SystemExit("identity hash collision against the pool")
    print("records to copy: %d, identities remapped: %d, pool holds %d already"
          % (len(nodes), len(mapping), len(existing)))

    # Extract per source asset, several instances at once, then remap.
    groups = defaultdict(dict)
    for (archive, asset, idx), node in nodes.items():
        groups[(archive, asset)][idx] = node
    payloads = []
    for (archive, asset), wanted in sorted(groups.items()):
        stream = ds2str.StreamSet(args.work / archive)
        if not stream.pc:
            raise SystemExit("not a PC stream: " + archive)
        found = False
        for meta, chunks in assets(stream):
            if meta["type"] != "SimGroup" or meta["path"] != asset:
                continue
            data = stream.asset_payload(chunks, meta["size"])
            payload, selected, _unused = isolate_many(data, wanted)
            if [r["guid"] for r in selected] != [wanted[i]["guid"] for i in sorted(wanted)]:
                raise SystemExit("closure does not match the source: " + asset)
            payload, stats = remap(payload, mapping)
            payloads.append((archive, asset, payload, stats, [wanted[i] for i in sorted(wanted)]))
            found = True
            break
        if not found:
            raise SystemExit("asset missing in %s: %s" % (archive, asset))
        print("   %-40s %2d records  %6d bytes  identities=%d references=%d"
              % (asset.split(chr(92))[-1][:40], len(wanted), len(payload), stats["identities"], stats["references"]))

    data = bytearray(args.pool.read_bytes())
    cut = trailing_padding(data)
    print("pool: %d bytes, real content ends at %d" % (len(data), cut))
    del data[cut:]
    appended = []
    for archive, asset, payload, stats, records in payloads:
        header = bytearray(b"RDHS" + bytes(8) + struct.pack("<I", 0xCCB51828) + bytes(16) + bytes(4))
        struct.pack_into("<I", header, 32, len(payload))
        name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2boss/" + archive + "|" + asset)).upper()
        digest = hashlib.sha256((archive + "|" + asset).encode()).hexdigest()[:12]
        path = "boss_donors/%s.simgroup.bin" % digest
        body = bytearray(header)
        body.extend((name + "\0" + path + "\0SimGroup\0").encode("latin1"))
        data.extend(chunk(bytes(body)))
        for start in range(0, len(payload), MAX_CHUNK - 12):
            data.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
        appended.append({"archive": archive, "asset": asset, "name": name, "path": path,
                         "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                         "records": [{"class": r["class"], "source": r["guid"], "guid": mapping[r["guid"]]} for r in records]})
    data = bytearray(frame_stream(bytes(data)))
    print("appended %d groups, pool now %d bytes (%.1f MiB)" % (len(appended), len(data), len(data) / 1048576))

    manifest_path = args.pool.with_suffix(".toc.json")
    manifest = json.loads(manifest_path.read_text())
    forced = [tuple(e) for e in manifest["forced"]]
    for package, toc_name in PACKAGES.items():
        if all(name != toc_name for _identity, name in forced):
            forced.append((stock_identity(toc_name), toc_name))
            print("TOC: forcing %s" % toc_name)
    if not args.write:
        print("dry run; pass --write to emit")
        return

    args.pool.write_bytes(bytes(data))
    toc = build(pool_root_name(manifest), manifest["content_size"],
                [tuple(e) for e in manifest["shared"]], forced)
    (args.pool.with_suffix(".toc")).write_bytes(toc)
    manifest["forced"] = [list(e) for e in forced]
    manifest["toc_bytes"] = len(toc)
    manifest["toc_sha256"] = hashlib.sha256(toc).hexdigest()
    manifest["boss_donors"] = {"roots": roots, "appended": appended}
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    refresh_manifest(args.pool)
    (ROOT / "analysis/boss_donors.json").write_text(json.dumps({"roots": roots, "appended": appended, "mapping": mapping}, indent=1))

    def words(text):
        return "{" + ",".join("0x%su" % w for w in text.split("-")) + "}"

    lines = ["// Generated by tools/append_boss_donors.py.", "#pragma once", "namespace ds2 {",
             "// A Divider or Ubermorph the pool carries as a direct arrival. `source` is",
             "// the authored entity it was copied from, so the record inherits its model,",
             "// family and orientation. Its dependencies were copied and remapped with it.",
             "struct BossDonorRow { Guid guid, source; const char* family; const char* package; };",
             "inline constexpr BossDonorRow kBossDonors[] = {"]
    for root in sorted(roots, key=lambda r: mapping[r["guid"]]):
        lines.append("    {%s,%s,%s,%s},  // %s" % (words(mapping[root["guid"]]), words(root["guid"]),
                                                  json.dumps(root["family"]), json.dumps(root["package"]), root["label"]))
    lines += ["};", "}", ""]
    (ROOT / "src/generated_boss_donors.h").write_text("\n".join(lines), encoding="utf-8")
    print("wrote src/generated_boss_donors.h with %d donors; now run tools/embed_stream_requirements.py" % len(roots))


if __name__ == "__main__":
    main()
