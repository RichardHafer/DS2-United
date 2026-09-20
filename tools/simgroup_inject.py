"""Add a component image to one record of a SimGroup payload.

A SimGroup payload has two parts (tools/ds2_assets.py reads both):

    header (0x3C bytes): +4 size of this part, +8/+0xC/+0x10 offsets of the
    reference table, the instance table and the list table, +0x14 their
    counts as u16
    reference table: one u32 per entry, the parameter image id
    instances (68 bytes): +8 offset of the record's own reference entry,
    +12 GUID, +0x1C GUID hash, +0x20 offset of its component list block
    list table: u32 offsets of the list blocks
    list block: u32 head, u32 count, then count u32 offsets into the
    reference table (one per component image)

    parameter block at (size + 15) & ~15: magic 0x91C8773C, +4 its size,
    +8/+12 type table (8 bytes per type: type id, word) and count, +16/+20
    image table (12 bytes: image id, type index u16, field count u16,
    offset of its u16 field index array) and count, +24/+28 field rows
    (12 bytes: field id, descriptor u16, flags u8, value index u8, value)
    and count. A flags 12 value is the distance from the value dword to the
    16 byte GUID it names.

Every table is contiguous, so a bigger table is rebuilt at the end of its
part and the header pointed at it; the old bytes stay as dead weight. The
list block offsets into the reference table are rewritten for the moved
table, everything else keeps its offsets. Verified by re-reading the result
with ds2_assets and by the catalog's field decoder (14.09.2026).
"""
import struct

from ds2_assets import simgroup, loose_images, u32


def guid_bytes(text):
    return struct.pack("<4I", *(int(w, 16) for w in text.split("-")))


def add_component(data, record_guid, type_entry, image_id, fields):
    """Return the payload with one more component image on `record_guid`.

    type_entry: (type_id, word) as in the type table of the source record.
    fields: list of (field_id, flags, value) where a flags 12 value is a
    GUID text and every other value a raw dword."""
    data = bytes(data)
    records, base = simgroup(data)
    record = [r for r in records if r["guid"] == record_guid]
    if len(record) != 1:
        raise ValueError("record %s not found once in the group" % record_guid)
    record = record[0]
    size = u32(data, 4)
    refs, instances, lists = struct.unpack_from("<III", data, 8)
    nrefs, ninstances, nlists = struct.unpack_from("<HHH", data, 0x14)
    if any(u32(data, refs + i * 4) == image_id for i in range(nrefs)):
        raise ValueError("image id %08X already in the group" % image_id)

    # --- part one: reference table plus one, component list plus one
    part = bytearray(data[:size])
    while len(part) % 4:
        part.append(0)
    new_refs = len(part)
    part.extend(data[refs:refs + nrefs * 4])
    part.extend(struct.pack("<I", image_id))
    new_ref_entry = new_refs + nrefs * 4
    struct.pack_into("<I", part, 8, new_refs)
    struct.pack_into("<H", part, 0x14, nrefs + 1)
    # every list block names reference entries by offset: move them along
    delta = new_refs - refs
    for i in range(nlists):
        block = u32(data, lists + i * 4)
        count = u32(data, block + 4)
        for k in range(count):
            at = block + 8 + k * 4
            struct.pack_into("<I", part, at, u32(data, at) + delta)
    for r in records:
        struct.pack_into("<I", part, r["offset"] + 8, u32(data, r["offset"] + 8) + delta)
    # the record's list block, one entry longer, at the end
    block = u32(data, record["offset"] + 0x20)
    count = u32(data, block + 4)
    new_block = len(part)
    part.extend(struct.pack("<II", u32(data, block), count + 1))
    for k in range(count):
        part.extend(struct.pack("<I", u32(data, block + 8 + k * 4) + delta))
    part.extend(struct.pack("<I", new_ref_entry))
    struct.pack_into("<I", part, record["offset"] + 0x20, new_block)
    slot = [i for i in range(nlists) if u32(data, lists + i * 4) == block]
    if len(slot) == 1:
        struct.pack_into("<I", part, lists + slot[0] * 4, new_block)
    struct.pack_into("<I", part, 4, len(part))
    while len(part) % 16:
        part.append(0)

    # --- part two: the parameter block with one type, one image, the fields
    psize = u32(data, base + 4)
    block = bytearray(data[base:base + psize])
    types_off = u32(block, 8); ntypes = struct.unpack_from("<H", block, 12)[0]
    images_off = u32(block, 16); nimages = u32(block, 20)
    fields_off = u32(block, 24); nfields = u32(block, 28)
    type_index = None
    for i in range(ntypes):
        if struct.unpack_from("<II", block, types_off + i * 8) == tuple(type_entry):
            type_index = i
    if type_index is None:
        # the type table is sorted by type id in every authored group (the
        # engine looks types up by bisection; an appended entry crashed the
        # ItemDropListComponent construction, 14.09. 21:02): insert in order
        # and shift the type index of every image behind the insert point
        entries = [struct.unpack_from("<II", block, types_off + i * 8) for i in range(ntypes)]
        insert_at = sum(1 for t in entries if t[0] < type_entry[0])
        entries.insert(insert_at, tuple(type_entry))
        while len(block) % 4:
            block.append(0)
        new_types = len(block)
        for t in entries:
            block.extend(struct.pack("<II", *t))
        struct.pack_into("<I", block, 8, new_types)
        struct.pack_into("<H", block, 12, ntypes + 1)
        for i in range(nimages):
            at = images_off + i * 12 + 4
            ti = struct.unpack_from("<H", block, at)[0]
            if ti >= insert_at:
                struct.pack_into("<H", block, at, ti + 1)
        type_index = insert_at
    while len(block) % 4:
        block.append(0)
    # field rows plus the new ones
    new_fields = len(block)
    block.extend(block[fields_off:fields_off + nfields * 12])
    # a flags 12 value is relative to its row: the moved rows keep naming
    # the same GUID bytes (which stay where they were)
    shift = new_fields - fields_off
    for i in range(nfields):
        row = new_fields + i * 12
        if block[row + 6] == 12:
            value = struct.unpack_from("<i", block, row + 8)[0]
            struct.pack_into("<i", block, row + 8, value - shift)
    new_rows = []
    guid_slots = []
    for field_id, flags, value in fields:
        row = len(block)
        if flags == 12:
            block.extend(struct.pack("<IHBBI", field_id, 0xFFFF, 12, 0xFF, 0))
            guid_slots.append((row, value))
        else:
            block.extend(struct.pack("<IHBBI", field_id, 0xFFFF, flags, 0xFF, value & 0xFFFFFFFF))
        new_rows.append(row)
    struct.pack_into("<I", block, 24, new_fields)
    struct.pack_into("<I", block, 28, nfields + len(fields))
    # the GUIDs the new rows name, right behind the rows
    for row, text in guid_slots:
        while len(block) % 4:
            block.append(0)
        struct.pack_into("<I", block, row + 8, len(block) - (row + 8))
        block.extend(guid_bytes(text))
    # the image's field index array, then the image table plus one
    while len(block) % 4:
        block.append(0)
    values = len(block)
    for row in new_rows:
        block.extend(struct.pack("<H", (row - new_fields) // 12))
    while len(block) % 4:
        block.append(0)
    new_images = len(block)
    block.extend(block[images_off:images_off + nimages * 12])
    block.extend(struct.pack("<IHHI", image_id, type_index, len(fields), values))
    struct.pack_into("<I", block, 16, new_images)
    struct.pack_into("<I", block, 20, nimages + 1)
    struct.pack_into("<I", block, 4, len(block))

    out = bytes(part) + bytes(block) + data[base + psize:]
    # read back: the group parses, the record lists the image, the image decodes
    records2, base2 = simgroup(out)
    r2 = [r for r in records2 if r["guid"] == record_guid][0]
    if image_id not in r2["component_images"] or len(r2["component_images"]) != len(record["component_images"]) + 1:
        raise ValueError("component not on the record after the edit")
    decoded = loose_images(out, base2)[image_id]
    if decoded["type_id"] != type_entry[0] or len(decoded["fields"]) != len(fields):
        raise ValueError("image did not decode as written")
    for (field_id, flags, value), entry in zip(fields, decoded["fields"]):
        if entry["field_id"] != field_id or entry["flags"] != flags:
            raise ValueError("field row mismatch")
        if flags == 12:
            at = entry["offset"] + 8 + entry["value"]
            if out[at:at + 16] != guid_bytes(value):
                raise ValueError("GUID reference does not resolve")
        elif entry["value"] != (value & 0xFFFFFFFF):
            raise ValueError("value mismatch")
    before = loose_images(data, base)
    after = loose_images(out, base2)
    for iid, img in before.items():
        if img["type_id"] != after[iid]["type_id"] or len(img["fields"]) != len(after[iid]["fields"]):
            raise ValueError("image %08X changed its type or field count" % iid)
    for r1, r3 in zip(records, records2):
        if r1["guid"] != r3["guid"] or (r1["guid"] != record_guid and r1["component_images"] != r3["component_images"]):
            raise ValueError("another record changed")
    return out
