"""Census extracted PC STR assets and SimGroups, preserving unknown records."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import sys
from ds2_assets import assets, simgroup, loose_images

p=argparse.ArgumentParser(description=__doc__)
p.add_argument("root",type=Path)
p.add_argument("--tool-dir",required=True)
p.add_argument("--output",type=Path,required=True)
p.add_argument("--dictionary",type=Path,required=True)
p.add_argument("--limit",type=int,default=0)
p.add_argument("--only",default="",help="scan only this folder below root; archive names stay relative to root")
args=p.parse_args()
sys.path.insert(0,args.tool_dir)
import ds2str
names=json.loads(args.dictionary.read_text())['names']
def name(value): return '|'.join(names.get(f'{value:08X}',[])) or f'0x{value:08X}'
def decode_field(f,data,base):
    row=dict(f)
    row['name']=name(f['field_id'])
    signed=struct.unpack('<i',struct.pack('<I',f['value']))[0]
    offset=f['offset']+8+signed
    if f['flags'] in (1,12,16,17) and base <= offset < len(data):
        end=data.find(b'\0',offset,min(offset+256,len(data)))
        if f['flags'] in (1,16,17) and end>offset and all(32<=b<127 for b in data[offset:end]):
            row['string_candidate']=data[offset:end].decode('ascii')
        if f['flags']==12 and offset+16<=len(data):
            row['guid_candidate']='-'.join(f'{v:08X}' for v in struct.unpack_from('<4I',data,offset))
    return row
args.output.mkdir(parents=True,exist_ok=True)
states=[]
classes=Counter()
count=0
with (args.output/'entities.jsonl').open('w',encoding='utf-8') as entities_out, (args.output/'assets.jsonl').open('w',encoding='utf-8') as assets_out:
    paths=sorted((args.root/args.only if args.only else args.root).rglob('*.str'))
    if args.limit: paths=paths[:args.limit]
    for ordinal,path in enumerate(paths,1):
        state={'archive':str(path.relative_to(args.root)),'groups':0,'entities':0,'errors':[]}
        try:
            stream=ds2str.StreamSet(path)
            if not stream.pc: raise ValueError('Not a PC archive')
            for meta,chunks in assets(stream):
                meta['archive']=state['archive']
                assets_out.write(json.dumps(meta)+'\n')
                if meta['type']!='SimGroup': continue
                state['groups']+=1
                try:
                    data=stream.asset_payload(chunks,meta['size'])
                    entities,base=simgroup(data)
                    images=loose_images(data,base)
                    for entity in entities:
                        entity['archive']=state['archive']
                        entity['asset']=meta['path']
                        entity['asset_chunk_offset']=meta['chunk_offset']
                        ids=[entity['image_id']]+entity.pop('component_images')
                        entity['images']=[]
                        for image_id in ids:
                            im=images.get(image_id)
                            if im is None:
                                entity['images'].append({'image_id':image_id,'unresolved':True})
                                continue
                            row={'image_id':image_id,'type_id':im['type_id'],
                                'class':name(im['type_id']),
                                'fields':[decode_field(f,data,base) for f in im['fields']]}
                            if im.get('baked'): row['baked']=True
                            entity['images'].append(row)
                        entity['class']=entity['images'][0].get('class','unresolved')
                        classes[entity['class']]+=1
                        entities_out.write(json.dumps(entity)+'\n')
                        state['entities']+=1
                except Exception as exc:
                    state['errors'].append(meta['path']+': '+str(exc))
        except Exception as exc:
            state['errors'].append(str(exc))
        states.append(state)
        count+=state['entities']
        if ordinal%50==0: print(f'{ordinal}/{len(paths)} archives, {count} entities',flush=True)
(args.output/'scan_state.json').write_text(json.dumps(states,indent=2),encoding='utf-8')
(args.output/'class_counts.json').write_text(json.dumps(classes.most_common(),indent=2),encoding='utf-8')
print(f'Finished {len(paths)} archives, {count} entities; {sum(bool(s["errors"]) for s in states)} archives with errors')
