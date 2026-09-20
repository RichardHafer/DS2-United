"""Read material references from PC MGAE 50 geometry."""
import re
import struct


def mesh_material_hashes(data):
    if len(data) < 0xE0 or data[:4] != b'MGAE' or struct.unpack_from('<I', data, 4)[0] != 50:
        raise ValueError('Unsupported Mesh header')
    count = struct.unpack_from('<I', data, 0x34)[0]
    offset = struct.unpack_from('<I', data, 0x50)[0]
    if count > 65536 or (count and (offset < 0xE0 or offset + count * 0xC0 > len(data))):
        raise ValueError('Invalid EARSMesh table')
    return sorted({struct.unpack_from('<I', data, offset + i * 0xC0 + 0xC)[0]
                   for i in range(count)} - {0})


def material_hash(data):
    if len(data) < 0x70 or struct.unpack_from('<II', data) != (0x0AA0DEED, 19):
        raise ValueError('Unsupported Material header')
    return struct.unpack_from('<I', data, 8)[0]


def material_texture_paths(data):
    material_hash(data)
    paths = set()
    for match in re.finditer(rb's:/assets/([^\x00]+)_tdf\.xml\x00', data, re.IGNORECASE):
        path = match.group(1).decode('ascii').lower().replace('/', '\\')
        if '..' in path.split('\\'):
            raise ValueError('Invalid material texture path')
        paths.add(path)
    return sorted(paths)
