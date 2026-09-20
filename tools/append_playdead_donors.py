"""Put authored play-dead arrivals into the resident pool.

The third arrival kind. The pool carried 34 enemies that walk in and 74 that
climb out of a vent, and nothing that lies on the floor pretending to be a
corpse, so a play-dead slasher had no candidate at all and stayed the enemy the
level authored.

Appending is the same operation the vent donors went through and is safe in a
way that rewriting is not: the container's existing chunks keep their offsets and
only the trailing FILL padding is replaced. The TOC lists the stream's
dependencies rather than its assets, so it does not have to change either.

What the runtime does with these is decided by `compatibleScripted`, not here:
same family, same context and the same scriptedKey. So the row carries that key
and the record keeps it, which is the opposite of a vent donor, where the
contract is deliberately cleared because the vent pointer gets rebound.
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
sys.path.insert(0, r"D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools")

from stream_framing import frame_stream  # noqa: E402
from remap_pool_guids import remap  # noqa: E402
from append_vent_donors import MAX_CHUNK, chunk, new_guid, pool_streams, trailing_padding  # noqa: E402


def refresh_manifest(pool):
    """The pool's digest is embedded and checked at build time, so a changed
    stream has to update the manifest in the same step or the next build stops."""
    import hashlib
    manifest_path = pool.with_suffix(".toc.json")
    manifest = json.loads(manifest_path.read_text())
    manifest["stream_sha256"] = hashlib.sha256(pool.read_bytes()).hexdigest()
    manifest_path.write_text(json.dumps(manifest, indent=1))
    print("manifest updated: %s" % manifest_path.name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pool", type=Path,
                        default=ROOT / "analysis/runtime_stream/ds2_randomizer_pool.str")
    parser.add_argument("--donors", type=Path, default=ROOT / "analysis/playdead_pool")
    parser.add_argument("--plan", type=Path, default=ROOT / "analysis/playdead_pool_plan.json")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()

    resident = pool_streams()
    report = json.loads((args.donors / "report.json").read_text())
    plan = {entry["guid"]: entry for entry in json.loads(args.plan.read_text())}

    chosen = [e for e in report["entries"] if e["package"] in resident]
    print("donors offered: %d, usable without new resources: %d"
          % (len(report["entries"]), len(chosen)))

    data = bytearray(args.pool.read_bytes())
    cut = trailing_padding(data)
    print("pool: %d bytes, real content ends at %d" % (len(data), cut))
    del data[cut:]

    added = []
    for entry in chosen:
        payload = (args.donors / entry["file"]).read_bytes()
        from ds2_assets import simgroup
        records, _ = simgroup(payload)
        mapping = {r["guid"]: new_guid(r["guid"]) for r in records}
        payload, stats = remap(payload, mapping)
        # The SHDR of a SimGroup asset: 36 bytes, magic at +12, size at +32.
        header = bytearray(b"RDHS" + bytes(8) + struct.pack("<I", 0xCCB51828) + bytes(16) + bytes(4))
        struct.pack_into("<I", header, 32, len(payload))
        name = "{%s}" % str(uuid.uuid5(uuid.NAMESPACE_URL, "ds2playdead/" + entry["guid"])).upper()
        path = "playdead_donors/%s/%s.simgroup.bin" % (entry["package"], entry["contract"][:12])
        body = bytearray(header)
        body.extend((name + "\0" + path + "\0SimGroup\0").encode("latin1"))
        data.extend(chunk(bytes(body)))
        for start in range(0, len(payload), MAX_CHUNK - 12):
            data.extend(chunk(b"TADS" + payload[start:start + MAX_CHUNK - 12]))
        row = plan[entry["guid"]]
        added.append({"guid": new_guid(entry["guid"]), "source": entry["guid"],
                      "contract": entry["contract"], "family": entry["family"],
                      "package": entry["package"], "context": row["context"],
                      "script_name": row["script_name"], "name": name, "path": path,
                      "bytes": len(payload), "identities": stats["identities"],
                      "references": stats["references"],
                      "sha256": hashlib.sha256(payload).hexdigest()})

    data = bytearray(frame_stream(bytes(data)))
    print("appended %d donors, pool now %d bytes (%.1f MiB)"
          % (len(added), len(data), len(data) / 1048576))
    packages = collections.Counter(a["package"] for a in added)
    print("play-dead models in the pool: %s" % dict(packages))
    if not args.write:
        print("dry run; pass --write to emit")
        return

    out = args.out or args.pool
    out.write_bytes(bytes(data))
    refresh_manifest(out)
    (args.donors / "appended.json").write_text(json.dumps(added, indent=1))
    print("wrote %s" % out)

    def words(text):
        return "{" + ",".join("0x%su" % w for w in text.split("-")) + "}"

    lines = ["// Generated by tools/append_playdead_donors.py.", "#pragma once",
             "namespace ds2 {",
             "// A donor the pool carries that arrives by playing dead. `source` is the",
             "// authored entity it was copied from, so the record inherits its model and",
             "// family; `scripted` is the authored spawning contract, and unlike a vent",
             "// donor it is kept rather than cleared, it is the whole reason two corpses",
             "// may swap at all.",
             "struct PlaydeadDonorRow { Guid guid, source; const char* scripted; };",
             "inline constexpr PlaydeadDonorRow kPlaydeadDonors[] = {"]
    for entry in sorted(added, key=lambda e: e["guid"]):
        lines.append("    {%s,%s,%s},  // %s"
                     % (words(entry["guid"]), words(entry["source"]),
                        json.dumps(entry["contract"]), entry["package"]))
    lines += ["};", "}", ""]
    header_path = ROOT / "src/generated_playdead_donors.h"
    header_path.write_text("\n".join(lines), encoding="utf-8")
    print("wrote %s with %d donors" % (header_path, len(added)))


if __name__ == "__main__":
    main()
