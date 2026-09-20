"""Inventory authored schematic rewards and emit an offline campaign policy table."""
import argparse
import hashlib
import json
from pathlib import Path

VISUAL_FIELDS = ('CMD_SetPickupIconLargeVFX', 'CMD_InventoryTextVFX',
                 'CMD_InventoryNewlyAddedVFX', 'CMD_SetSchematicsLoadedVFX')


def fields(row):
    return {f['name']: f.get('string_candidate', f.get('guid_candidate', f['value']))
            for image in row['images'] for f in image.get('fields', [])}


def campaign(row):
    parts = row['archive'].replace('\\', '/').split('/')
    return len(parts) > 1 and parts[0] in ('ds2dat7', 'ds2dat8', 'ds2dat9') and parts[1].startswith('ch')


def inventory(catalog):
    states = json.loads((catalog / 'scan_state.json').read_text())
    if any(s['errors'] for s in states):
        raise ValueError('Resolve catalog scan errors before generating the schematic pool')
    rows = [json.loads(line) for line in (catalog / 'entities.jsonl').open(encoding='utf-8')]
    globals_ = {r['guid']: r for r in rows
                if r['archive'].replace('\\', '/') == 'ds2dat10/global_assets/global_assets.str'}
    definitions = {guid: r for guid, r in globals_.items() if r['class'] == 'Schematic'}
    sources = []
    references = []
    for row in rows:
        if not campaign(row):
            continue
        f = fields(row)
        if row['class'] == 'Schematic':
            definition = f['CMD_SetSharedPickupItemGUID']
            kind = 'Placed'
        else:
            for image in row['images']:
                for field in image.get('fields', []):
                    if field.get('guid_candidate') in definitions:
                        references.append(dict(source=row['guid'], archive=row['archive'],
                                               source_class=row['class'], field=field['name'],
                                               definition=field['guid_candidate']))
            definition = f.get('CMD_SetDropItemGUID')
            if row['class'] != 'StandardContainer' or definition not in definitions:
                continue
            kind = 'Container'
        if definition not in definitions:
            raise ValueError(f'Missing global definition for {row["guid"]}')
        d = fields(definitions[definition])
        reward = d['CMD_SetPickupItemGUID']
        if reward not in globals_:
            raise ValueError('Reward definition missing from global assets')
        reward_fields = fields(globals_[reward])
        if kind == 'Placed' and f['CMD_SetPickupItemGUID'] != reward:
            raise ValueError('Placed reward differs from shared definition')
        status = 'base_story_candidate'
        if f.get('CMD_SetCompletionRequirement', 0):
            status = 'completion_gated'
        elif 'hacker' in d['CMD_OnPickupMsg']:
            status = 'hacker_access_unverified'
        if row['spawn_flags'] != 1 or not row['transform_offset']:
            raise ValueError('Unexpected source spawn flags or missing placement')
        sources.append(dict(source_guid=row['guid'], kind=kind, definition_guid=definition,
                            reward_guid=reward, reward_name=reward_fields.get('CMD_SetName', d['CMD_OnPickupMsg']),
                            reward_class=globals_[reward]['class'], event=d['CMD_OnPickupMsg'], status=status,
                            archive=row['archive'], asset=row['asset'], index=row['index'],
                            asset_chunk_offset=row['asset_chunk_offset'],
                            spawn_flags=row['spawn_flags'], transform_offset=row['transform_offset'],
                            completion_requirement=f.get('CMD_SetCompletionRequirement', 0),
                            min_level_requirement=f.get('CMD_SetMinLevelReq', 0)))
        if status == 'base_story_candidate':
            for name in VISUAL_FIELDS:
                if not isinstance(d.get(name), str) or len(d[name].split('-')) != 4:
                    raise ValueError('Missing presentation GUID for base-story schematic')
                if kind == 'Placed' and f.get(name) != d[name]:
                    raise ValueError('Authored presentation differs from its global schematic')
            sources[-1]['visual_guids'] = [d[name] for name in VISUAL_FIELDS]
    sources.sort(key=lambda r: r['source_guid'])
    base = [r for r in sources if r['status'] == 'base_story_candidate']
    if len(base) != 23 or len({r['reward_guid'] for r in base}) != 23:
        raise ValueError('Campaign pool changed; review the census before regenerating')
    types = []
    for guid, row in sorted(definitions.items()):
        f = fields(row)
        reward = globals_[f['CMD_SetPickupItemGUID']]
        found = [r for r in sources if r['definition_guid'] == guid]
        types.append(dict(definition_guid=guid, reward_guid=f['CMD_SetPickupItemGUID'],
                          event=f['CMD_OnPickupMsg'], reward_name=fields(reward).get('CMD_SetName', f['CMD_OnPickupMsg']),
                          reward_class=reward['class'], archive=row['archive'], asset=row['asset'],
                          index=row['index'], campaign_sources=[r['source_guid'] for r in found],
                          status=found[0]['status'] if found else 'no_campaign_source_found'))
    return dict(schema=1, enabled=False, runtime_integrated=False,
                scope='Base story candidate pool; authored data proves placement, not all runtime gates',
                catalog_sha256=hashlib.sha256((catalog / 'entities.jsonl').read_bytes()).hexdigest(),
                archives=len(states), groups=sum(s['groups'] for s in states),
                entities=len(rows), scan_errors=0, definitions=types, sources=sources,
                other_campaign_references=references)


def guid_cpp(value):
    return '{{' + ', '.join('0x' + v + 'u' for v in value.split('-')) + '}}'


def write_outputs(report, output, header):
    output.mkdir(parents=True, exist_ok=True)
    (output / 'schematic_catalog.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    lines = ['# Authored campaign schematic inventory', '',
             'Generated from the extracted PC asset census. Internal level names are not displayed chapter numbers.', '',
             f"Scanned {report['archives']} archives, {report['groups']} SimGroups and {report['entities']} entities, with no parser errors.", '',
             '39 global definitions; 28 placed Schematic entities and one StandardContainer source in campaign levels.',
             'The base-story candidate pool has 23 distinct rewards. Four completion-gated elite suits and two Hacker sources remain excluded.',
             'This establishes authored sources, not complete reachability or entitlement behavior. No runtime hook is installed.', '',
             '| Event / reward label | Status | Source kind | Entity GUID | Reward GUID | Archive |',
             '| --- | --- | --- | --- | --- | --- |']
    for r in report['sources']:
        lines.append(f"| {r['reward_name']} | {r['status']} | {r['kind']} | `{r['source_guid']}` | `{r['reward_guid']}` | `{r['archive']}` |")
    lines += ['', '## All global definitions', '', '| Event | Definition GUID | Reward GUID | Status |', '| --- | --- | --- | --- |']
    for r in report['definitions']:
        lines.append(f"| {r['reward_name']} ({r['event']}) | `{r['definition_guid']}` | `{r['reward_guid']}` | {r['status']} |")
    lines += ['', 'JSON retains exact SimGroup paths, chunk offsets, instance indices, gates and every decoded campaign reference to global schematic definitions.', '']
    (output / 'SCHEMATIC_INVENTORY.md').write_text('\n'.join(lines), encoding='utf-8')
    body = ['#pragma once', '// Generated by tools/build_schematic_catalog.py.', '#include "schematic_policy.h"', '', 'namespace ds2::schematics {', 'inline constexpr Source kCampaignSources[] = {']
    for r in report['sources']:
        if r['status'] == 'base_story_candidate':
            body.append('    {' + guid_cpp(r['source_guid']) + ', ' + guid_cpp(r['reward_guid']) + ', SourceKind::' + r['kind'] + '},')
    body += ['};', '}  // namespace ds2::schematics', '']
    header.write_text('\n'.join(body), encoding='utf-8')
    body = ['#pragma once', '// Generated by tools/build_schematic_catalog.py.',
            '#include "schematic_attributes.h"', '', 'namespace ds2::schematics {',
            'inline constexpr Blueprint kCampaignBlueprints[] = {']
    for r in report['sources']:
        if r['status'] == 'base_story_candidate':
            body.append('    {' + guid_cpp(r['definition_guid']) + ', {{' +
                        ', '.join(guid_cpp(v) for v in r['visual_guids']) + '}}, ' +
                        json.dumps(r['reward_name']) + '},')
    body += ['};', '}  // namespace ds2::schematics', '']
    header.with_name('generated_schematic_blueprints.h').write_text('\n'.join(body), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parents[1]
    parser.add_argument('--catalog', type=Path, default=root / 'analysis/catalog')
    parser.add_argument('--output', type=Path, default=root / 'analysis/schematics')
    parser.add_argument('--header', type=Path, default=root / 'src/generated_schematic_catalog.h')
    args = parser.parse_args()
    report = inventory(args.catalog)
    write_outputs(report, args.output, args.header)
    print(f"Definitions={len(report['definitions'])}, campaign sources={len(report['sources'])}, base pool=23; runtime disabled")
