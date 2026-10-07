"""Carry the chapter-only records the pool's enemies point at into the pool.

The pool holds the 34 enemy roots, but not the records those roots name by class
default. Those dependencies live only inside chapter NPC mini-global archives,
as byte-identical copies under one GUID, so a pool enemy placed in any other
level points at a record that is not loaded:

  * a pool Puker names its spit projectile and the record is absent
    (analysis/randomizer_playtest_bugs_2026-09-17.md, bug 3),
  * a pool Infector names the Zombie it turns a corpse into, and that record is
    authored in exactly one archive in the game, so outside it the infection
    consumes the corpse and produces nothing, which never satisfies a
    kill-counted door (bug 1).

tools/append_shared_records.py cannot carry these: its CARRIED_CLASSES list does
not name them, and it refuses any record with more than one component image.
These have nine to twenty-eight. They must be copied the way a donor root is
copied, which is what this does:

  * the entity closure of each dependency is resolved the way the pool's own
    roots were (resolve_pool_dependencies.closure),
  * records with authored spawn flags are left alone, because they live in
    global_assets and are referenced by retail GUID,
  * every copied identity is rewritten into the RNDM/POOL namespace,
  * the pool is extended append-only: existing chunks keep their offsets and
    only the trailing padding is replaced,
  * finally the records the pool already carries are repointed at the new twins
    in place, byte for byte, the way tools/remap_installed_pool.py does it, so
    the pool Puker stops naming a chapter record and names its own copy.

Run tools/embed_stream_requirements.py afterwards, then rebuild the plugin.
Dry run by default; pass --write to emit.
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

from ds2_assets import assets, simgroup  # noqa: E402
from stream_framing import frame_stream  # noqa: E402
from remap_pool_guids import guid_bytes, guid_hash, remap  # noqa: E402
from resolve_pool_dependencies import closure, compact  # noqa: E402
from build_isolated_pool import isolate_many  # noqa: E402
from build_research_toc import build, pool_root_name  # noqa: E402
from append_vent_donors import MAX_CHUNK, chunk, new_guid, trailing_padding  # noqa: E402
from append_direct_donors import refresh_manifest  # noqa: E402
from remap_installed_pool import file_offset, walk  # noqa: E402

# The dependencies a pool enemy names by class default and the pool does not
# carry. Verified against analysis/runtime_stream/ds2_randomizer_pool.str: each
# retail GUID still stands in the pool with no RNDM/POOL twin beside it.
DEPENDENCIES = [
    ("58CAB417-F2C73BF9-3045454C-58373634", "puker spit projectile (CMD_SetRangedAttackProjectile)"),
    ("36C8D00C-FBDB69AE-45544150-3230314C", "spitter projectile"),
    ("58C81FF5-1CA4318D-3030554C-58583732", "brute projectile"),
    ("58C7A221-EFA5E62E-3030554C-58583732", "lurker projectile (only partly twinned)"),
    ("4BC78C36-9D3C8B28-544E4543-53574F4B", "pregnant spawn target"),
    ("36CB50A1-AEA1303F-45544150-3230314C", "infector spawned NPC (CMD_SetSpawnedNPC)"),
]


def pool_identities(pool_path):
    """Every record identity the pool already carries, to refuse a collision."""
    import ds2str
    stream = ds2str.StreamSet(str(pool_path))
    found = {}
    for meta, chunks in assets(stream):
        if meta["type"] != "SimGroup":
            continue
        data = stream.asset_payload(chunks, meta["size"])
        for record in simgroup(data)[0]:
            found[record["guid"]] = meta["path"]
    return found


def pick_source(matches):
    """These records recur as byte-identical copies; take one deterministically."""
    return sorted(matches, key=lambda n: (n["archive"], n["asset"], n["index"]))[0]


def repoint(pool_path, mapping):
    """Rewrite references to the retail dependencies in place, byte for byte.

    Identities are not touched here: the copies were already emitted under their
    twin identity, so only references inside records the pool already carried can
    match. No chunk changes length.
    """
    items = list(walk(pool_path))
    raw = bytearray(pool_path.read_bytes())
    identities = references = patched = 0
    for header, payload, spans in items:
        if header["type"] != "SimGroup":
            continue
        edited, report = remap(payload, mapping)
        identities += report["identities"]
        references += report["references"]
        for position in report["edited_offsets"]:
            at = file_offset(spans, position)
            if at is None:
                raise SystemExit("%s: a reference lies in a compressed chunk" % header["name"])
            raw[at] = edited[position]
            patched += 1
    return bytes(raw), identities, references, patched


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=WORK)
    parser.add_argument("--pool", type=Path, default=ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    import ds2str

    index = defaultdict(list)
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            node = compact(json.loads(line))
            index[node["guid"]].append(node)

    existing = pool_identities(args.pool)
    nodes, carried = {}, []
    # A dependency an earlier run already copied still needs its references
    # repointed: the twin can be in the pool while the records that name it were
    # left pointing at the retail GUID (the lurker projectile was in that state).
    present = {}
    for guid, label in DEPENDENCIES:
        matches = index.get(guid, [])
        if not matches:
            raise SystemExit("dependency %s: not in the catalog" % guid)
        if new_guid(guid) in existing:
            present[guid] = new_guid(guid)
            carried.append({"guid": guid, "label": label, "closure": 0, "present": True})
            print("%-40s %s  already in the pool" % (label[:40], guid))
            continue
        source = pick_source(matches)
        # No dedicated character package: these are dependencies of enemies whose
        # packages the pool already forces. closure only echoes the key back.
        result = closure({"guid": guid, "source_archive": source["archive"], "package": None}, index)
        if result["truncated"]:
            raise SystemExit("closure truncated for " + guid)
        copied = 0
        for node in result["nodes"]:
            if node["spawn_flags"]:
                continue  # flagged record, referenced by retail GUID where it lies
            # Only the global_assets archive itself, not the character packages
            # that live under a global_assets directory (slasher_hospital.str
            # and the like, which are per-character records worth copying).
            if node["archive"].rsplit(chr(92), 1)[-1].lower() == "global_assets.str":
                # global_assets is always resident, so the retail GUID resolves.
                # Copying shared records such as the PowerUps a drop list names
                # would duplicate them under a second identity for no gain.
                continue
            key = node["archive"], node["asset"], node["index"]
            nodes.setdefault(key, node)
            copied += 1
        carried.append({"guid": guid, "label": label, "archive": source["archive"],
                        "copies": len(matches), "closure": copied})
        print("%-40s %s  copies=%-3d closure=%d records" % (label[:40], guid, len(matches), copied))

    mapping = dict(present)
    for key, node in sorted(nodes.items()):
        target = new_guid(node["guid"])
        if node["guid"] in mapping and mapping[node["guid"]] != target:
            raise SystemExit("one identity, two targets: " + node["guid"])
        mapping[node["guid"]] = target
    shared = [key for key, node in nodes.items() if mapping[node["guid"]] in existing]
    for key in shared:
        print("   already copied by an earlier run: %s" % nodes[key]["guid"])
        del nodes[key]
    hashes = [guid_hash(g) for g in mapping.values() if g not in existing] + [guid_hash(g) for g in existing]
    if len(set(hashes)) != len(hashes):
        raise SystemExit("identity hash collision against the pool")
    print("records to copy: %d, identities remapped: %d, pool holds %d already"
          % (len(nodes), len(mapping), len(existing)))

    groups = defaultdict(dict)
    for (archive, asset, idx), node in nodes.items():
        groups[(archive, asset)][idx] = node
    payloads = []
    for (archive, asset), wanted in sorted(groups.items()):
        stream = ds2str.StreamSet(str(args.work / archive))
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

    appended = []
    if not payloads:
        print("nothing new to copy; only stale references are repointed")
    data = bytearray(args.pool.read_bytes())
    if payloads:
        cut = trailing_padding(data)
        print("pool: %d bytes, real content ends at %d" % (len(data), cut))
        del data[cut:]
    for archive, asset, payload, stats, records in payloads:
        header = bytearray(b"RDHS" + bytes(8) + struct.pack("<I", 0xCCB51828) + bytes(16) + bytes(4))
        struct.pack_into("<I", header, 32, len(payload))
        name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2chapdep/" + archive + "|" + asset)).upper()
        digest = hashlib.sha256((archive + "|" + asset).encode()).hexdigest()[:12]
        path = "chapter_dependencies/%s.simgroup.bin" % digest
        body = bytearray(header)
        body.extend((name + "\0" + path + "\0SimGroup\0").encode("latin1"))
        data.extend(chunk(bytes(body)))
        for start in range(0, len(payload), MAX_CHUNK - 12):
            data.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
        appended.append({"archive": archive, "asset": asset, "name": name, "path": path,
                         "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                         "records": [{"class": r["class"], "source": r["guid"], "guid": mapping[r["guid"]]} for r in records]})
    if payloads:
        data = bytearray(frame_stream(bytes(data)))
        print("appended %d groups, pool now %d bytes (%.1f MiB)" % (len(appended), len(data), len(data) / 1048576))

    if not args.write:
        print("dry run; pass --write to emit")
        return

    if payloads:
        args.pool.write_bytes(bytes(data))

    # Now make the records the pool already carried name the copies.
    patched, identities, references, byte_count = repoint(args.pool, mapping)
    args.pool.write_bytes(patched)
    print("repointed in place: %d identities, %d typed references, %d bytes"
          % (identities, references, byte_count))
    if identities:
        raise SystemExit("unexpected: an identity was rewritten by the repoint pass")
    left = [old for old in mapping if guid_bytes(old) in patched]
    print("retail dependency GUIDs still present in the pool: %d" % len(left))

    manifest_path = args.pool.with_suffix(".toc.json")
    manifest = json.loads(manifest_path.read_text())
    toc = build(pool_root_name(manifest), manifest["content_size"],
                [tuple(e) for e in manifest["shared"]], [tuple(e) for e in manifest["forced"]])
    (args.pool.with_suffix(".toc")).write_bytes(toc)
    manifest["toc_bytes"] = len(toc)
    manifest["toc_sha256"] = hashlib.sha256(toc).hexdigest()
    manifest["chapter_dependencies"] = {"carried": carried, "appended": appended}
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    refresh_manifest(args.pool)
    (ROOT / "analysis/chapter_dependencies.json").write_text(
        json.dumps({"carried": carried, "appended": appended, "mapping": mapping}, indent=1))
    print("wrote the pool and analysis/chapter_dependencies.json; "
          "now run tools/embed_stream_requirements.py, then rebuild")


if __name__ == "__main__":
    main()
