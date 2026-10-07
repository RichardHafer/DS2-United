"""Compare the faulting mesh header with packaged resource payloads."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
from ds2_paths import MODDING_TOOLS
sys.path[:0] = [str(ROOT.parent / 'trainer'),
    str(MODDING_TOOLS)]
from analyze_minidump import Dump
from ds2_assets import assets
import ds2str

dump = Dump(Path('C:/DS2CrashDumps/deadspace2.exe.30384.dmp'))
address = 0x23427000
wanted = dump.read(address, 128)
reader = ds2str.StreamSet.__new__(ds2str.StreamSet)
reader.data = (ROOT / 'analysis/runtime_stream/ds2_randomizer_pool.str').read_bytes()
reader.pc, reader.end, reader.hdrlen = True, '<', 12
for meta, chunks in assets(reader):
    payload = reader.asset_payload(chunks, meta['size'])
    if payload[:4] != wanted[:4]:
        continue
    differences = [i for i, (a, b) in enumerate(zip(payload[:128], wanted)) if a != b]
    print(f'header_differences={differences}')
    print('disk_header=' + payload[:128].hex())
    print(meta)
    equal = all(dump.read(address + i, 1) == payload[i:i + 1] for i in range(len(payload)))
    print(f'payload_bytes={len(payload)} full_payload_identical={equal}')
    print(f'fault_field_68={dump.u32(address + 0x68):08X}')
