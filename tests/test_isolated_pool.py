import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from build_isolated_pool import isolate, isolate_many
from ds2_assets import simgroup


def fixture():
    data = bytearray(320)
    struct.pack_into("<IIII", data, 4, 224, 60, 68, 0)
    struct.pack_into("<HHH", data, 0x14, 2, 2, 0)
    struct.pack_into("<I", data, 0x24, 0xCCB51828)
    struct.pack_into("<I", data, 0x28, 0x00010003)
    struct.pack_into("<II", data, 60, 100, 101)
    for i in range(2):
        at = 68 + i * 68
        struct.pack_into("<I", data, at + 8, 60 + i * 4)
        struct.pack_into("<4I", data, at + 12, i + 1, 2, 3, 4)
    struct.pack_into("<IIIHHIIII", data, 224, 0x91C8773C, 96, 32,
                     1, 0, 40, 2, 64, 0)
    struct.pack_into("<I", data, 256, 123)
    struct.pack_into("<IHHI", data, 264, 100, 0, 0, 64)
    struct.pack_into("<IHHI", data, 276, 101, 0, 0, 64)
    return data


class IsolationTests(unittest.TestCase):
    def test_rejects_wrong_native_version(self):
        data = fixture()
        struct.pack_into("<I", data, 0x28, 0)
        with self.assertRaisesRegex(ValueError, "version"):
            isolate(data, 0)

    def test_dependency_group_retains_both_records(self):
        source = fixture()
        result, selected, unused = isolate_many(source, [1, 0])
        records, base = simgroup(result)
        self.assertEqual([r["image_id"] for r in records], [100, 101])
        self.assertEqual(result[base:], source[base:])
        self.assertEqual(len(selected), 2)
        self.assertEqual(unused, 0)
        self.assertEqual(source, fixture())

    def test_rejects_duplicate_or_empty_selection(self):
        for indices in ([], [0, 0]):
            with self.assertRaises(ValueError):
                isolate_many(fixture(), indices)

    def test_second_record_keeps_offsets_and_parameter_data(self):
        source = fixture()
        result, selected, unused = isolate(source, 1)
        records, base = simgroup(result)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["guid"], selected["guid"])
        self.assertEqual(records[0]["image_id"], 101)
        self.assertEqual(result[base:], source[base:])
        self.assertEqual(result[136:204], bytes(68))
        self.assertEqual(unused, 1)

    def test_rejects_automatic_spawn_flags(self):
        source = fixture()
        struct.pack_into("<I", source, 68 + 0x2C, 1)
        with self.assertRaisesRegex(ValueError, "spawn flags"):
            isolate(source, 0)

    def test_rejects_missing_image(self):
        source = fixture()
        struct.pack_into("<I", source, 60, 999)
        with self.assertRaisesRegex(ValueError, "parameter image"):
            isolate(source, 0)

    def test_rejects_bad_index(self):
        with self.assertRaisesRegex(ValueError, "index"):
            isolate(fixture(), 2)


if __name__ == "__main__":
    unittest.main()
