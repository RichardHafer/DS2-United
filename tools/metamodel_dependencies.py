"""Read version-14 MetaModel asset tables from PC payloads."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
from ds2_assets import assets, sdbm


def dependencies(data):
    if len(data) < 0x68:
        raise ValueError('Short MetaModel header')
    magic, version, size = struct.unpack_from('<III', data)
    if magic != 0x4D4D646C or version != 14:
        raise ValueError('Unsupported MetaModel format')
    offset = struct.unpack_from('<I', data, 0x34)[0]
    count = struct.unpack_from('<H', data, 0x58)[0]
    if size > len(data) or size < 0x68:
        raise ValueError('Invalid MetaModel size')
    if count and (offset < 0x68 or offset % 4 or offset + count * 32 > size):
        raise ValueError('Invalid MetaModel asset table')
    result = []
    for index in range(count):
        at = offset + index * 32
        name, path, type_id = struct.unpack_from('<III', data, at)
        guid = '-'.join(f'{x:08X}' for x in struct.unpack_from('<4I', data, at + 12))
        result.append(dict(index=index, offset=at, name_id=name, path_id=path, type_id=type_id, guid=guid))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stream', type=Path, required=True)
    parser.add_argument('--links', type=Path, required=True)
    parser.add_argument('--tool-dir', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str
    links = json.loads(args.links.read_bytes())
    digest = hashlib.sha256(args.stream.read_bytes()).hexdigest()
    if links['stream_sha256'] != digest or links['errors']:
        raise ValueError('Resource link scan does not match stream')
    targets = {row['guid']: row['targets'] for row in links['links']}
    reader = ds2str.StreamSet(args.stream)
    if not reader.pc:
        raise ValueError('Not a PC stream')
    report = dict(stream_sha256=digest, models=[], runtime_ready=False)
    for meta, chunks in assets(reader):
        if meta['type'] != 'MetaModel':
            continue
        rows = dependencies(reader.asset_payload(chunks, meta['size']))
        for row in rows:
            found = targets.get(row['guid'], [])
            row['targets'] = [t for t in found if sdbm(t['type']) == row['type_id']]
            row['status'] = 'matched' if row['targets'] else 'unresolved'
        report['models'].append(dict(path=meta['path'], dependencies=rows))
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    rows = [d for m in report['models'] for d in m['dependencies']]
    print(f'Models: {len(report["models"])}; typed entries: {len(rows)}; matched: {sum(r["status"] == "matched" for r in rows)}')
    print(f'Distinct dependency GUIDs: {len({r["guid"] for r in rows})}')


if __name__ == '__main__':
    main()
