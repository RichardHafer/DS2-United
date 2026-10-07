"""Put authored vent arrivals into the resident pool.

The pool exists so a replacement can come from anywhere in the game rather than
from whatever the current room happens to carry. Its 34 donors are all direct
arrivals though, so vents were left with the level's own entities; in chapter 1
that is slashers and nothing else.

This appends isolated vent arrivals to `ds2_randomizer_pool.str`. Appending is
safe in a way that rewriting is not: the container's existing chunks keep their
offsets, only the trailing FILL padding is replaced. The TOC lists the stream's
dependencies, not its assets (checked: none of the 120 asset names appears in
it), so it does not have to change either.

Every appended identity is rewritten into the RNDM/POOL namespace first. A pool
entity that keeps its retail GUID is what made the game fault on the way back to
the menu (analysis/POOL_IDENTITY_COLLISION.md), and there is no reason to repeat
that.

Only donors whose character stream the pool already forces resident are taken,
so nothing new has to be loaded: 74 of the 76 the plan lists qualify.
"""
import argparse
import collections
import csv
import hashlib
import json
import re
import struct
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from ds2_paths import MODDING_TOOLS  # noqa: E402
sys.path.insert(0, str(MODDING_TOOLS))

from ds2_assets import simgroup  # noqa: E402
from stream_framing import frame_stream  # noqa: E402
from remap_pool_guids import guid_bytes, guid_hash, remap  # noqa: E402

MAX_CHUNK = 131072
TAG = (0x4D444E52, 0x4C4F4F50)  # "RNDM", "POOL"
BS = chr(92)


def new_guid(old):
    words = [int(w, 16) for w in old.split("-")]
    return "%08X-%08X-%08X-%08X" % (words[0], words[1], TAG[0], TAG[1])


def chunk(body, tag=b"COHS"):
    if len(body) + 8 > MAX_CHUNK:
        raise ValueError("Chunk too large")
    return tag + struct.pack("<I", len(body) + 8) + body


def pool_streams():
    """The character streams the plugin already forces resident."""
    names = set()
    for line in (ROOT / "src/generated_stream_requirements.h").read_text(encoding="utf-8").splitlines():
        if "char_str" in line:
            names.add(line.split('"')[1].replace(BS + BS, BS).split(BS)[-1])
    return names


def trailing_padding(data):
    """Offset where the stream's trailing FILL chunks begin."""
    position = 12
    last_real = len(data)
    while position + 8 <= len(data):
        size = struct.unpack_from("<I", data, position + 4)[0]
        if size < 8 or position + size > len(data):
            break
        tag = data[position:position + 4]
        if tag != b"LLIF":
            last_real = position + size
        position += size
    return last_real


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pool", type=Path, default=ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str")
    parser.add_argument("--donors", type=Path, default=ROOT / "analysis/vent_pool")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()

    resident = pool_streams()
    report = json.loads((args.donors / "report.json").read_text())
    with (ROOT / "analysis/catalog/enemy_targets.tsv").open(newline="", encoding="utf-8") as f:
        targets = {r["guid"]: r for r in csv.DictReader(f, delimiter="\t")}

    chosen = [e for e in report["entries"] if e["package"] in resident]
    print("donors offered: %d, usable without new resources: %d"
          % (len(report["entries"]), len(chosen)))

    data = bytearray((args.pool).read_bytes())
    cut = trailing_padding(data)
    print("pool: %d bytes, real content ends at %d" % (len(data), cut))
    del data[cut:]

    added = []
    for entry in chosen:
        payload = (args.donors / entry["file"]).read_bytes()
        records, _ = simgroup(payload)
        mapping = {r["guid"]: new_guid(r["guid"]) for r in records}
        payload, stats = remap(payload, mapping)
        # The SHDR of a SimGroup asset: 36 bytes, magic at +12, size at +32.
        header = bytearray(b"RDHS" + bytes(8) + struct.pack("<I", 0xCCB51828) + bytes(16) + bytes(4))
        struct.pack_into("<I", header, 32, len(payload))
        name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2vent/" + entry["guid"])).upper()
        path = "vent_donors/%s/%s.simgroup.bin" % (entry["package"], entry["contract"][:12])
        body = bytearray(header)
        body.extend((name + "\0" + path + "\0SimGroup\0").encode("latin1"))
        data.extend(chunk(bytes(body)))
        for start in range(0, len(payload), MAX_CHUNK - 12):
            data.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
        row = targets[entry["guid"]]
        added.append({"guid": new_guid(entry["guid"]), "source": entry["guid"],
                      "vent": row["vent_guid"], "contract": entry["contract"],
                      "family": entry["family"], "package": entry["package"],
                      "context": row["context"], "name": name, "path": path,
                      "bytes": len(payload), "identities": stats["identities"],
                      "references": stats["references"],
                      "sha256": hashlib.sha256(payload).hexdigest()})

    # The native reader takes the stream in 128 KiB blocks and no chunk may cross
    # a block boundary, which is what the FILL chunks are for. frame_stream lays
    # that out, splitting payloads where it has to; feeding it the original
    # reproduces the original byte for byte, so the existing assets keep their
    # places and only the appended ones are framed anew.
    data = bytearray(frame_stream(bytes(data)))
    print("appended %d donors, pool now %d bytes (%.1f MiB)"
          % (len(added), len(data), len(data) / 1048576))

    families = collections.Counter(a["family"] for a in added)
    print("families in the pool's vents: %s" % dict(families))
    if not args.write:
        print("dry run; pass --write to emit")
        return

    out = args.out or args.pool
    out.write_bytes(bytes(data))
    (args.donors / "appended.json").write_text(json.dumps(added, indent=1))
    print("wrote %s" % out)

    def words(text):
        return "{" + ",".join("0x%su" % w for w in text.split("-")) + "}"

    lines = ["// Generated by tools/append_vent_donors.py.", "#pragma once",
             "namespace ds2 {",
             "// A donor the pool carries that arrives through a vent. `source` is the",
             "// authored entity it was copied from, so the record inherits its model,",
             "// family and orientation; `ventFree` is the arrival contract without the",
             "// vent pointer, which is what the rebind replaces.",
             "struct VentDonorRow { Guid guid, source, vent; const char* ventFree; };",
             "inline constexpr VentDonorRow kVentDonors[] = {"]
    for entry in sorted(added, key=lambda e: e["guid"]):
        lines.append("    {%s,%s,%s,%s}," % (words(entry["guid"]), words(entry["source"]),
                                             words(entry["vent"]), json.dumps(entry["contract"])))
    lines += ["};", "}", ""]
    header = ROOT / "src/generated_vent_donors.h"
    header.write_text("\n".join(lines), encoding="utf-8")
    print("wrote %s with %d donors" % (header, len(added)))


if __name__ == "__main__":
    main()
