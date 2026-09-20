"""Rename isolated entity occurrences and their resolved dependency edges."""
import argparse
import hashlib
import json
import struct
from collections import defaultdict
from pathlib import Path
from ds2_assets import simgroup
from remap_pool_guids import remap, guid_hash


def plan(groups, graph, reserved):
    flagged = {(n['archive'], n['guid']) for root in graph['roots'] for n in root['nodes'] if n['spawn_flags']}
    nodes, by_reference, by_group = {}, defaultdict(list), defaultdict(list)
    for group in groups:
        for node in group['entities']:
            key = (group['archive'], group['asset'], node['index'])
            if key in nodes:
                raise ValueError('Duplicate source occurrence')
            nodes[key] = node
            by_reference[group['archive'], node['guid']].append(key)
            by_group[group['file']].append(key)
    used = set(reserved) | {0, 0xFFFFFFFF}
    used.update(guid_hash(n['guid']) for n in nodes.values())
    destinations = {}
    for key in sorted(nodes):
        for nonce in range(100000):
            token = json.dumps(['DS2Randomizer:entities:v1', *key, nonce], separators=(',', ':')).encode()
            guid = '-'.join(f'{x:08X}' for x in struct.unpack('<4I', hashlib.sha256(token).digest()[:16]))
            value = guid_hash(guid)
            if value not in used:
                break
        else:
            raise ValueError('Entity hash allocation exhausted')
        used.add(value)
        destinations[key] = guid
    maps = {name: {nodes[key]['guid']: destinations[key] for key in keys} for name, keys in by_group.items()}
    if any(len(maps[name]) != len(keys) for name, keys in by_group.items()):
        raise ValueError('Duplicate GUID within a group')
    owners = {key: name for name, keys in by_group.items() for key in keys}
    edges = set()
    external = set()
    for root in graph['roots']:
        if root['truncated']:
            raise ValueError('Truncated dependency graph')
        for edge in root['edges']:
            if edge['status'] != 'entity':
                continue
            sources = by_reference[edge['source_archive'], edge['source']]
            targets = by_reference[edge['target_archive'], edge['target']]
            if not sources:
                if (edge['source_archive'], edge['source']) not in flagged:
                    raise ValueError('Missing source occurrence')
                continue
            if len(sources) != 1:
                raise ValueError('Ambiguous source occurrence')
            source = sources[0]
            if not targets:
                if (edge['target_archive'], edge['target']) not in flagged:
                    raise ValueError('Missing target occurrence')
                external.add((owners[source], edge['target_archive'], edge['target']))
                continue
            if len(targets) != 1:
                raise ValueError('Ambiguous target occurrence')
            target = targets[0]
            mapping = maps[owners[source]]
            old, new = edge['target'], destinations[target]
            if old in mapping and mapping[old] != new:
                raise ValueError('Conflicting dependency binding within a group')
            mapping[old] = new
            edges.add((source, target))
    return maps, destinations, sorted(edges), sorted(external)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pool', type=Path, required=True)
    parser.add_argument('--parameters', type=Path, required=True)
    parser.add_argument('--graph', type=Path, required=True)
    parser.add_argument('--inventory', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    pool_bytes = (args.pool / 'report.json').read_bytes()
    pool = json.loads(pool_bytes)
    parameters = json.loads((args.parameters / 'report.json').read_bytes())
    graph_bytes, inventory_bytes = args.graph.read_bytes(), args.inventory.read_bytes()
    inventory = json.loads(inventory_bytes)
    if (pool.get('errors') or not inventory.get('catalog_complete') or inventory.get('errors') or
            parameters['pool_report_sha256'] != hashlib.sha256(pool_bytes).hexdigest() or
            parameters['inventory_sha256'] != hashlib.sha256(inventory_bytes).hexdigest() or
            pool['graph_sha256'] != hashlib.sha256(graph_bytes).hexdigest()):
        raise ValueError('Inconsistent namespace inputs')
    maps, identities, edges, external = plan(pool['groups'], json.loads(graph_bytes), inventory['entity_hashes'])
    parameter_groups = {g['file']: g for g in parameters['groups']}
    if set(parameter_groups) != set(maps):
        raise ValueError('Parameter group set mismatch')
    outputs, audits = {}, []
    for group in pool['groups']:
        name = group['file']
        if Path(name).name != name:
            raise ValueError('Invalid group filename')
        data = (args.parameters / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != parameter_groups[name]['sha256']:
            raise ValueError('Parameter payload checksum mismatch')
        output, audit = remap(data, maps[name])
        records, _ = simgroup(output)
        expected = [identities[group['archive'], group['asset'], n['index']] for n in group['entities']]
        if [r['guid'] for r in records] != expected or audit['identities'] != len(expected):
            raise ValueError('Entity namespace round trip failed')
        outputs[name] = output
        audits.append({'file': name, 'sha256': hashlib.sha256(output).hexdigest(),
            'identities': audit['identities'], 'references': audit['references'], 'mapping': maps[name]})
    args.output.mkdir(parents=True, exist_ok=True)
    if any(args.output.iterdir()):
        raise ValueError('Output directory must be empty')
    for name, data in outputs.items():
        (args.output / name).write_bytes(data)
    manifest = {'runtime_ready': False, 'groups': audits, 'external_entities': external,
        'resolved_edges': len(edges), 'graph_sha256': hashlib.sha256(graph_bytes).hexdigest(),
        'identities': [{'source': key, 'destination': value} for key, value in sorted(identities.items())]}
    (args.output / 'report.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'Entities: {len(identities)}; resolved edges: {len(edges)}; typed reference edits: {sum(a["references"] for a in audits)}')
    print(f'External entity bindings: {len(external)}; runtime registration remains unverified')


if __name__ == '__main__':
    main()
