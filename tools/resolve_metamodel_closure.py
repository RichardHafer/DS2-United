"""Resolve nested MetaModel tables and record other resource frontiers."""
import argparse
import hashlib
import json
import sys
from collections import defaultdict, deque, Counter
from pathlib import Path
from ds2_assets import sdbm
from inventory_parameter_ids import read_asset
from metamodel_dependencies import dependencies
from resolve_pool_resources import header_guid

ROOT = Path(__file__).resolve().parents[1]


def walk(roots, resolve, read, limit=10000):
    queue = deque(roots)
    seen, nodes, edges = set(), [], []
    while queue:
        guid, type_id = queue.popleft()
        key = guid, type_id
        if key in seen:
            continue
        if len(seen) >= limit:
            raise ValueError('Resource traversal limit reached')
        seen.add(key)
        matches = resolve(guid, type_id)
        node = dict(guid=guid, type_id=type_id, candidates=matches,
                    status='header_matched' if matches else 'unresolved', expanded=False)
        nodes.append(node)
        if not matches or type_id != sdbm('MetaModel'):
            continue
        payloads = [read(m) for m in matches]
        hashes = {hashlib.sha256(p).hexdigest() for p in payloads}
        if len(hashes) != 1:
            raise ValueError('Conflicting MetaModel payloads')
        node['payload_sha256'] = next(iter(hashes))
        node['expanded'] = True
        for row in dependencies(payloads[0]):
            edges.append(dict(source=guid, target=row['guid'], type_id=row['type_id'], offset=row['offset']))
            queue.append((row['guid'], row['type_id']))
    return nodes, edges


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--tool-dir', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, args.tool_dir)
    from ds2str import refpack_decompress
    index = defaultdict(list)
    with (ROOT / 'analysis/catalog/assets.jsonl').open(encoding='utf-8') as stream:
        for line in stream:
            meta = json.loads(line)
            if meta['resource_id']:
                index[meta['resource_id'], sdbm(meta['type'])].append(meta)
    def resolve(guid, type_id):
        found = []
        for meta in index[int(guid.split('-')[0], 16), type_id]:
            with (args.work / meta['archive']).open('rb') as stream:
                if stream.read(4) != b'3slo':
                    raise ValueError('Not a PC stream')
                stream.seek(meta['chunk_offset'])
                if header_guid(stream.read(44), meta) == guid:
                    found.append(meta)
        return found
    def read(meta):
        with (args.work / meta['archive']).open('rb') as stream:
            return read_asset(stream, meta, refpack_decompress)
    source = ROOT / 'analysis/resource_payload_audit.json'
    audit = json.loads(source.read_bytes())
    if audit['errors'] or any(r['status'] != 'identical' for r in audit['resources']):
        raise ValueError('Verified direct resource audit required')
    roots = [(r['guid'], sdbm(r['variants'][0][0]['type'])) for r in audit['resources']]
    nodes, edges = walk(roots, resolve, read)
    report = dict(runtime_ready=False, errors=[], resources=nodes, edges=edges,
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
        scope='Recursive MetaModel asset tables; other resource formats remain unexpanded')
    args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'Resources: {len(nodes)}; expanded models: {sum(n["expanded"] for n in nodes)}; typed edges: {len(edges)}')
    print('Unresolved:', sum(n['status'] == 'unresolved' for n in nodes))
    print('Unexpanded types:', dict(Counter(n['candidates'][0]['type'] for n in nodes if n['candidates'] and not n['expanded'])))


if __name__ == '__main__':
    main()
