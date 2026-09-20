"""Join authored enemy targets, spawners, vent markers and package evidence."""
import argparse
from collections import Counter,defaultdict
import csv
import json
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--catalog',type=Path,required=True)
p.add_argument('--dictionary',type=Path,required=True)
p.add_argument('--work-root',type=Path,required=True)
args=p.parse_args()
out=args.catalog
dictionary=json.loads(args.dictionary.read_text())
bases=dictionary['bases']
def npc(name):
    return name=='EARS::Modules::DeadSpaceNPC' or any(npc(b) for b in bases.get(name,[]))
npc_classes={k.rsplit('::',1)[-1] for k in bases if npc(k)}
special={'Kong','GuardianTentacle','StabTentacle','Landmine','NestProjectile',
         'DragTentacle','SlamDragTentacle','LeviathanTentacle','SlugTentacle'}
enemy_classes=(npc_classes|special)-{'NeutralNPC','GenericNPC','DeadSpaceNPC','CadaverousNPC'}
constraints={
 'Guardian':'Wall-bound actor; pod/tentacle and attack geometry dependencies',
 'GuardianPod':'Guardian-dependent sub-enemy; standalone lifecycle unverified',
 'GuardianTentacle':'Guardian-dependent appendage; not a whole enemy replacement',
 'Nest':'Anchored encounter actor; zero-gravity/location and projectile dependencies',
 'NestProjectile':'Dependent projectile; not a whole enemy replacement',
 'Landmine':'Anchored hazard actor; root/body and projectile ownership must be distinguished',
 'Polyp':'PDB class exists but no authored instances found; do not equate name with Landmine',
 'Kong':'Scripted boss/paired sequence; ordinary spawn eligibility unverified',
 'NicoleBoss':'Final encounter logic; preserve encounter mechanics',
 'BossTripod':'Boss-specific state and paired animations; preserve encounter mechanics',
 'Tripod':'Encounter/zero-gravity constraints; shared name does not prove interchangeability',
 'WindowTripod':'Window-bound encounter; surface and scripted target required',
 'TripodProjectile':'Dependent enemy-class projectile; not a whole enemy replacement',
 'DragTentacle':'Player drag/paired sequence and encounter path dependencies',
 'StabTentacle':'Attack appendage; no authored instance found in scanned corpus',
 'SlamDragTentacle':'Paired encounter appendage; no authored instance found',
 'LeviathanTentacle':'PDB legacy/special class; no authored instance found',
 'SlugTentacle':'PDB legacy/special class; no authored instance found',
 'DividerPart':'Detached-part lifecycle and owner/animation constraints',
 'DividerCore':'Dismemberment creates dependent parts; retain resource and owner contracts',
 'SwarmEnemy':'Small swarm unit; population and aggregation lifecycle required',
 'Infector':'Corpse selection/infection and target resources required',
 'Hunter':'Regeneration/pursuit lifecycle; identify authored model before labeling Hunter/Ubermorph',
 'Stalker':'Cover/navigation and coordinated AI dependencies',
 'Brute':'Large actor clearance, attack space and door/vent geometry constraints',
}
def eligibility(cls):
    if cls in {'NestProjectile','TripodProjectile','GuardianTentacle','StabTentacle','DividerPart'}:
        return 'dependent_enemy_or_attack_part'
    if cls in constraints: return 'constrained_enemy'
    if cls in enemy_classes: return 'ordinary_candidate_unverified_swap'
    if cls in {'NeutralNPC','GenericNPC'}: return 'not_confirmed_hostile'
    return 'non_enemy'
def fields(row,component=None):
    result={}
    for image in row['images']:
        if component and image.get('class')!=component: continue
        for f in image.get('fields',[]):
            result[f['name']]=f.get('string_candidate',f.get('guid_candidate',f['value']))
    return result
def chapter(row):
    return row['archive'].replace('\\','/').split('/')[1]
def same_scope(source,candidates):
    exact=[r for r in candidates if r['archive']==source['archive']]
    return exact or [r for r in candidates if chapter(r)==chapter(source)] or candidates
def write(name,rows,columns):
    with (out/name).open('w',newline='',encoding='utf-8') as f:
        w=csv.DictWriter(f,fieldnames=columns,delimiter='\t',extrasaction='ignore')
        w.writeheader();w.writerows(rows)
records=[]
index=defaultdict(list)
for line in (out/'entities.jsonl').open(encoding='utf-8'):
    row=json.loads(line)
    index[row['guid']].append(row)
    records.append(row)
models=defaultdict(set)
packages=defaultdict(set)
for line in (out/'assets.jsonl').open(encoding='utf-8'):
    asset=json.loads(line)
    if asset['type']=='MetaModel':
        models[asset['resource_id']].add(asset['name'])
        if '\\char_str\\npc\\' in asset['archive']:
            packages[asset['resource_id']].add(Path(asset['archive']).stem)
enemy_rows=[]
by_guid={}
for row in records:
    cls=row['class']
    if cls not in enemy_classes: continue
    f=fields(row)
    spawning_component=any(i.get('class')=='DSNPCSpawningComponent' for i in row['images'])
    mode=f.get('CMD_SetSpawningMode','default_NONE_descriptor' if spawning_component else 'no_spawning_component')
    vent=f.get('CMD_SetSpawningVent','')
    vents=same_scope(row,[r for r in index.get(vent,[]) if r['class']=='VentMarker'])
    vent_types=sorted({str(fields(v).get('CMD_SetVentType','default_FLOOR_descriptor')) for v in vents})
    model=f.get('CMD_SetModel','')
    model_id=int(model.split('-')[0],16) if isinstance(model,str) and '-' in model else 0
    context=str(mode)
    if mode=='SPAWNMODE_VENT':
        context+='|'+('|'.join(vent_types) if vent_types else 'unresolved_vent')
    result={'class':cls,'guid':row['guid'],'archive':row['archive'],
        'asset':row['asset'],'entity_index':row['index'],'entity_offset':hex(row['offset']),
        'script_name':f.get('CMD_SetScriptName',''),'model_guid':model,
        'model_assets':'|'.join(sorted(models.get(model_id,[]))),
        'package_candidates':'|'.join(sorted(packages.get(model_id,[]))),
        'mode':mode,'vent_guid':vent,'vent_type':'|'.join(vent_types),
        'context':context,'vent_matches':len(vents),'eligibility':eligibility(cls),
        'constraints':constraints.get(cls,'Resource residency, authored arrival and active PC ABI still require validation'),
        'inactive_animation':f.get('CMD_SetSpawningInactiveAnim',''),
        'active_animation':f.get('CMD_SetSpawningActiveAnim',''),
        'animation_flags':f.get('CMD_SetSpawningAnimOptions',''),
        'activation_flags':f.get('CMD_SetActivationFlags',''),
        'vent_components':'|'.join(i.get('class','') for i in row['images'] if 'VentComponent' in i.get('class','')),
        'multiplayer':chapter(row).startswith('mp_') or chapter(row).startswith('global_mp')}
    if cls=='DragTentacle' and 'bed_hospital01_mm_drag.meta.xml' in result['model_assets']:
        result['eligibility']='non_enemy_interaction_proxy'
        result['constraints']='Hospital-bed scripted drag proxy, not a hostile actor; never select as an enemy'
    # The grapple is the only attack an Infector has on the player. One of them,
    # the psych hall infector of the intro chase, has both grapple chances
    # authored to zero because Isaac is in a straitjacket there. As a donor it
    # wanders around the player and never attacks. It stays a source, so the
    # intro spawn can still be replaced, but it is never handed out.
    if cls=='Infector' and f.get('CMD_SetGrappleChanceCorpse')==0 and f.get('CMD_SetGrappleChanceNoCorpse')==0:
        result['eligibility']='non_hostile_scripted'
        result['constraints']='Grapple chance authored to zero for the intro chase; never attacks the player, never select as a donor'
    enemy_rows.append(result)
    by_guid[(row['archive'],row['guid'])]=result
spawn_rows=[]
for row in records:
    if row['class']!='FuncSpawn': continue
    f=fields(row)
    target=f.get('CMD_SetSpawnTarget','')
    candidates=same_scope(row,index.get(target,[]))
    classes=sorted({r['class'] for r in candidates})
    enemy=[r for r in candidates if r['class'] in enemy_classes and
           by_guid[(r['archive'],r['guid'])]['eligibility']!='non_enemy_interaction_proxy']
    resolved=[by_guid[(r['archive'],r['guid'])] for r in enemy]
    status='enemy_target' if enemy and len(classes)==1 else ('unresolved' if not candidates else 'non_enemy_target' if not enemy else 'ambiguous')
    spawn_rows.append({'archive':row['archive'],'source_guid':row['guid'],
        'source_name':f.get('CMD_SetTargetName',''),'target_guid':target,
        'target_classes':'|'.join(classes),'classification':status,
        'target_matches':len(candidates),'target_contexts':'|'.join(sorted({r['context'] for r in resolved})),
        'max_spawn':f.get('CMD_SetMaxSpawn','unspecified'),
        'options':f.get('CMD_SetSpawnOptions','unspecified'),
        'entity_offset':hex(row['offset']),'asset':row['asset']})
write('enemy_targets.tsv',enemy_rows,list(enemy_rows[0]))
write('funcspawn_targets.tsv',spawn_rows,list(spawn_rows[0]))
variant_groups=defaultdict(list)
for r in enemy_rows:
    variant_groups[(r['class'],r['model_guid'],r['model_assets'])].append(r)
write('enemy_model_variants.tsv',[{'class':c,'model_guid':g,'model_assets':m,
    'package_candidates':'|'.join(sorted({p for r in rs for p in r['package_candidates'].split('|') if p})),
    'authored_records':len(rs),'contexts':'|'.join(sorted({r['context'] for r in rs})),
    'eligibility':'|'.join(sorted({r['eligibility'] for r in rs})),
    'evidence_archive':rs[0]['archive'],'evidence_guid':rs[0]['guid']}
    for (c,g,m),rs in sorted(variant_groups.items())],
    ['class','model_guid','model_assets','package_candidates','authored_records','contexts','eligibility','evidence_archive','evidence_guid'])
context_counts=Counter((r['class'],r['model_assets'],r['context']) for r in enemy_rows if not r['multiplayer'])
write('observed_spawn_contexts.tsv',[{'class':c,'model_assets':m,'context':ctx,'authored_records':n,
    'replacement_status':'observed_donor_context_only_not_swap_verified'} for (c,m,ctx),n in sorted(context_counts.items())],
    ['class','model_assets','context','authored_records','replacement_status'])
counts=Counter(r['class'] for r in enemy_rows)
write('enemy_classes.tsv',[{'class':c,'authored_records':counts[c],
    'eligibility':eligibility(c),'constraints':constraints.get(c,'Ordinary candidate; exact donor compatibility and resources must be validated'),
    'evidence':'Xbox PDB type'+('; PC authored instances' if counts[c] else '; no authored instance in corpus')}
    for c in sorted(enemy_classes)],['class','authored_records','eligibility','constraints','evidence'])
package_rows=[]
for path in sorted(args.work_root.rglob('*.str')):
    if '\\char_str\\npc\\' not in str(path): continue
    stem=path.stem
    matches=[r for r in enemy_rows if stem in r['package_candidates'].split('|')]
    warning='Package assets present; not proof of a unique actor variant or live resources'
    if stem.startswith(('mp_','deprecated_')): warning='MP/deprecated label; do not include without authored campaign evidence'
    package_rows.append({'package':stem,'archive':str(path.relative_to(args.work_root)),
        'matched_classes':'|'.join(sorted({r['class'] for r in matches})),
        'model_linked_records':len(matches),'status':warning})
write('npc_packages.tsv',package_rows,['package','archive','matched_classes','model_linked_records','status'])
summary={'entities':len(records),'enemy_or_encounter_records':len(enemy_rows),
    'excluded_interaction_proxies':sum(r['eligibility']=='non_enemy_interaction_proxy' for r in enemy_rows),
    'funcspawns':len(spawn_rows),
    'spawner_classification':dict(Counter(r['classification'] for r in spawn_rows)),
    'npc_packages':len(package_rows),'enemy_classes_with_records':sum(n>0 for n in counts.values()),
    'campaign_modes':dict(Counter(str(r['mode']) for r in enemy_rows if not r['multiplayer'])),
    'campaign_vent_types':dict(Counter(r['vent_type'] for r in enemy_rows if r['mode']=='SPAWNMODE_VENT' and not r['multiplayer'])),
    'unique_campaign_enemy_guids':len({r['guid'] for r in enemy_rows if not r['multiplayer']})}
(out/'summary.json').write_text(json.dumps(summary,indent=2),encoding='utf-8')
print(json.dumps(summary,indent=2))
