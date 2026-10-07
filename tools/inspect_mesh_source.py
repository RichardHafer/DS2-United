"""Inspect original mesh framing around the supplemental projectile resources."""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
from ds2_paths import MODDING_TOOLS, WORK
sys.path.insert(0, str(MODDING_TOOLS))
from ds2str import refpack_decompress
audit = json.loads((ROOT / 'analysis/resource_supplement_audit.json').read_bytes())
for resource in audit['resources']:
    meta = resource['variants'][0][0]
    if 'zombiepod.geo' not in meta['path']:
        continue
    print(resource['guid'], meta)
    with (WORK / meta['archive']).open('rb') as stream:
        stream.seek(meta['chunk_offset'])
        for _ in range(10):
            at = stream.tell()
            header = stream.read(12)
            size = struct.unpack_from('<I', header, 4)[0]
            print(hex(at), size, header.hex())
            if header[8:] == b'kapR':
                body = stream.read(size - 12)
                count = struct.unpack_from('<I', body)[0]
                decoded = refpack_decompress(body[4:4 + count])
                print('decoded', len(decoded), 'header', decoded[:40].hex())
            if size < 12:
                break
            stream.seek(at + size)
