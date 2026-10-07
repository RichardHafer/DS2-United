"""Carry the chapter-only data records the pool's enemies point at.

A pool enemy keeps every typed reference its authored original had. Most of
them name records that are resident everywhere (NavAnimSettings and
DismemberableProps in global_assets) or records the pool copied along. A few
name data that only exists inside chapter NPC archives, recurring there as
byte-identical copies under one GUID: the grapple camera descriptions
(BoneBoundCameraInfo, PairedAnimCameraInfo), item drop lists and tentacle
controls. Whether such a record is loaded depends on which room the player is
in, and the engine does not check: the grapple path at 0x010AAF30 looks the
camera info up (0x00776440), creates it (0x00776480) and dereferences the
result. Dump 33076 (2026-09-15 01:19) is a pool Leaper grappling the player in
a room whose archives carry neither of its two camera infos, so the create
returned null and the game read [0x60].

This appends one copy of each such record to the pool, rewritten into the
RNDM/POOL namespace, and repoints every reference inside the pool at the copy,
byte for byte in place (tools/remap_installed_pool.py mechanics), so the
existing chunks keep their offsets. Only records with spawn flags 0 and at most one
component image qualify; anything else is refused rather than guessed at.

Usage: python tools/append_shared_records.py [--write]
Then: python tools/embed_stream_requirements.py and a plugin build.
"""
import argparse
import collections
import hashlib
import json
import struct
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))

import ds2str  # noqa: E402
from ds2_assets import assets, simgroup, loose_images  # noqa: E402
from stream_framing import frame_stream  # noqa: E402
from remap_pool_guids import guid_hash, remap  # noqa: E402
from remap_installed_pool import walk, file_offset  # noqa: E402
from build_isolated_pool import isolate_many  # noqa: E402
from append_vent_donors import MAX_CHUNK, chunk, new_guid, trailing_padding  # noqa: E402
from append_direct_donors import refresh_manifest  # noqa: E402
from append_boss_donors import pool_identities  # noqa: E402

CARRIED_CLASSES = {"BoneBoundCameraInfo", "PairedAnimCameraInfo", "ItemDropList", "TentacleControl"}
NULL = "00000000-00000000-00000000-00000000"


def pool_groups(pool):
    import ds2str
    stream = ds2str.StreamSet(str(pool))
    for meta, chunks in assets(stream):
        if meta["type"] != "SimGroup":
            continue
        data = stream.asset_payload(chunks, meta["size"])
        yield meta, data


def dangling(pool, catalog):
    """Every GUID the pool references that is neither a pool record nor a
    global_assets record, with the class the catalog knows for it."""
    have = set()
    refs = collections.defaultdict(set)
    groups = list(pool_groups(pool))
    for _meta, data in groups:
        for r in simgroup(data)[0]:
            have.add(r["guid"])
    for meta, data in groups:
        records, base = simgroup(data)
        images = loose_images(data, base)
        for r in records:
            # the entity image too: a drop list keeps its references there
            for iid in [r["image_id"], *r["component_images"]]:
                for f in images[iid]["fields"]:
                    if f["flags"] != 12:
                        continue
                    at = f["offset"] + 8 + f["value"]
                    g = "-".join("%08X" % w for w in struct.unpack_from("<4I", data, at))
                    refs[g].add(r["guid"])
    out = {}
    for g, sources in refs.items():
        if g in have or g == NULL or g not in catalog:
            continue
        rows = catalog[g]
        if any("global_assets" in r["archive"] for r in rows):
            continue
        out[g] = {"class": rows[0]["class"], "rows": rows, "referenced_by": sorted(sources)}
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pool", type=Path, default=ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str")
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()
    import ds2str

    catalog = collections.defaultdict(list)
    with (ROOT / "analysis/catalog/entities.jsonl").open(encoding="utf-8") as stream:
        for line in stream:
            r = json.loads(line)
            catalog[r["guid"]].append(r)
    carry(args, catalog, 1)


def carry(args, catalog, depth):
    """One pass. A carried drop list names further chapter-only lists, so a
    written pass that leaves nested records behind runs again, at most three
    times; every pass starts from the pool as it is on disk."""
    print("== pass %d" % depth)
    missing = dangling(args.pool, catalog)
    wanted = {g: m for g, m in missing.items() if m["class"] in CARRIED_CLASSES}
    for g, m in sorted(missing.items()):
        flag = "carry" if g in wanted else "leave"
        print("%-5s %s %-22s archives=%-3d referenced_by=%d" % (flag, g, m["class"], len(m["rows"]), len(m["referenced_by"])))
    if not wanted:
        print("nothing to carry")
        return

    existing = pool_identities(args.pool)
    mapping = {}
    for g in wanted:
        target = new_guid(g)
        if target in existing:
            raise SystemExit("pool already carries %s" % target)
        mapping[g] = target
    hashes = [guid_hash(g) for g in mapping.values()] + [guid_hash(g) for g in existing]
    if len(set(hashes)) != len(hashes):
        raise SystemExit("identity hash collision against the pool")

    # One source occurrence per record: prefer an archive that holds several
    # of them, so the copies come out of as few groups as possible.
    by_archive = collections.Counter()
    for g, m in wanted.items():
        for r in m["rows"]:
            by_archive[(r["archive"], r["asset"])] += 1
    picks = collections.defaultdict(dict)
    for g, m in wanted.items():
        best = max(m["rows"], key=lambda r: (by_archive[(r["archive"], r["asset"])], -r["index"]))
        picks[(best["archive"], best["asset"])][best["index"]] = g

    payloads = []
    for (archive, asset), indices in sorted(picks.items()):
        stream = ds2str.StreamSet(str(WORK / archive))
        if not stream.pc:
            raise SystemExit("not a PC stream: " + archive)
        for meta, chunks in assets(stream):
            if meta["type"] != "SimGroup" or meta["path"] != asset:
                continue
            data = stream.asset_payload(chunks, meta["size"])
            records, base = simgroup(data)
            images = loose_images(data, base)
            for index, g in indices.items():
                rec = records[index]
                if rec["guid"] != g:
                    raise SystemExit("catalog index drifted for %s in %s" % (g, asset))
                if rec["spawn_flags"] != 0 or len(rec["component_images"]) > 1:
                    raise SystemExit("%s is not a plain data record (flags=%d images=%d)"
                                     % (g, rec["spawn_flags"], len(rec["component_images"])))
                for f in [f for iid in [rec["image_id"], *rec["component_images"]] for f in images[iid]["fields"]]:
                    if f["flags"] == 12:
                        at = f["offset"] + 8 + f["value"]
                        ref = "-".join("%08X" % w for w in struct.unpack_from("<4I", data, at))
                        if ref != NULL and ref not in mapping and ref not in existing:
                            print("   note: %s itself references %s (left as is)" % (g, ref))
            payload, selected, _unused = isolate_many(data, sorted(indices))
            if [r["guid"] for r in selected] != [indices[i] for i in sorted(indices)]:
                raise SystemExit("isolation does not match the selection in " + asset)
            payload, stats = remap(payload, mapping)
            payloads.append((archive, asset, payload, stats, [indices[i] for i in sorted(indices)]))
            break
        else:
            raise SystemExit("asset not found: " + asset)

    # Repoint the references inside the existing pool, in place.
    raw = bytearray(args.pool.read_bytes())
    references = 0
    for header, payload, spans in walk(args.pool):
        if header["type"] != "SimGroup":
            continue
        patched, report = remap(payload, mapping)
        if report["identities"]:
            raise SystemExit("an existing pool group carries one of the identities")
        references += report["references"]
        for position in report["edited_offsets"]:
            at = file_offset(spans, position)
            if at is None:
                raise SystemExit("%s: a reference lies in a compressed chunk" % header["name"])
            raw[at] = patched[position]
    print("references repointed inside the pool: %d" % references)

    cut = trailing_padding(raw)
    del raw[cut:]
    added = []
    for archive, asset, payload, stats, guids in payloads:
        header = bytearray(b"RDHS" + bytes(8) + struct.pack("<I", 0xCCB51828) + bytes(16) + bytes(4))
        struct.pack_into("<I", header, 32, len(payload))
        name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2shared/" + ",".join(guids))).upper()
        path = "shared_records/%s.simgroup.bin" % hashlib.sha1(asset.encode()).hexdigest()[:12]
        body = bytearray(header)
        body.extend((name + "\0" + path + "\0SimGroup\0").encode("latin1"))
        raw.extend(chunk(bytes(body)))
        for start in range(0, len(payload), MAX_CHUNK - 12):
            raw.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
        for g in guids:
            added.append({"guid": mapping[g], "source": g, "class": wanted[g]["class"], "archive": archive,
                          "asset": asset, "name": name, "path": path,
                          "referenced_by": wanted[g]["referenced_by"]})
        print("group %s: %d records from %s (%d identities, %d references rewritten)"
              % (path, len(guids), asset.split("\\")[-1], stats["identities"], stats["references"]))
    raw = bytearray(frame_stream(bytes(raw)))
    print("pool: %d records carried, now %d bytes" % (len(added), len(raw)))
    if not args.write:
        print("dry run; pass --write to emit")
        return
    args.pool.write_bytes(bytes(raw))
    refresh_manifest(args.pool)
    manifest = ROOT / "analysis/runtime_stream/shared_records.json"
    previous = json.loads(manifest.read_text()) if manifest.exists() and depth > 1 else []
    manifest.write_text(json.dumps(previous + added, indent=1))
    print("wrote %s and the manifest digest" % args.pool)

    left = dangling(args.pool, catalog)
    still = {g: m["class"] for g, m in left.items() if m["class"] in CARRIED_CLASSES}
    if still:
        if depth >= 3:
            raise SystemExit("still dangling after three passes: %s" % still)
        print("nested records left: %s; next pass" % still)
        return carry(args, catalog, depth + 1)
    print("verified: no chapter-only %s reference left in the pool" % "/".join(sorted(CARRIED_CLASSES)))


if __name__ == "__main__":
    main()
