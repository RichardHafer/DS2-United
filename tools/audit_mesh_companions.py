"""Audit MeshVolatile companions and extend the supplemental resource audit."""
import hashlib
import json
import struct
import sys
from pathlib import Path
from collections import defaultdict
from inventory_parameter_ids import read_asset
from resolve_pool_resources import header_guid

ROOT = Path(__file__).resolve().parents[1]
from ds2_paths import MODDING_TOOLS, WORK
sys.path.insert(0, str(MODDING_TOOLS))
from ds2str import refpack_decompress


def main():
    audit = json.loads((ROOT / 'analysis/resource_supplement_audit.json').read_bytes())
    catalog = [json.loads(line) for line in
               (ROOT / 'analysis/catalog/assets.jsonl').read_text().splitlines()]
    by_key = defaultdict(list)
    for row in catalog:
        by_key[(row['path'], row['type'], row['name'])].append(row)
    additions, report = [], []
    for resource in audit['resources']:
        mesh = resource['variants'][0][0]
        if mesh['type'] != 'Mesh':
            continue
        candidates = by_key[(mesh['path'], 'MeshVolatile', mesh['name'] + '.volatile')]
        verified = []
        identities = set()
        sizes = set()
        hashes = set()
        for candidate in candidates:
            with (WORK / candidate['archive']).open('rb') as stream:
                stream.seek(candidate['chunk_offset'])
                identity = header_guid(stream.read(44), candidate)
                payload = read_asset(stream, candidate, refpack_decompress)
            digest = hashlib.sha256(payload).hexdigest()
            identities.add(identity)
            sizes.add(len(payload))
            hashes.add(digest)
            verified.append({**candidate, 'sha256': digest})
        with (WORK / mesh['archive']).open('rb') as stream:
            mesh_payload = read_asset(stream, mesh, refpack_decompress)
        total = struct.unpack_from('<I', mesh_payload, 12)[0]
        missing = total - len(mesh_payload)
        valid = bool(verified) and len(identities) == len(hashes) == 1 and sizes == {missing}
        report.append({'mesh': mesh['path'], 'mesh_guid': resource['guid'],
            'mesh_bytes': len(mesh_payload), 'declared_total': total,
            'companion_bytes': sorted(sizes), 'candidate_count': len(candidates),
            'companion_guids': sorted(identities), 'verified': valid})
        if not valid:
            raise ValueError(f'Unverified mesh companion: {mesh["path"]}')
        additions.append({'guid': next(iter(identities)), 'status': 'identical', 'variants': [verified]})
    existing = {row['guid'] for row in audit['resources']}
    if any(row['guid'] in existing for row in additions):
        raise ValueError('Companion identity already present')
    companions = {row['mesh_guid']: addition for row, addition in zip(report, additions)}
    expanded = []
    for resource in audit['resources']:
        expanded.append(resource)
        if resource['guid'] in companions:
            expanded.append(companions[resource['guid']])
    audit['resources'] = expanded
    audit['runtime_ready'] = False
    audit['scope'] = 'Typed supplement plus size-verified MeshVolatile companions'
    (ROOT / 'analysis/resource_supplement_with_companions.json').write_text(json.dumps(audit, indent=2))
    (ROOT / 'analysis/mesh_companion_audit.json').write_text(json.dumps(report, indent=2))
    native = json.loads((ROOT / 'analysis/native_character_streams.json').read_bytes())
    stock_report = []
    for package in native['packages']:
        rows = [row for row in catalog if row['archive'] == package['archive']]
        keys = {(row['path'], row['type'], row['name']) for row in rows}
        meshes = [row for row in rows if row['type'] == 'Mesh']
        missing, self_contained, unknown = [], 0, []
        with (WORK / package['archive']).open('rb') as stream:
            for row in meshes:
                try:
                    payload = read_asset(stream, row, refpack_decompress)
                except (OSError, ValueError) as error:
                    unknown.append({'path': row['path'], 'error': str(error)})
                    continue
                if len(payload) < 40 or payload[:4] != b'MGAE':
                    unknown.append(row['path'])
                    continue
                total = struct.unpack_from('<I', payload, 12)[0]
                if total <= len(payload):
                    self_contained += 1
                elif (row['path'], 'MeshVolatile', row['name'] + '.volatile') not in keys:
                    missing.append(row['path'])
        stock_report.append({'package': package['package'], 'mesh_count': len(meshes),
                             'missing_companions': missing, 'self_contained': self_contained,
                             'unknown_headers': unknown, 'archive_cataloged': bool(rows)})
    (ROOT / 'analysis/native_mesh_companions.json').write_text(json.dumps(stock_report, indent=2))
    print('Stock packages:', len(stock_report), 'meshes:', sum(row['mesh_count'] for row in stock_report),
          'missing companions:', sum(len(row['missing_companions']) for row in stock_report),
          'unknown headers:', sum(len(row['unknown_headers']) for row in stock_report),
          'uncataloged:', sum(not row['archive_cataloged'] for row in stock_report))
    for row in report:
        print(row)
    print(f'Added {len(additions)} companions; runtime preparation remains unverified')


if __name__ == '__main__':
    main()
