"""Combine audited supplemental resources and private templates without changing payloads."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from build_research_toc import build, pool_root_name
from ds2_assets import assets
from stream_framing import frame_stream

ROOT = Path(__file__).resolve().parents[1] / "analysis"


def records(data):
    if data[:12] != b'3slo\x0c\x00\x00\x00\x02\x00\x03\x01':
        raise ValueError("Unexpected PC stream header")
    offset = 12
    output = bytearray()
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError("Truncated chunk")
        tag, size = struct.unpack_from("<4sI", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError("Invalid chunk size")
        if tag == b'COHS':
            output.extend(data[offset:offset + size])
        elif tag == b'LLIF':
            pass
        else:
            raise ValueError("Unexpected chunk")
        offset += size
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool-dir", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--supplement", type=Path, default=ROOT / "minimal_resource_stream")
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str
    def inventory(data):
        stream = ds2str.StreamSet.__new__(ds2str.StreamSet)
        stream.data, stream.pc, stream.end, stream.hdrlen = bytes(data), True, '<', 12
        return [(meta["name"], meta["path"], meta["type"], meta["size"],
                 hashlib.sha256(stream.asset_payload(parts, meta["size"])).hexdigest())
                for meta, parts in assets(stream)]
    original = json.loads((ROOT / "research_stream/ds2_randomizer_pool.toc.json").read_text())
    supplement = json.loads((args.supplement / "report.json").read_text())
    inputs = [(args.supplement / "ds2_randomizer_supplement.str", supplement["sha256"]),
              (ROOT / "research_stream/ds2_randomizer_pool.str", original["stream_sha256"])]
    output = bytearray(b'3slo\x0c\x00\x00\x00\x02\x00\x03\x01')
    expected = []
    for path, digest in inputs:
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"Source fingerprint mismatch: {path}")
        expected.extend(inventory(data))
        output.extend(records(data))
    if len({(r[1], r[2]) for r in expected}) != len(expected):
        raise ValueError("Duplicate asset path and type")
    output = frame_stream(output)
    if inventory(output) != expected:
        raise ValueError("Combined stream round-trip mismatch")
    shared = original["shared"] + [[0, "global_assets\\global_assets_global"], [0, "text_assets\\text_assets_global"]]
    content_size = sum(r[3] for r in expected)
    toc = build(pool_root_name(original), content_size, shared, original["forced"])
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        raise ValueError("Output directory must be empty")
    (args.output / "ds2_randomizer_pool.str").write_bytes(output)
    (args.output / "ds2_randomizer_pool.toc").write_bytes(toc)
    report = {**original, "content_size": content_size, "shared": shared,
              "stream_sha256": hashlib.sha256(output).hexdigest(),
              "toc_sha256": hashlib.sha256(toc).hexdigest(), "toc_bytes": len(toc),
              "assets": expected, "runtime_ready": False}
    (args.output / "ds2_randomizer_pool.toc.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Combined {len(expected)} assets; {len(output)} stream bytes; payloads unchanged")


if __name__ == "__main__":
    main()
