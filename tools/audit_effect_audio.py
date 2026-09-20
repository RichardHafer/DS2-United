"""Inventory effect/audio payloads and match their explicit binary references."""
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path
from ds2_assets import assets
from resolve_pool_resources import header_guid
from scan_resource_links import candidates

ROOT = Path(__file__).resolve().parents[1]
WORK = Path('D:/SteamLibrary/steamapps/common/Dead Space 2/work')
sys.path.insert(0, 'D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools')
import ds2str


def main():
    catalog = [json.loads(line) for line in (ROOT / 'analysis/catalog/assets.jsonl').read_text().splitlines()]
    by_prefix, by_name = defaultdict(list), defaultdict(list)
    for row in catalog:
        by_prefix[row['resource_id']].append(row)
        for value in {row['name'].replace('\\', '/').rsplit('/', 1)[-1].lower(),
                      row['path'].replace('\\', '/').rsplit('/', 1)[-1].lower()}:
            by_name[value].append(row)
    packages = json.loads((ROOT / 'analysis/native_character_streams.json').read_bytes())['packages']
    stock = {row['archive'] for row in packages}
    stock.update(['ds2dat10\\global_assets\\global_assets.str',
                  'ds2dat10\\global_assets\\global_assets_global.str'])
    supplement = ROOT / 'analysis/mesh_companion_runtime/ds2_randomizer_pool.str'
    report = {'scope': 'Aligned full GUIDs and literal filenames; not a decoded event graph',
              'runtime_ready': False, 'payloads': [], 'links': [], 'literal_names': [], 'errors': []}
    hits, literal = defaultdict(list), defaultdict(list)
    types = {'VFX', 'SBK', 'RCB', 'Lua', 'LUA'}
    supplied_paths = set()
    sources = [(WORK / archive, archive) for archive in sorted(stock)] + [(supplement, 'private_pool')]
    for path, archive in sources:
        try:
            reader = ds2str.StreamSet(path)
            for meta, parts in assets(reader):
                supplied_paths.add((meta['path'], meta['type']))
                if meta['type'] not in types:
                    continue
                data = reader.asset_payload(parts, meta['size'])
                source = {'archive': archive, 'path': meta['path'], 'type': meta['type']}
                report['payloads'].append({**source, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
                for offset, guid in candidates(data, by_prefix):
                    hits[guid].append({**source, 'offset': offset})
                for match in re.finditer(rb'[A-Za-z0-9_./\\:-]{3,}\.(?:sbk|wav|snu|lua|vfb|vfx|rcb)\x00', data, re.I):
                    name = match.group()[:-1].decode('ascii')
                    literal[name].append({**source, 'offset': match.start()})
        except (OSError, ValueError) as error:
            report['errors'].append({'archive': archive, 'error': str(error)})
    matches = defaultdict(list)
    for prefix in sorted({int(g.split('-')[0], 16) for g in hits}):
        for meta in by_prefix[prefix]:
            try:
                with (WORK / meta['archive']).open('rb') as stream:
                    stream.seek(meta['chunk_offset'])
                    guid = header_guid(stream.read(44), meta)
                if guid in hits:
                    matches[guid].append(meta)
            except (OSError, ValueError) as error:
                report['errors'].append({'archive': meta['archive'], 'error': str(error)})
    for guid, targets in sorted(matches.items()):
        supplied = any((target['path'], target['type']) in supplied_paths for target in targets)
        report['links'].append({'guid': guid, 'occurrences': hits[guid],
                                'targets': targets, 'supplied_by_path_and_type': supplied})
    for name, occurrences in sorted(literal.items()):
        basename = name.replace('\\', '/').rsplit('/', 1)[-1].lower()
        targets = by_name.get(basename, [])
        report['literal_names'].append({'name': name, 'occurrences': occurrences,
            'candidate_count': len(targets),
            'supplied_by_name': any((row['path'], row['type']) in supplied_paths for row in targets)})
    report['unmatched_binary_candidates'] = len(hits) - len(matches)
    report['summary'] = {'payload_types': dict(Counter(row['type'] for row in report['payloads'])),
        'matched_guids': len(matches), 'outside_loaded_paths': sum(not row['supplied_by_path_and_type'] for row in report['links']),
        'literal_names': len(literal), 'errors': len(report['errors'])}
    (ROOT / 'analysis/effect_audio_audit.json').write_text(json.dumps(report, indent=2))
    print(report['summary'], flush=True)
    for row in report['links']:
        if not row['supplied_by_path_and_type']:
            print('OUTSIDE', row['guid'], sorted({r['path'] for r in row['targets']}), flush=True)
    for row in report['literal_names']:
        if not row['supplied_by_name']:
            print('NAME_UNVERIFIED', row['name'], row['candidate_count'], flush=True)


if __name__ == '__main__':
    main()
