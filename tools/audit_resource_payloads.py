"""Compare full payloads of GUID-matched dependency resources."""
import argparse
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path
from inventory_parameter_ids import read_asset
from resolve_pool_resources import header_guid

ROOT = Path(__file__).resolve().parents[1]


def classify(candidates):
    variants = defaultdict(list)
    for candidate in candidates:
        variants[candidate['sha256'], candidate['type']].append(candidate)
    return 'identical' if len(variants) == 1 else 'conflicting', list(variants.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--tool-dir', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=ROOT / 'analysis/pool_resource_matches.json')
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    from ds2str import refpack_decompress
    source = args.source
    report = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'resources': [], 'errors': [], 'runtime_ready': False}
    for row in json.loads(source.read_bytes())['resources']:
        if row['status'] != 'header_matched':
            continue
        candidates = []
        for meta in row['candidates']:
            try:
                with (args.work / meta['archive']).open('rb') as stream:
                    if stream.read(4) != b'3slo':
                        raise ValueError('Not a PC stream')
                    stream.seek(meta['chunk_offset'])
                    if header_guid(stream.read(44), meta) != row['guid']:
                        raise ValueError('Full resource GUID mismatch')
                    data = read_asset(stream, meta, refpack_decompress)
                candidates.append({**meta, 'sha256': hashlib.sha256(data).hexdigest()})
            except (OSError, ValueError) as error:
                report['errors'].append({'guid': row['guid'], 'archive': meta['archive'], 'error': str(error)})
        status, variants = classify(candidates)
        if len(candidates) != len(row['candidates']):
            status = 'incomplete'
        report['resources'].append({'guid': row['guid'], 'status': status, 'variants': variants})
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    counts = {status: sum(r['status'] == status for r in report['resources'])
              for status in ('identical', 'conflicting', 'incomplete')}
    print(counts, 'errors:', len(report['errors']))
    for row in report['resources']:
        if row['status'] == 'conflicting':
            print(row['guid'], [(v[0]['type'], len(v), v[0]['archive']) for v in row['variants']])
    if report['errors']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
