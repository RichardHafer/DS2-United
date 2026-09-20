"""Compare the captured loader payload with its serialized parameter tables."""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'trainer'))
sys.path.insert(0, r'D:\SteamLibrary\steamapps\common\Dead Space 2\Dead Space 2 Modding Tools')
from analyze_minidump import Dump
from ds2_assets import assets, simgroup, u32
import ds2str

dump = Dump(Path(r'C:\DS2CrashDumps\deadspace2.exe.29336.dmp'))
stream = ds2str.StreamSet(ROOT / 'analysis/runtime_stream/ds2_randomizer_pool.str')
path = r'randomizer\pool\5fea1ca4784515be46cf864d.simgroup.bin'
meta, chunks = next((m, c) for m, c in assets(stream) if m['path'] == path)
serialized = stream.asset_payload(chunks, meta['size'])
_, base = simgroup(serialized)
address = 0x23B6FE50
expected = bytearray(serialized)
parameter_address = address + base
table = base + u32(serialized, base + 16)
count = u32(serialized, base + 20)
for offset in (8, 16, 24):
    struct.pack_into('<I', expected, base + offset,
                     parameter_address + u32(serialized, base + offset))
for index in range(count):
    at = table + index * 12 + 8
    value = u32(serialized, at)
    if value:
        struct.pack_into('<I', expected, at, parameter_address + value)
actual = b''.join(dump.read(address + i, 1) or b'\0' for i in range(len(expected)))
rows = []
for index in range(count):
    at = table + index * 12
    rows.append({'index': index, 'address': hex(address + at),
                 'serialized': serialized[at:at + 12].hex(),
                 'expected': expected[at:at + 12].hex(),
                 'actual': actual[at:at + 12].hex(),
                 'matches': actual[at:at + 12] == expected[at:at + 12]})
blocks = []
for at in range(0, len(expected), 128):
    end = min(at + 128, len(expected))
    blocks.append({'offset': hex(at), 'address': hex(address + at),
                   'equal_bytes': sum(a == b for a, b in zip(actual[at:end], expected[at:end])),
                   'bytes': end - at})
report = {'asset': meta, 'payload_address': hex(address),
          'parameter_address': hex(parameter_address), 'images': rows, 'blocks': blocks,
          'note': 'Class descriptor resolution is not normalized. Missing dump bytes become zero.'}
(ROOT / 'analysis/crash_29336_payload_comparison.json').write_text(json.dumps(report, indent=2))
print('Asset:', path, 'bytes:', len(expected))
print('Image table matches:', sum(r['matches'] for r in rows), '/', count)
for row in rows:
    if not row['matches']:
        print(row)
print('128-byte blocks:', blocks)
dump.close()
