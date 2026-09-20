"""Rewrite entity identities and typed references in research SimGroups."""
import struct
from ds2_assets import simgroup, loose_images, u32


def guid_bytes(guid):
    words = [int(word, 16) for word in guid.split("-")]
    if len(words) != 4:
        raise ValueError("Invalid GUID")
    return struct.pack("<4I", *words)


def guid_hash(guid):
    result = 0
    for word in struct.unpack("<4I", guid_bytes(guid)):
        result = (result * 33 + word) & 0xFFFFFFFF
    return result


def remap(data, mapping):
    records, base = simgroup(data)
    images = loose_images(data, base)
    destinations = list(mapping.values())
    if len({guid_hash(g) for g in destinations}) != len(destinations):
        raise ValueError("Destination GUID hash collision")
    edits = {}

    def patch(offset, payload):
        for index, value in enumerate(payload):
            at = offset + index
            if at in edits and edits[at] != value:
                raise ValueError("Conflicting GUID patches")
            edits[at] = value

    identities = references = 0
    for record in records:
        old = record["guid"]
        if u32(data, record["offset"] + 0x1C) != guid_hash(old):
            raise ValueError("Source identity hash mismatch")
        if old in mapping:
            new = mapping[old]
            patch(record["offset"] + 12, guid_bytes(new))
            patch(record["offset"] + 0x1C, struct.pack("<I", guid_hash(new)))
            identities += 1
    visited = set()
    image_end = base + u32(data, base + 4)
    for image in images.values():
        for field in image["fields"]:
            if field["flags"] != 12 or field["offset"] in visited:
                continue
            visited.add(field["offset"])
            relative = struct.unpack("<i", struct.pack("<I", field["value"]))[0]
            offset = field["offset"] + 8 + relative
            if offset < base or offset + 16 > image_end:
                raise ValueError("GUID field exceeds parameter image bounds")
            old = "-".join(f"{word:08X}" for word in struct.unpack_from("<4I", data, offset))
            if old in mapping:
                patch(offset, guid_bytes(mapping[old]))
                references += 1
    result = bytearray(data)
    for offset, value in edits.items():
        result[offset] = value
    restored, _ = simgroup(result)
    for source, target in zip(records, restored):
        if target["guid"] != mapping.get(source["guid"], source["guid"]):
            raise ValueError("Identity round trip failed")
    return bytes(result), {"identities": identities, "references": references,
                           "edited_offsets": sorted(edits)}
