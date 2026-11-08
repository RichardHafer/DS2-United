"""Check peer template comparison against lifecycle and identity failures."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from compare_coop_world import compare, REQUIRED_CODE
from ds2_assets import sdbm


def snapshot():
    return dict(errors=[], source="test", functions={n: dict(sha256="a" * 64) for n in REQUIRED_CODE},
        templates=[dict(node=0x123400, guid=[1, 0, 0, 0], hash=35937,
                        class_id=sdbm("Zombie"), name="Zombie", flags=0)])


class WorldComparison(unittest.TestCase):
    def setUp(self):
        self.host = snapshot()
        self.client = copy.deepcopy(self.host)

    def compare(self, *classes):
        return compare(self.host, self.client, classes)

    def test_heap_addresses_do_not_define_identity(self):
        self.client["templates"][0]["node"] = 0x765400
        self.assertEqual(self.compare()["status"], "template_prerequisites_match")

    def test_different_guid_with_same_hash_is_rejected(self):
        self.client["templates"][0]["guid"] = [0, 0, 0, 35937]
        self.assertEqual(self.compare()["conflicts"][0]["fields"], ["guid"])

    def test_class_change_is_rejected(self):
        item = self.client["templates"][0]
        item.update(name="Puker", class_id=sdbm("Puker"))
        self.assertEqual(self.compare()["conflicts"][0]["fields"], ["class_id"])

    def test_flag_change_is_rejected(self):
        self.client["templates"][0]["flags"] = 16
        self.assertEqual(self.compare()["conflicts"][0]["fields"], ["flags"])

    def test_missing_template_is_reported(self):
        self.client["templates"][0].update(guid=[0, 0, 0, 9], hash=9)
        self.assertEqual(len(self.compare()["missing_on_client"]), 1)

    def test_duplicate_hash_is_not_silently_overwritten(self):
        self.client["templates"] *= 2
        self.assertTrue(self.compare()["client_errors"])
        self.assertEqual(self.compare()["conflicts"][0]["reason"], "ambiguous_client_hash")

    def test_hash_is_recomputed(self):
        self.client["templates"][0].update(hash=10, hash_valid=True)
        self.assertIn("invalid_hash:0", self.compare()["client_errors"])

    def test_missing_requested_class_does_not_pass_vacuously(self):
        self.assertIn("class_not_present:Puker", self.compare("Puker")["selection_errors"])

    def test_incomplete_capture_is_rejected(self):
        self.client["errors"] = ["unreadable_node"]
        self.assertEqual(self.compare()["status"], "template_prerequisites_not_met")

    def test_absent_code_evidence_is_rejected(self):
        self.host["functions"] = {}
        self.client["functions"] = {}
        self.assertEqual(len(self.compare()["code_mismatches"]), len(REQUIRED_CODE))

    def test_empty_capture_does_not_pass(self):
        self.host["templates"] = []
        self.assertEqual(self.compare()["status"], "template_prerequisites_not_met")

    def test_selection_ignores_other_valid_templates(self):
        extra = copy.deepcopy(self.host["templates"][0])
        extra.update(guid=[0, 0, 0, 2], hash=2, name="Puker", class_id=sdbm("Puker"))
        self.host["templates"].append(extra)
        self.assertEqual(self.compare("Zombie")["status"], "template_prerequisites_match")
        self.assertEqual(self.compare()["status"], "template_prerequisites_not_met")


if __name__ == "__main__":
    unittest.main()
