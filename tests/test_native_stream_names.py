import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from map_native_character_streams import name_offsets, stream_name


class NativeStreamNames(unittest.TestCase):
    def test_archive_container_is_not_part_of_name(self):
        self.assertEqual(stream_name(r"ds2dat10\global_assets\npc\brute.str"),
                         r"global_assets\npc\brute")

    def test_requires_full_terminated_name(self):
        self.assertEqual(name_offsets(b"\0foo\0xfoo\0foo_extra\0foo\0", "foo"), [1, 20])

    def test_rejects_non_archive(self):
        with self.assertRaises(ValueError):
            stream_name(r"global_assets\npc\brute.str")


if __name__ == "__main__":
    unittest.main()
