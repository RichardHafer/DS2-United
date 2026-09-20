"""Rename local parameter images while preserving type and component layouts."""
import struct
from ds2_assets import simgroup, loose_images, u16, u32


def remap_images(data, mapping):
    records, base = simgroup(data)
    images = loose_images(data, base)
    if set(mapping) != set(images):
        raise ValueError("Mapping must cover every local image")
    values = list(mapping.values())
    if any(not isinstance(value, int) or not 0 < value <= 0xFFFFFFFF for value in values):
        raise ValueError("Invalid destination image ID")
    if len(set(values)) != len(values) or set(values) & set(images):
        raise ValueError("Image ID collision")
    table = base + u32(data, base + 16)
    count = u32(data, base + 20)
    if len(images) != count:
        raise ValueError("Duplicate source image IDs")
    refs, ref_count = u32(data, 8), u16(data, 20)
    result = bytearray(data)
    edits = []
    for offset in [table + i * 12 for i in range(count)] + [refs + i * 4 for i in range(ref_count)]:
        old = u32(data, offset)
        if old not in mapping:
            raise ValueError("Unresolved parameter image reference")
        struct.pack_into("<I", result, offset, mapping[old])
        edits.append(offset)
    restored_records, restored_base = simgroup(result)
    restored_images = loose_images(result, restored_base)
    if any(restored_images[mapping[old]] != image for old, image in images.items()):
        raise ValueError("Parameter contents changed")
    for original, restored in zip(records, restored_records):
        expected = {**original, "image_id": mapping[original["image_id"]],
                    "component_images": [mapping[x] for x in original["component_images"]]}
        if restored != expected:
            raise ValueError("Instance reference round trip failed")
    return bytes(result), {"images": count, "references": ref_count, "dword_offsets": edits}
