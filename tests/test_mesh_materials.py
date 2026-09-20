import struct
import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from mesh_materials import mesh_material_hashes, material_hash, material_texture_paths


class MeshMaterialTests(unittest.TestCase):
    def test_material_hash_and_texture_pair_path(self):
        data = bytearray(112)
        struct.pack_into('<III', data, 0, 0x0AA0DEED, 19, 0x66A81894)
        data += b's:/assets/projectiles/chars/brutepod/textures/brutepod_c_tdf.xml\0'
        self.assertEqual(material_hash(data), 0x66A81894)
        self.assertEqual(material_texture_paths(data), ['projectiles\\chars\\brutepod\\textures\\brutepod_c'])

    def test_mesh_table(self):
        data = bytearray(0x260)
        struct.pack_into('<4sI', data, 0, b'MGAE', 50)
        struct.pack_into('<I', data, 0x34, 2)
        struct.pack_into('<I', data, 0x50, 0xE0)
        struct.pack_into('<I', data, 0xEC, 0x66A81894)
        struct.pack_into('<I', data, 0x1AC, 0x66A81894)
        self.assertEqual(mesh_material_hashes(data), [0x66A81894])
        struct.pack_into('<I', data, 0x50, 0x260)
        with self.assertRaises(ValueError):
            mesh_material_hashes(data)

    def test_unsupported_headers(self):
        with self.assertRaises(ValueError):
            material_hash(bytes(112))
        with self.assertRaises(ValueError):
            mesh_material_hashes(bytes(224))
