"""Read DS2 PC SimGroup and loose parameter images without game mutation."""
import struct

def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]

def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]

def sdbm(name):
    result = 0
    for char in name.lower():
        result = (result * 0x1003F + ord(char)) & 0xFFFFFFFF
    return result

def assets(stream):
    current = None
    chunks = []
    for offset, tag, body in stream.chunks():
        inner = stream.tag(body[:4])
        if inner == b"SHDR":
            if current is not None:
                yield current, chunks
            names = body[36:].split(b"\0")
            current = {"name": names[0].decode("latin1"),
                       "path": names[1].decode("latin1"),
                       "type": names[2].decode("latin1"),
                       "resource_id":struct.unpack_from(stream.end + "I",body,16)[0],
                       "size": struct.unpack_from(stream.end + "I", body, 32)[0],
                       "chunk_offset": offset}
            chunks = []
        elif inner in (b"Rpak", b"SDAT", b"LZXD") and current is not None:
            chunks.append(body)
    if current is not None:
        yield current, chunks

def simgroup(data):
    if len(data) < 0x3C or u32(data, 0x24) != 0xCCB51828:
        raise ValueError("Unsupported SimGroup magic")
    if u32(data, 0x28) != 0x00010003:
        raise ValueError("Unsupported PC SimGroup version")
    size = u32(data, 4)
    refs, instances, lists = struct.unpack_from("<III", data, 8)
    nrefs, ninstances, nlists = struct.unpack_from("<HHH", data, 0x14)
    if size > len(data) or refs + 4*nrefs > size or instances + 68*ninstances > size:
        raise ValueError("Invalid SimGroup bounds")
    result = []
    for index in range(ninstances):
        at = instances + index*68
        ref = u32(data, at + 8)
        components = u32(data, at + 0x20)
        if not refs <= ref < refs + 4*nrefs or components + 8 > size:
            raise ValueError("Invalid instance reference")
        count = u32(data, components + 4) if components else 0
        if components + 8 + count*4 > size:
            raise ValueError("Invalid component list")
        component_refs = [u32(data, components + 8 + i*4) for i in range(count)]
        if any(not refs <= r < refs + 4*nrefs for r in component_refs):
            raise ValueError("Invalid component image reference")
        result.append({"index": index, "offset": at,
                       "guid": "-".join(f"{v:08X}" for v in struct.unpack_from("<4I",data,at+12)),
                       "image_id": u32(data, ref),
                       "component_images": [u32(data,r) for r in component_refs],
                       "spawn_flags": u32(data,at+0x2C),
                       "transform_offset": u32(data,at+0x30)})
    image_at = (size + 15) & ~15
    return result, image_at

def loose_images(data, base):
    if base + 32 > len(data) or u32(data, base) != 0x91C8773C:
        raise ValueError("Unsupported parameter image magic")
    size = u32(data, base+4)
    if base+size > len(data):
        raise ValueError("Invalid parameter image bounds")
    types = base + u32(data,base+8)
    ntypes = u16(data,base+12)
    images = base + u32(data,base+16)
    nimages = u32(data,base+20)
    fields = base + u32(data,base+24)
    nfields = u32(data,base+28)
    if max(types+ntypes*8,images+nimages*12,fields+nfields*12) > base+size:
        raise ValueError("Invalid parameter tables")
    result = {}
    for index in range(nimages):
        at = images+index*12
        image_id, type_index, count, values = struct.unpack_from("<IHHI",data,at)
        if type_index >= ntypes or base+values+count*2 > base+size:
            raise ValueError("Invalid parameter image")
        entries = []
        for ordinal in range(count):
            field_index = u16(data,base+values+ordinal*2)
            if field_index >= nfields:
                raise ValueError("Invalid field index")
            offset = fields+field_index*12
            field_id, descriptor, flags, value_index, value = struct.unpack_from("<IHBBI",data,offset)
            entries.append({"field_id":field_id,"descriptor":descriptor,"flags":flags,
                            "value_index":value_index,"value":value,"offset":offset})
        result[image_id] = {"type_id":u32(data,types+type_index*8),"fields":entries}
    return result
