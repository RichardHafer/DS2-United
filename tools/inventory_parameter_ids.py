"""Inventory serialized IDs from every cataloged PC SimGroup."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from ds2_assets import simgroup, u32
from remap_pool_guids import guid_hash
from resolve_pool_resources import header_guid

ROOT = Path(__file__).resolve().parents[1]


def read_asset(stream, meta, decompress):
    stream.seek(meta["chunk_offset"])
    header = stream.read(44)
    header_guid(header, meta)
    size = u32(header, 4)
    names = stream.read(size - 44).split(b"\0")
    if (len(names) < 3 or names[1].decode("latin1") != meta["path"] or
            names[2].decode("latin1") != meta.get("type", "SimGroup")):
        raise ValueError("Asset name or type mismatch")
    output = bytearray()
    while len(output) < meta["size"]:
        chunk = stream.read(8)
        if len(chunk) != 8 or chunk[:4] != b"COHS":
            raise ValueError("Missing payload chunk")
        length = u32(chunk, 4)
        if not 12 <= length <= 131072:
            raise ValueError("Invalid payload chunk size")
        body = stream.read(length - 8)
        if len(body) != length - 8:
            raise ValueError("Truncated payload")
        if body[:4] == b"TADS":
            output.extend(body[4:])
        elif body[:4] == b"kapR" and len(body) >= 8:
            compressed = u32(body, 4)
            if compressed > len(body) - 8:
                raise ValueError("Truncated compressed payload")
            output.extend(decompress(body[8:8 + compressed]))
        else:
            raise ValueError("Unexpected payload type")
    return bytes(output[:meta["size"]])


def image_ids(data, base):
    if base + 32 > len(data) or u32(data, base) != 0x91C8773C:
        raise ValueError("Unsupported parameter table")
    end = base + u32(data, base + 4)
    table, count = base + u32(data, base + 16), u32(data, base + 20)
    if end > len(data) or table < base + 32 or table + count * 12 > end:
        raise ValueError("Invalid image table bounds")
    ids = [u32(data, table + i * 12) for i in range(count)]
    if len(set(ids)) != len(ids):
        raise ValueError("Duplicate image definition")
    return ids


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--tool-dir", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    from ds2str import refpack_decompress
    catalog = ROOT / "analysis/catalog/assets.jsonl"
    rows = [row for line in catalog.read_text(encoding="utf-8").splitlines()
            if (row := json.loads(line))["type"] == "SimGroup"]
    report = {"scope": "Cataloged PC SimGroups, including unused parameter images",
              "catalog_sha256": hashlib.sha256(catalog.read_bytes()).hexdigest(),
              "assets": [], "errors": [], "runtime_ready": False}
    reserved, hashes = set(), set()
    for ordinal, row in enumerate(rows):
        try:
            with (args.work / row["archive"]).open("rb") as stream:
                if stream.read(4) != b"3slo":
                    raise ValueError("Not a PC stream")
                data = read_asset(stream, row, refpack_decompress)
            records, base = simgroup(data)
            ids = image_ids(data, base)
            reserved.update(ids)
            hashes.update(guid_hash(record["guid"]) for record in records)
            report["assets"].append({"archive": row["archive"], "asset": row["path"],
                "sha256": hashlib.sha256(data).hexdigest(), "images": len(ids), "entities": len(records)})
        except (OSError, ValueError, struct.error) as error:
            report["errors"].append({"archive": row["archive"], "asset": row["path"], "error": str(error)})
        if (ordinal + 1) % 100 == 0:
            print(f"Scanned {ordinal + 1}/{len(rows)} SimGroups", flush=True)
    report["image_ids"] = sorted(reserved)
    report["entity_hashes"] = sorted(hashes)
    report["catalog_complete"] = len(report["assets"]) == len(rows) and not report["errors"]
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Images: {len(reserved)}; entity hashes: {len(hashes)}; errors: {len(report['errors'])}")
    if not report["catalog_complete"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
