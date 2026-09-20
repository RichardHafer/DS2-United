"""Include supplemental mesh materials and their texture pairs before geometry."""
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path
from inventory_parameter_ids import read_asset
from mesh_materials import mesh_material_hashes, material_hash, material_texture_paths
from resolve_pool_resources import header_guid

ROOT = Path(__file__).resolve().parents[1]
WORK = Path('D:/SteamLibrary/steamapps/common/Dead Space 2/work')
sys.path.insert(0, 'D:/SteamLibrary/steamapps/common/Dead Space 2/Dead Space 2 Modding Tools')
from ds2str import refpack_decompress


def main():
    catalog = [json.loads(line) for line in (ROOT / 'analysis/catalog/assets.jsonl').read_text().splitlines()]
    by_archive, by_key = defaultdict(list), defaultdict(list)
    for row in catalog:
        by_archive[row['archive']].append(row)
        by_key[(row['path'], row['type'])].append(row)
    audit = json.loads((ROOT / 'analysis/resource_supplement_with_companions.json').read_bytes())
    additions, report = {}, []

    def payload(row):
        with (WORK / row['archive']).open('rb') as stream:
            return read_asset(stream, row, refpack_decompress)

    def include(key):
        if key in additions:
            return
        candidates = by_key[key]
        if not candidates:
            raise ValueError(f'Missing dependency: {key}')
        variants, identities = defaultdict(list), set()
        for row in candidates:
            data = payload(row)
            digest = hashlib.sha256(data).hexdigest()
            with (WORK / row['archive']).open('rb') as stream:
                stream.seek(row['chunk_offset'])
                identities.add(header_guid(stream.read(44), row))
            variants[digest].append({**row, 'sha256': digest})
        if len(variants) != 1 or len(identities) != 1:
            raise ValueError(f'Conflicting dependency: {key}')
        additions[key] = dict(guid=next(iter(identities)), status='identical', variants=list(variants.values()))

    for resource in audit['resources']:
        mesh = resource['variants'][0][0]
        if mesh['type'] != 'Mesh':
            continue
        required = mesh_material_hashes(payload(mesh))
        matches = defaultdict(set)
        for source in resource['variants'][0]:
            for row in by_archive[source['archive']]:
                if row['type'] == 'Material' and material_hash(payload(row)) in required:
                    matches[material_hash(payload(row))].add((row['path'], row['type']))
        for identity in required:
            if len(matches[identity]) != 1:
                raise ValueError(f'Material {identity:08X}: expected one typed path')
            key = next(iter(matches[identity]))
            data = payload(by_key[key][0])
            textures = material_texture_paths(data)
            for path in textures:
                include((path + '.tg4h', 'tg4h'))
                include((path + '.tg4d', 'tg4d'))
            include(key)
            report.append(dict(mesh=mesh['path'], material_hash=f'{identity:08X}',
                               material=key[0], textures=textures))
    # Texture headers and data stay adjacent; all materials precede the meshes.
    audit['resources'] = list(additions.values()) + audit['resources']
    audit['scope'] = 'Supplemental geometry, volatile data, material hashes and named texture pairs'
    (ROOT / 'analysis/resource_supplement_with_materials.json').write_text(json.dumps(audit, indent=2))
    (ROOT / 'analysis/mesh_material_audit.json').write_text(json.dumps(report, indent=2))
    print(f'Mesh/material bindings: {len(report)}; added resources: {len(additions)}')
    for row in report:
        print(row['mesh'], row['material_hash'], row['material'], len(row['textures']))


if __name__ == '__main__':
    main()
