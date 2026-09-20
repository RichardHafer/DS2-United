"""Read stock PC TOC descriptors without applying pointer relocations."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def parse(data):
    def require(offset, size):
        if offset < 0 or size < 0 or offset + size > len(data):
            raise ValueError("TOC field outside file")

    def word(offset):
        require(offset, 4)
        return struct.unpack_from("<I", data, offset)[0]

    def identity(offset):
        require(offset, 8)
        name_offset = word(offset + 4)
        require(name_offset, 1)
        end = data.find(b"\0", name_offset)
        if end < 0:
            raise ValueError("Unterminated TOC name")
        name = data[name_offset:end].decode("ascii")
        if not name or any(ord(c) < 32 or ord(c) > 126 for c in name):
            raise ValueError("Invalid TOC name")
        return {"hash": f"{word(offset):08X}", "name": name, "offset": offset}

    require(0, 16)
    if word(0) != 0x9CBA7B28 or word(4) != 10:
        raise ValueError("Unsupported PC TOC header")
    count, identity_count = struct.unpack_from("<HH", data, 8)
    reference_count_total = struct.unpack_from("<H", data, 12)[0]
    require(16, count * 20)
    identity_start = 16 + count * 20
    reference_start = identity_start + identity_count * 8
    require(identity_start, identity_count * 8)
    require(reference_start, reference_count_total * 4)
    valid_identities = set(range(identity_start, reference_start, 8))
    def checked_identity(offset):
        if offset not in valid_identities:
            raise ValueError("Reference does not address an identity table entry")
        return identity(offset)

    # The native relocation routine visits every slot, including unused ones.
    for offset in valid_identities:
        identity(offset)
    for offset in range(reference_start, reference_start + reference_count_total * 4, 4):
        if word(offset):
            checked_identity(word(offset))
    entries = []
    for i in range(count):
        offset = 16 + i * 20
        leading, dependent, flags = struct.unpack_from("<BBB", data, offset + 8)
        reference_count = leading + dependent + (flags & 1)
        references = word(offset + 16)
        require(references, reference_count * 4)
        if reference_count and (references < reference_start or
                                (references - reference_start) % 4 or
                                references + reference_count * 4 > reference_start + reference_count_total * 4):
            raise ValueError("Descriptor references outside reference table")
        ids = [checked_identity(word(references + n * 4)) for n in range(reference_count)]
        start = leading + (flags & 1)
        entries.append({"index": i, "identity": checked_identity(word(offset)),
                        "content_size": word(offset + 4), "dlc_index": word(offset + 12),
                        "leading_count": leading, "flags": flags,
                        "leading_references": ids[:start], "dependencies": ids[start:]})
    hashes = [int(entry["identity"]["hash"], 16) for entry in entries]
    if hashes != sorted(hashes):
        raise ValueError("Descriptor identities violate native binary-search order")
    return {"descriptor_count": count, "header_identity_count": identity_count,
            "header_reference_count": reference_count_total, "entries": entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    reports, errors = [], []
    for path in sorted(args.work.rglob("*.toc")):
        try:
            data = path.read_bytes()
            reports.append({"file": str(path.relative_to(args.work)),
                            "sha256": hashlib.sha256(data).hexdigest(), **parse(data)})
        except (ValueError, OSError) as error:
            errors.append({"file": str(path.relative_to(args.work)), "error": str(error)})
    output = Path(__file__).resolve().parents[1] / "analysis/stock_toc_dependencies.json"
    output.write_text(json.dumps({"scope": "Offline PC TOC decoding; registration not implemented",
                                  "tocs": reports, "errors": errors}, indent=2))
    print(f"Parsed TOCs: {len(reports)}; errors: {len(errors)}")
    for error in errors[:8]:
        print(error)


if __name__ == "__main__":
    main()
