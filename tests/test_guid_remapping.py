import struct
import unittest
from test_isolated_pool import fixture
from remap_pool_guids import remap, guid_bytes, guid_hash
from ds2_assets import simgroup

OLD = "00000001-00000002-00000003-00000004"
NEW = "ABCDEF01-23456789-ABCDEF01-23456789"


def source():
    data = fixture()
    records, _ = simgroup(data)
    for row in records:
        struct.pack_into("<I", data, row["offset"] + 28, guid_hash(row["guid"]))
    data.extend(guid_bytes(OLD))
    struct.pack_into("<I", data, 228, 112)
    struct.pack_into("<I", data, 252, 1)
    struct.pack_into("<H", data, 270, 1)
    struct.pack_into("<I", data, 272, 80)
    struct.pack_into("<IHBBI", data, 288, 123, 65535, 12, 255, 24)
    struct.pack_into("<H", data, 304, 0)
    return data


class RemapTests(unittest.TestCase):
    def test_hash(self):
        self.assertEqual(guid_hash(OLD), ((1 * 33 + 2) * 33 + 3) * 33 + 4)

    def test_identity_and_reference(self):
        data = source()
        result, audit = remap(data, {OLD: NEW})
        self.assertEqual(result[320:336], guid_bytes(NEW))
        self.assertEqual(simgroup(result)[0][0]["guid"], NEW)
        self.assertEqual(struct.unpack_from("<I", result, 96)[0], guid_hash(NEW))
        self.assertEqual((audit["identities"], audit["references"]), (1, 1))
        self.assertTrue(all(data[i] == result[i] for i in range(len(data)) if i not in audit["edited_offsets"]))

    def test_reject_hash_collision(self):
        with self.assertRaisesRegex(ValueError, "collision"):
            remap(source(), {OLD: NEW, "00000009-00000000-00000000-00000000": NEW})

    def test_reject_stale_source_hash(self):
        data = source()
        struct.pack_into("<I", data, 96, 0)
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            remap(data, {OLD: NEW})

    def test_reject_bad_reference(self):
        data = source()
        struct.pack_into("<I", data, 296, 9999)
        with self.assertRaisesRegex(ValueError, "bounds"):
            remap(data, {OLD: NEW})


if __name__ == "__main__":
    unittest.main()
