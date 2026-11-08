"""Put the Dementia mutator's phantom Pack children into the resident pool.

The hallucinated Pack of chapter 11's Dementia Land (PackEnemy records
36CB5AC0-E992D1xx in npcs_mtb_escape_folderstream) lives only in that chapter.
This appends four of the ten identical siblings with their closure, the same
way tools/append_boss_donors.py appends the bosses: existing chunks keep their
offsets, every copied identity is rewritten into the RNDM/POOL namespace, and
records already in the pool are not copied twice. Unlike the boss donors they
are no randomizer donors: they go to the manifest's own "dementia_records"
section and to src/generated_dementia_records.h, which only the co-op plugin's
Dementia module reads (analysis/DEMENTIA_PHANTOM_PACK_2026-10-19.md).

Their package pack_boy_dementia is already forced resident by the pool TOC.

The variant build starts from the variant-free base pool, so run this on the
variant-free pool (a build_variants.py run with an empty spec), then
tools/variant_pack.py build --write, which refreshes the base from it.

    python -I tools/append_dementia_records.py [--pool PATH] [--write]
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
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))

from ds2_assets import assets  # noqa: E402
from stream_framing import frame_stream  # noqa: E402
from remap_pool_guids import guid_hash, remap  # noqa: E402
from resolve_pool_dependencies import closure, compact  # noqa: E402
from build_isolated_pool import isolate_many  # noqa: E402
from append_vent_donors import MAX_CHUNK, chunk, new_guid, trailing_padding  # noqa: E402
from append_direct_donors import refresh_manifest  # noqa: E402
from append_boss_donors import pool_identities  # noqa: E402

ROOTS = [
    ("36CB5AC0-E992D12F-45544150-3230314C", "dementia pack child 1"),
    ("36CB5AC0-E992D132-45544150-3230314C", "dementia pack child 2"),
    ("36CB5AC0-E992D134-45544150-3230314C", "dementia pack child 3"),
    ("36CB5AC0-E992D136-45544150-3230314C", "dementia pack child 4"),
]
PACKAGE = "pack_boy_dementia"
PACKAGE_TOC = r"global_assets\global_assets\ds_assets\char_str\npc\pack_boy_dementia_cct\pack_boy_dementia"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--pool", type=Path, default=ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    import ds2str

    manifest_path = args.pool.with_suffix(".toc.json")
    manifest = json.loads(manifest_path.read_text())
    if (manifest.get("variants") or {}).get("rows") and args.write:
        raise SystemExit("the pool carries variants: run this on the variant-free pool (see the docstring)")
    if all(name != PACKAGE_TOC for _identity, name in manifest["forced"]):
        raise SystemExit("the pool TOC does not force %s" % PACKAGE_TOC)

    index = defaultdict(list)
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            node = compact(json.loads(line))
            index[node["guid"]].append(node)

    nodes, roots = {}, []
    existing = pool_identities(args.pool)
    for guid, label in ROOTS:
        matches = index.get(guid, [])
        if len(matches) != 1:
            raise SystemExit("root %s: %d catalog matches" % (guid, len(matches)))
        if new_guid(guid) in existing:
            roots.append({"guid": guid, "label": label, "archive": matches[0]["archive"], "closure": 0, "present": True})
            print("%s  %-24s already in the pool" % (guid, label))
            continue
        result = closure({"guid": guid, "source_archive": matches[0]["archive"], "package": PACKAGE}, index)
        if result["truncated"]:
            raise SystemExit("closure truncated for " + guid)
        copied = 0
        for node in result["nodes"]:
            if node["spawn_flags"]:
                continue        # global_assets records stay where they are
            nodes.setdefault((node["archive"], node["asset"], node["index"]), node)
            copied += 1
        roots.append({"guid": guid, "label": label, "archive": matches[0]["archive"], "closure": copied})
        print("%s  %-24s closure=%d records" % (guid, label, copied))

    mapping = {r["guid"]: new_guid(r["guid"]) for r in roots if r.get("present")}
    shared = []
    for key, node in nodes.items():
        target = new_guid(node["guid"])
        if node["guid"] in mapping and mapping[node["guid"]] != target:
            raise SystemExit("one identity, two targets: " + node["guid"])
        mapping[node["guid"]] = target
        if target in existing:
            shared.append(key)
    for key in shared:
        print("   shared with the pool already: %s" % nodes[key]["guid"])
        del nodes[key]
    hashes = [guid_hash(g) for g in mapping.values() if g not in existing] + [guid_hash(g) for g in existing]
    if len(set(hashes)) != len(hashes):
        raise SystemExit("identity hash collision against the pool")
    print("records to copy: %d, identities remapped: %d, pool holds %d already" % (len(nodes), len(mapping), len(existing)))

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
        for record in [wanted[i] for i in sorted(wanted)]:
            print("      %-28s %s -> %s" % (record.get("class"), record["guid"], mapping[record["guid"]]))

    data = bytearray(args.pool.read_bytes())
    cut = trailing_padding(data)
    print("pool: %d bytes, real content ends at %d" % (len(data), cut))
    del data[cut:]
    appended = []
    for archive, asset, payload, stats, records in payloads:
        header = bytearray(b"RDHS" + bytes(8) + struct.pack("<I", 0xCCB51828) + bytes(16) + bytes(4))
        struct.pack_into("<I", header, 32, len(payload))
        name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2dementia/" + archive + "|" + asset)).upper()
        digest = hashlib.sha256((archive + "|" + asset).encode()).hexdigest()[:12]
        path = "dementia_records/%s.simgroup.bin" % digest
        body = bytearray(header)
        body.extend((name + "\0" + path + "\0SimGroup\0").encode("latin1"))
        data.extend(chunk(bytes(body)))
        for start in range(0, len(payload), MAX_CHUNK - 12):
            data.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
        appended.append({"archive": archive, "asset": asset, "name": name, "path": path, "bytes": len(payload),
                         "sha256": hashlib.sha256(payload).hexdigest(),
                         "records": [{"class": r["class"], "source": r["guid"], "guid": mapping[r["guid"]]} for r in records]})
    data = bytearray(frame_stream(bytes(data)))
    print("appended %d groups, pool now %d bytes (%.1f MiB)" % (len(appended), len(data), len(data) / 1048576))
    if not args.write:
        print("dry run; pass --write to emit")
        return

    args.pool.write_bytes(bytes(data))
    manifest = json.loads(manifest_path.read_text())
    previous = manifest.get("dementia_records") or {}
    manifest["dementia_records"] = {"roots": roots, "appended": (previous.get("appended") or []) + appended}
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    refresh_manifest(args.pool)

    def words(text):
        return "{" + ",".join("0x%su" % w for w in text.split("-")) + "}"

    lines = ["// Dead Space 2 United by Hafer.", "// Generated by tools/append_dementia_records.py.", "#pragma once", "#include <cstdint>",
             "namespace coop_dementia_data {",
             "// The Dementia mutator's phantom Pack children: pool copies of chapter 11's",
             "// Dementia Land PackEnemy records (`source`), each with its own identity so",
             "// living phantoms never share one.",
             "struct DementiaRecordRow { uint32_t guid[4], source[4]; };",
             "inline constexpr DementiaRecordRow kDementiaRecords[] = {"]
    for root in roots:
        lines.append("    {%s,%s},  // %s" % (words(new_guid(root["guid"])), words(root["guid"]), root["label"]))
    lines += ["};", "}", ""]
    (ROOT / "src/generated_dementia_records.h").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print("wrote src/generated_dementia_records.h with %d records; now run tools/variant_pack.py build --write" % len(roots))


if __name__ == "__main__":
    main()
