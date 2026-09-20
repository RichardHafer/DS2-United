"""Exercise DS2 bounds and table layouts with synthetic records."""
from pathlib import Path
import struct
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from ds2_assets import simgroup,loose_images,sdbm

class AssetsTests(unittest.TestCase):
    def fixture(self):
        d=bytearray(0x90+52)
        struct.pack_into('<IIIIIHHHH',d,0,0,0x84,0x3C,0x40,0,1,1,0,0)
        struct.pack_into('<II',d,0x24,0xCCB51828,0x10003)
        struct.pack_into('<I',d,0x3C,0x12345678)
        struct.pack_into('<I4I',d,0x48,0x3C,1,2,3,4)
        struct.pack_into('<IIIIIIII',d,0x90,0x91C8773C,52,32,1,40,1,52,0)
        struct.pack_into('<II',d,0xB0,sdbm('Zombie'),1)
        struct.pack_into('<IHHI',d,0xB8,0x12345678,0,0,52)
        return d
    def test_entity_and_null_components(self):
        entities,base=simgroup(self.fixture())
        self.assertEqual(base,0x90)
        self.assertEqual(entities[0]['guid'],'00000001-00000002-00000003-00000004')
        self.assertEqual(entities[0]['component_images'],[])
        self.assertEqual(loose_images(self.fixture(),base)[0x12345678]['type_id'],sdbm('Zombie'))
    def test_reject_bad_magic(self):
        d=self.fixture();d[0x24]=0
        with self.assertRaises(ValueError): simgroup(d)
    def test_reject_bad_reference(self):
        d=self.fixture();struct.pack_into('<I',d,0x48,0xFFFFFFF0)
        with self.assertRaises(ValueError): simgroup(d)
    def test_reject_bad_image_type(self):
        d=self.fixture();struct.pack_into('<H',d,0xBC,2)
        with self.assertRaises(ValueError): loose_images(d,0x90)
    def test_reject_truncation(self):
        with self.assertRaises(ValueError): loose_images(self.fixture()[:-1],0x90)

if __name__=='__main__': unittest.main()
