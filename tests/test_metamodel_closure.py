import unittest
import struct
from test_metamodel_dependencies import fixture
from resolve_metamodel_closure import walk
from ds2_assets import sdbm

GUID = '00000004-00000005-00000006-00000007'


class ClosureTests(unittest.TestCase):
    def test_self_cycle_is_expanded_once(self):
        data = fixture()
        struct.pack_into('<I', data, 0x70, sdbm('MetaModel'))
        nodes, edges = walk([(GUID, sdbm('MetaModel'))], lambda g, t: [{'type': 'MetaModel'}], lambda m: data)
        self.assertEqual(len(nodes), 1)
        self.assertEqual(len(edges), 1)
        self.assertTrue(nodes[0]['expanded'])

    def test_missing_target_is_explicit(self):
        nodes, _ = walk([(GUID, 3)], lambda g, t: [], lambda m: None)
        self.assertEqual(nodes[0]['status'], 'unresolved')

    def test_conflicting_model_payloads_stop_traversal(self):
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            walk([(GUID, sdbm('MetaModel'))], lambda g, t: [b'a', b'b'], lambda m: m)

    def test_bound_does_not_silently_truncate(self):
        with self.assertRaisesRegex(ValueError, 'limit'):
            walk([(GUID, 3)], lambda g, t: [], lambda m: None, limit=0)


if __name__ == '__main__':
    unittest.main()
