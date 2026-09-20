"""Build an offline private-root TOC referencing stock character streams."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from read_stock_toc import parse

ROOT = Path(__file__).resolve().parents[1] / "analysis"


# The loose files live below randomizer\ and the engine resolves a stream
# request by an exact (case-insensitive) compare of the request name against
# the TOC root's identity name (0x848C00 via 0x748670). A root written without
# the folder is registered, not found, and dropped again when its reference
# count falls to zero at the end of the load (0x74BFBD); the stream then
# arrives raw, without one of its forced children. 15.09.2026: two TOC rebuilds
# used the bare manifest name and every session ran with an empty pool.
STREAM_FOLDER = "randomizer\\"


def pool_root_name(manifest):
    """The root name as the plugin requests it: folder plus manifest root_name."""
    name = manifest["root_name"]
    return name if name.startswith(STREAM_FOLDER) else STREAM_FOLDER + name


def build(name, content_size, shared, forced):
    if not name or len(shared) > 255 or len(forced) > 255:
        raise ValueError("Invalid root or reference count")
    if not 0 <= content_size <= 0xFFFFFFFF:
        raise ValueError("Content size exceeds uint32")
    identities = [(0, name)] + list(shared) + list(forced)
    if len({n for _, n in identities}) != len(identities):
        raise ValueError("Duplicate stream names")
    for value, text in identities:
        if not text or any(ord(c) < 32 or ord(c) > 126 for c in text):
            raise ValueError("Invalid stream name")
        if not 0 <= value <= 0xFFFFFFFF:
            raise ValueError("Invalid stream identity")
    reference_start = 36 + 8 * len(identities)
    data = bytearray(reference_start + 4 * (len(identities) - 1))
    struct.pack_into("<IIHHHH", data, 0, 0x9CBA7B28, 10, 1, len(identities), len(identities) - 1, 0)
    struct.pack_into("<II4BII", data, 16, 36, content_size, len(shared), len(forced), 0, 0, 0, reference_start)
    for i, (value, text) in enumerate(identities):
        struct.pack_into("<II", data, 36 + i * 8, value, len(data))
        data.extend(text.encode("ascii") + b"\0")
        if i:
            struct.pack_into("<I", data, reference_start + (i - 1) * 4, 36 + i * 8)
    data.extend(bytes((-len(data)) % 2048))
    parsed = parse(data)["entries"][0]
    if parsed["content_size"] != content_size or len(parsed["dependencies"]) != len(forced):
        raise ValueError("TOC round-trip mismatch")
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tool-dir", required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str
    from ds2_assets import assets
    stream = ROOT / "research_stream/ds2_randomizer_pool.str"
    source = ds2str.StreamSet(stream)
    if not source.pc:
        raise ValueError("Expected a PC template stream")
    content_size = sum(meta["size"] for meta, _ in assets(source))
    stock = json.loads((ROOT / "stock_toc_dependencies.json").read_text())
    identities = {}
    for toc in stock["tocs"]:
        for entry in toc["entries"]:
            identity = entry["identity"]
            identities.setdefault(identity["name"], set()).add(int(identity["hash"], 16))
    def original(name):
        values = identities.get(name, set())
        if len(values) != 1:
            raise ValueError(f"Missing or conflicting stock identity: {name}")
        return next(iter(values)), name
    packages = json.loads((ROOT / "native_character_streams.json").read_text())["packages"]
    forced = [original(p["toc_name"]) for p in packages]
    shared = [original(r"global_assets\global_assets")]
    name = "ds2_randomizer_pool"
    if name in identities:
        raise ValueError("Root name collides with stock TOC")
    data = build(name, content_size, shared, forced)
    output = stream.with_suffix(".toc")
    output.write_bytes(data)
    report = {"runtime_ready": False, "scope": "Research TOC; native registration and dispatch untested",
              "root_name": name, "root_identity": 0, "content_size": content_size,
              "stream_sha256": hashlib.sha256(stream.read_bytes()).hexdigest(),
              "toc_sha256": hashlib.sha256(data).hexdigest(), "toc_bytes": len(data),
              "shared": shared, "forced": forced,
              "warning": "Full pool requests all catalogued character packages; not a release memory budget"}
    output.with_suffix(".toc.json").write_text(json.dumps(report, indent=2))
    print(f"Research TOC: {len(data)} bytes; content: {content_size}; shared: {len(shared)}; forced: {len(forced)}")


if __name__ == "__main__":
    main()
