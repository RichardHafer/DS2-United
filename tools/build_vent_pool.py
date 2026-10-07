"""Isolate authored vent arrivals so the global pool can serve vents.

The pool's 34 donors are all direct arrivals: they walk in, and none of them
carries a `<Species>VentComponent`, which is what a vent climb-out needs. So a
vent spawn can only ever be replaced by an entity the *level* happens to carry,
and in chapter 1 that means slashers and nothing else.

There is very little to add. A vent arrival's contract is the mode, the
activation flags and a pointer to its vent, and the runtime rebinds the pointer,
so what matters is only the first two. Across the whole game that leaves **31**
distinct contracts, and the largest covers 290 of 402 vent spawns with 28 models
across eight species behind it. `tools/verify_vent_coverage.py` prints the
breakdown; `analysis/vent_pool_plan.json` is the selection this tool consumes.

Isolation is the same operation `build_isolated_pool.py` performs, unchanged: an
authored vent arrival carries spawn flags of zero, because the arrival mode lives
in the DSNPCSpawningComponent and not in the instance record.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from build_isolated_pool import isolate_many  # noqa: E402
from ds2_assets import assets, simgroup  # noqa: E402
from ds2_paths import MODDING_TOOLS, WORK  # noqa: E402


def isolate_vent(data, index):
    """One vent instance, isolated exactly the way a ground donor is.

    `isolate_many` refuses an instance with nonzero spawn flags, and the probe
    that led here shows why that is no obstacle: an authored vent arrival carries
    spawn flags of zero. The arrival mode lives in the DSNPCSpawningComponent,
    not in the instance record, so the same isolation applies unchanged.
    """
    records, _ = simgroup(data)
    if not 0 <= index < len(records):
        raise ValueError("Instance index out of range")
    result, selected, orphans = isolate_many(data, [index])
    check = simgroup(result)[0]
    if len(check) != 1 or check[0]["guid"] != records[index]["guid"]:
        raise ValueError("Isolation did not keep the instance")
    return result, selected[0], orphans


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--work", type=Path,
                        default=WORK)
    parser.add_argument("--tool-dir", default=str(MODDING_TOOLS))
    parser.add_argument("--plan", type=Path, default=ROOT / "analysis/vent_pool_plan.json")
    parser.add_argument("--output", type=Path, default=ROOT / "analysis/vent_pool")
    parser.add_argument("--limit", type=int, default=0, help="stop after N donors, for a probe")
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str  # noqa: E402

    plan = json.loads(args.plan.read_text())
    if args.limit:
        plan = plan[:args.limit]
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"entries": [], "skipped": []}
    for donor in plan:
        name = "%s_%s" % (donor["package"], donor["contract"][:8])
        try:
            stream = ds2str.StreamSet(str(args.work / donor["archive"]))
            if not stream.pc:
                raise ValueError("Not a PC stream")
            matches = [(meta, chunks) for meta, chunks in assets(stream)
                       if meta["type"] == "SimGroup" and meta["path"] == donor["asset"]]
            if len(matches) != 1:
                raise ValueError("Source asset is not unique (%d matches)" % len(matches))
            meta, chunks = matches[0]
            data = stream.asset_payload(chunks, meta["size"])
            candidate, instance, orphans = isolate_vent(data, int(donor["entity_index"]))
            if instance["guid"] != donor["guid"]:
                raise ValueError("Catalog does not match source archive")
            path = args.output / (name + ".simgroup.bin")
            path.write_bytes(candidate)
            report["entries"].append({
                "file": path.name, "package": donor["package"], "family": donor["family"],
                "contract": donor["contract"], "guid": donor["guid"],
                "spawn_flags": instance["spawn_flags"], "bytes": len(candidate),
                "retained_unreferenced_images": orphans,
                "source_archive": donor["archive"], "source_asset": donor["asset"],
                "sha256": hashlib.sha256(candidate).hexdigest(),
                "spawns_of_contract": donor["spawns_of_contract"]})
        except (ValueError, OSError, struct.error, KeyError) as error:
            report["skipped"].append({"donor": name, "reason": str(error)})
    (args.output / "report.json").write_text(json.dumps(report, indent=2))
    print("isolated: %d, skipped: %d" % (len(report["entries"]), len(report["skipped"])))
    for entry in report["skipped"][:8]:
        print("   skipped %s: %s" % (entry["donor"], entry["reason"]))
    families = sorted({e["family"] for e in report["entries"]})
    print("families available: %s" % ", ".join(families))
    print("no game file was written; output in %s" % args.output)


if __name__ == "__main__":
    main()
