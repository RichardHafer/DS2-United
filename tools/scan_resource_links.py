"""Find aligned full resource identities in dependency payloads."""
import argparse
import hashlib
import json
import struct
import sys
from collections import defaultdict, Counter
from pathlib import Path
from ds2_assets import assets
from resolve_pool_resources import header_guid

ROOT = Path(__file__).resolve().parents[1]


def candidates(data, prefixes):
    result = []
    for offset in range(0, len(data) - 15, 4):
        first = struct.unpack_from('<I', data, offset)[0]
        if first and first in prefixes:
            guid = '-'.join(f'{word:08X}' for word in struct.unpack_from('<4I', data, offset))
            result.append((offset, guid))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--tool-dir', required=True)
    parser.add_argument('--stream', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    import ds2str
    by_prefix = defaultdict(list)
    with (ROOT / 'analysis/catalog/assets.jsonl').open(encoding='utf-8') as stream:
        for line in stream:
            meta = json.loads(line)
            if meta['resource_id']:
                by_prefix[meta['resource_id']].append(meta)
    reader = ds2str.StreamSet(args.stream)
    if not reader.pc:
        raise ValueError('Not a PC stream')
    hits = defaultdict(list)
    payloads = []
    for meta, chunks in assets(reader):
        data = reader.asset_payload(chunks, meta['size'])
        digest = hashlib.sha256(data).hexdigest()
        payloads.append({'path': meta['path'], 'sha256': digest, 'type': meta['type']})
        for offset, guid in candidates(data, by_prefix):
            hits[guid].append({'path': meta['path'], 'type': meta['type'], 'offset': offset})
    required = {int(g.split('-')[0], 16) for g in hits}
    matches, errors = defaultdict(list), []
    checked = 0
    for prefix in sorted(required):
        for meta in by_prefix[prefix]:
            try:
                with (args.work / meta['archive']).open('rb') as stream:
                    if stream.read(4) != b'3slo':
                        raise ValueError('Not a PC stream')
                    stream.seek(meta['chunk_offset'])
                    guid = header_guid(stream.read(44), meta)
                checked += 1
                if guid in hits:
                    matches[guid].append(meta)
            except (OSError, ValueError) as error:
                errors.append({'archive': meta['archive'], 'path': meta['path'], 'error': str(error)})
    report = {'scope': 'Aligned binary identity occurrences, not typed dependency proof',
        'runtime_ready': False, 'stream_sha256': hashlib.sha256(args.stream.read_bytes()).hexdigest(),
        'payloads': payloads, 'checked_headers': checked, 'errors': errors,
        'links': [{'guid': guid, 'occurrences': occurrences, 'targets': matches[guid]}
                  for guid, occurrences in sorted(hits.items()) if matches.get(guid)],
        'unmatched_candidates': len(hits) - len(matches)}
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Payloads: {len(payloads)}; full GUID matches: {len(matches)}; checked headers: {checked}; errors: {len(errors)}')
    print('Target types:', dict(Counter(a['type'] for v in matches.values() for a in v)))
    if errors:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
