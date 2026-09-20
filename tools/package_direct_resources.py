"""Package verified direct resources without editing their identities."""
import argparse
import hashlib
import json
import sys
from pathlib import Path
from ds2_assets import assets, u32
from inventory_parameter_ids import read_asset
from package_research_pool import chunk, MAX_CHUNK
from resolve_pool_resources import header_guid
from stream_framing import frame_stream


def original_payload_chunks(stream, size, decompress):
    encoded = bytearray()
    decoded = bytearray()
    while len(decoded) < size:
        header = stream.read(8)
        if len(header) != 8 or header[:4] != b'COHS':
            raise ValueError('Missing original resource chunk')
        length = u32(header, 4)
        if not 12 <= length <= 131072:
            raise ValueError('Invalid original resource chunk size')
        body = stream.read(length - 8)
        if len(body) != length - 8:
            raise ValueError('Truncated original resource chunk')
        if body[:4] == b'TADS':
            decoded.extend(body[4:])
        elif body[:4] == b'kapR' and len(body) >= 8:
            count = u32(body, 4)
            if count > len(body) - 8:
                raise ValueError('Truncated original compressed resource')
            decoded.extend(decompress(body[8:8 + count]))
        else:
            raise ValueError('Unsupported original resource encoding')
        encoded.extend(header + body)
    # Some texture headers include zero padding to a four-byte boundary.
    if len(decoded) != size and (len(decoded) != ((size + 3) & ~3) or any(decoded[size:])):
        raise ValueError('Original resource payload size mismatch')
    return bytes(encoded), bytes(decoded[:size])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--tool-dir', required=True)
    parser.add_argument('--audit', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--filename', default='ds2_randomizer_direct_resources.str')
    args = parser.parse_args()
    if Path(args.filename).name != args.filename or not args.filename.endswith('.str'):
        raise ValueError('Invalid output filename')
    sys.path.insert(0, args.tool_dir)
    import ds2str
    audit_bytes = args.audit.read_bytes()
    audit = json.loads(audit_bytes)
    if audit['errors'] or any(r['status'] != 'identical' for r in audit['resources']):
        raise ValueError('Complete conflict-free payload audit required')
    output = bytearray(b'3slo\x0c\x00\x00\x00\x02\x00\x03\x01')
    manifest = []
    for row in audit['resources']:
        meta = min(row['variants'][0], key=lambda c: (c['archive'], c['path']))
        with (args.work / meta['archive']).open('rb') as stream:
            if stream.read(4) != b'3slo':
                raise ValueError('Not a PC stream')
            stream.seek(meta['chunk_offset'])
            header = stream.read(44)
            if header_guid(header, meta) != row['guid']:
                raise ValueError('Resource identity changed')
            body = header[8:] + stream.read(u32(header, 4) - 44)
            encoded, payload = original_payload_chunks(stream, meta['size'], ds2str.refpack_decompress)
        if hashlib.sha256(payload).hexdigest() != meta['sha256']:
            raise ValueError('Resource payload changed')
        output.extend(chunk(body))
        output.extend(encoded)
        manifest.append({'guid': row['guid'], 'source': meta})
    output = frame_stream(output)
    reader = ds2str.StreamSet.__new__(ds2str.StreamSet)
    reader.data, reader.pc, reader.end, reader.hdrlen = bytes(output), True, '<', 12
    decoded = list(assets(reader))
    if len(decoded) != len(manifest):
        raise ValueError('Resource count mismatch')
    for (meta, chunks), expected in zip(decoded, manifest):
        payload = reader.asset_payload(chunks, meta['size'])
        if (hashlib.sha256(payload).hexdigest() != expected['source']['sha256'] or
                meta['name'] != expected['source']['name'] or meta['type'] != expected['source']['type']):
            raise ValueError('Resource round trip failed')
        if header_guid(output[meta['chunk_offset']:meta['chunk_offset'] + 44], meta) != expected['guid']:
            raise ValueError('Resource GUID round trip failed')
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        raise ValueError('Output directory must be empty')
    (args.output / args.filename).write_bytes(output)
    (args.output / 'report.json').write_text(json.dumps({'runtime_ready': False,
        'scope': 'Resources supplied by the audit; unexpanded format dependencies remain outside this package',
        'audit_sha256': hashlib.sha256(audit_bytes).hexdigest(), 'resources': manifest,
        'bytes': len(output), 'sha256': hashlib.sha256(output).hexdigest()}, indent=2), encoding='utf-8')
    print(f'Packaged and decoded {len(manifest)} resources; {len(output)} bytes; not installed')


if __name__ == '__main__':
    main()
