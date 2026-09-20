"""Wrap namespaced templates in a standalone PC research stream."""
import argparse
import hashlib
import json
import struct
import sys
import uuid
from pathlib import Path
from ds2_assets import assets
from stream_framing import frame_stream

ROOT = Path(__file__).resolve().parents[1]
MAX_CHUNK = 131072


def chunk(body, tag=b'COHS'):
    if len(body) + 8 > MAX_CHUNK:
        raise ValueError('Chunk too large')
    return tag + struct.pack('<I', len(body) + 8) + body


def pad_stream(output):
    # Loose streams are read in full 128 KiB blocks, including the last read.
    size = (-len(output)) % MAX_CHUNK
    if size == 0:
        return
    if size < 8:
        size += MAX_CHUNK
    if size > MAX_CHUNK:
        output.extend(chunk(b'', b'LLIF'))
        size -= 8
    output.extend(chunk(bytes(size - 8), b'LLIF'))


def package(entries):
    output = bytearray(b'3slo\x0c\x00\x00\x00\x02\x00\x03\x01')
    names, paths = set(), set()
    for header, name, path, payload in entries:
        if len(header) != 36 or header[:4] != b'RDHS' or header[12:16] != struct.pack('<I', 0xCCB51828):
            raise ValueError('Unsupported SimGroup descriptor')
        if header[16:32] != bytes(16):
            raise ValueError('Unexpected SimGroup resource GUID')
        if name in names or path in paths or '\0' in name + path:
            raise ValueError('Duplicate or invalid asset name')
        names.add(name)
        paths.add(path)
        body = bytearray(header)
        struct.pack_into('<I', body, 32, len(payload))
        body.extend((name + '\0' + path + '\0SimGroup\0').encode('latin1'))
        output.extend(chunk(body))
        for start in range(0, len(payload), MAX_CHUNK - 12):
            output.extend(chunk(b'TADS' + payload[start:start + MAX_CHUNK - 12]))
    return frame_stream(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--tool-dir', required=True)
    parser.add_argument('--entities', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str
    catalog = [json.loads(line) for line in (ROOT / 'analysis/catalog/assets.jsonl').read_text(encoding='utf-8').splitlines()]
    index = {(a['archive'], a['path']): a for a in catalog if a['type'] == 'SimGroup'}
    reserved = {a['name'].lower() for a in catalog}
    sources = json.loads((ROOT / 'analysis/dependency_pool/report.json').read_text(encoding='utf-8'))
    report = json.loads((args.entities / 'report.json').read_text(encoding='utf-8'))
    if sources['graph_sha256'] != report['graph_sha256']:
        raise ValueError('Dependency graph mismatch')
    files = {g['file']: g for g in report['groups']}
    if set(files) != {g['file'] for g in sources['groups']}:
        raise ValueError('Group set mismatch')
    entries, manifest = [], []
    for group in sources['groups']:
        meta = index[group['archive'], group['asset']]
        with (args.work / group['archive']).open('rb') as stream:
            stream.seek(meta['chunk_offset'])
            raw = stream.read(44)
        if raw[:4] != b'COHS' or struct.unpack_from('<I', raw, 40)[0] != meta['size']:
            raise ValueError('Source SHDR mismatch')
        name = '{' + str(uuid.uuid5(uuid.NAMESPACE_URL, 'DS2Randomizer:pool:v1:' + group['file'])).upper() + '}'
        if name.lower() in reserved:
            raise ValueError('SimGroup name collision')
        reserved.add(name.lower())
        path = 'randomizer\\pool\\' + group['file']
        data = (args.entities / group['file']).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != files[group['file']]['sha256']:
            raise ValueError('Entity payload checksum mismatch')
        entries.append((raw[8:44], name, path, data))
        manifest.append({'name': name, 'path': path, 'file': group['file'], 'sha256': digest})
    data = package(entries)
    reader = ds2str.StreamSet.__new__(ds2str.StreamSet)
    reader.data, reader.pc, reader.end, reader.hdrlen = data, True, '<', 12
    decoded = list(assets(reader))
    if len(decoded) != len(entries):
        raise ValueError('Packaged asset count mismatch')
    for (meta, chunks), (_, name, path, payload) in zip(decoded, entries):
        if meta['name'] != name or meta['path'] != path or reader.asset_payload(chunks, meta['size']) != payload:
            raise ValueError('Stream round trip failed')
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        raise ValueError('Output directory must be empty')
    (args.output / 'ds2_randomizer_pool.str').write_bytes(data)
    (args.output / 'report.json').write_text(json.dumps({'runtime_ready': False, 'assets': manifest,
        'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
        'remaining': ['native stream registration', 'external entity residency', 'model and audio dependencies', 'unload lifecycle']}, indent=2), encoding='utf-8')
    print(f'Packed and decoded {len(entries)} groups; {len(data)} bytes; not installed')


if __name__ == '__main__':
    main()
