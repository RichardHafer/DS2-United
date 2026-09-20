import io
import struct
import unittest
from test_isolated_pool import fixture
from ds2_assets import simgroup
from inventory_parameter_ids import image_ids, read_asset
from namespace_pool_parameters import allocate


class InventoryTests(unittest.TestCase):
    def test_reads_all_definitions(self):
        data = fixture()
        _, base = simgroup(data)
        self.assertEqual(image_ids(data, base), [100, 101])

    def test_rejects_duplicate_definition(self):
        data = bytearray(fixture())
        _, base = simgroup(data)
        table = base + struct.unpack_from('<I', data, base + 16)[0]
        struct.pack_into('<I', data, table + 12, 100)
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            image_ids(data, base)

    def test_reads_split_uncompressed_payload(self):
        data = fixture()
        names = b'group\0path\0SimGroup\0'
        header = bytearray(44)
        header[:4], header[8:12] = b'COHS', b'RDHS'
        struct.pack_into('<I', header, 4, 44 + len(names))
        struct.pack_into('<I', header, 24, 123)
        struct.pack_into('<I', header, 40, len(data))
        chunks = b''.join(b'COHS' + struct.pack('<I', len(part) + 12) + b'TADS' + part
                          for part in (data[:80], data[80:]))
        meta = dict(chunk_offset=0, resource_id=123, size=len(data), path='path')
        self.assertEqual(read_asset(io.BytesIO(header + names + chunks), meta, None), data)
        with self.assertRaisesRegex(ValueError, 'Truncated'):
            read_asset(io.BytesIO(header + names + chunks[:-1]), meta, None)

    def test_allocation_is_stable_and_unique(self):
        groups = {'b': [100], 'a': [100, 101]}
        first = allocate(groups, {200})
        self.assertEqual(first, allocate(dict(reversed(list(groups.items()))), {200}))
        values = [value for mapping in first.values() for value in mapping.values()]
        self.assertEqual(len(set(values)), 3)
        self.assertFalse(set(values) & {0, 100, 101, 200, 0xFFFFFFFF})
        occupied = first['a'][100]
        self.assertNotIn(occupied, allocate(groups, {occupied})['a'].values())


if __name__ == '__main__':
    unittest.main()
