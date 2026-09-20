import unittest
from test_isolated_pool import fixture
from remap_parameter_images import remap_images
from ds2_assets import simgroup, loose_images


class ParameterRemapTests(unittest.TestCase):
    def test_remaps_definitions_and_references(self):
        source = fixture()
        result, audit = remap_images(source, {100: 200, 101: 201})
        records, base = simgroup(result)
        self.assertEqual([r["image_id"] for r in records], [200, 201])
        self.assertEqual(set(loose_images(result, base)), {200, 201})
        touched = {at + byte for at in audit["dword_offsets"] for byte in range(4)}
        self.assertTrue(all(source[i] == result[i] for i in range(len(source)) if i not in touched))

    def test_rejects_incomplete_map(self):
        with self.assertRaisesRegex(ValueError, "every local"):
            remap_images(fixture(), {100: 200})

    def test_rejects_collision(self):
        for mapping in ({100: 200, 101: 200}, {100: 101, 101: 200}):
            with self.assertRaisesRegex(ValueError, "collision"):
                remap_images(fixture(), mapping)

    def test_rejects_zero(self):
        with self.assertRaisesRegex(ValueError, "Invalid"):
            remap_images(fixture(), {100: 0, 101: 200})


if __name__ == "__main__":
    unittest.main()
