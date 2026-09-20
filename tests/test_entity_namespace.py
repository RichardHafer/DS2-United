import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from namespace_pool_entities import plan
from remap_pool_guids import guid_hash

A = '00000001-00000002-00000003-00000004'
B = '00000002-00000002-00000003-00000004'


def fixture():
    groups = [dict(archive=archive, asset='asset', file=archive,
                   entities=[dict(index=0, guid=A)]) for archive in ('one', 'two')]
    graph = dict(roots=[dict(truncated=False, nodes=[], edges=[])])
    return groups, graph


class EntityNamespaceTests(unittest.TestCase):
    def test_repeated_guid_keeps_distinct_occurrences(self):
        groups, graph = fixture()
        maps, ids, _, _ = plan(groups, graph, [])
        self.assertNotEqual(maps['one'][A], maps['two'][A])
        self.assertEqual(ids, plan(list(reversed(groups)), graph, [])[1])
        occupied = guid_hash(maps['one'][A])
        self.assertNotIn(occupied, [guid_hash(g) for g in plan(groups, graph, [occupied])[1].values()])

    def test_cross_group_reference_uses_target_occurrence(self):
        groups, graph = fixture()
        groups[0]['entities'][0]['guid'] = B
        graph['roots'][0]['edges'] = [dict(source_archive='one', source=B,
            target_archive='two', target=A, status='entity')]
        maps, _, edges, external = plan(groups, graph, [])
        self.assertEqual(maps['one'][A], maps['two'][A])
        self.assertEqual(len(edges), 1)
        self.assertFalse(external)

    def test_rejects_conflicting_local_identity(self):
        groups, graph = fixture()
        graph['roots'][0]['edges'] = [dict(source_archive='one', source=A,
            target_archive='two', target=A, status='entity')]
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            plan(groups, graph, [])

    def test_only_flagged_missing_targets_stay_external(self):
        groups, graph = fixture()
        graph['roots'][0]['edges'] = [dict(source_archive='one', source=A,
            target_archive='outside', target=B, status='entity')]
        with self.assertRaisesRegex(ValueError, 'Missing target'):
            plan(groups, graph, [])
        graph['roots'][0]['nodes'] = [dict(archive='outside', guid=B, spawn_flags=1)]
        maps, _, _, external = plan(groups, graph, [])
        self.assertNotIn(B, maps['one'])
        self.assertEqual(len(external), 1)


if __name__ == '__main__':
    unittest.main()
