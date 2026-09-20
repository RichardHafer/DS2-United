import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from build_spawn_compatibility import arrival, classify


def record(**changes):
    value = dict(guid="source", model_guid="model1", context="SPAWNMODE_VENT|Wall",
                 mode="SPAWNMODE_VENT", vent_type="Wall", vent_matches="1", vent_guid="vent1",
                 vent_components="ZombieVentComponent", inactive_animation="", active_animation="",
                 eligibility="ordinary_candidate_unverified_swap", **{"class": "Zombie"})
    value.update(changes)
    return value


class CompatibilityTests(unittest.TestCase):
    def test_different_vent_needs_adapter(self):
        self.assertEqual(classify(record(), record(guid="donor", model_guid="model2", vent_guid="vent2"), set()),
                         "requires_source_vent_rebinding")

    def test_exact_pair(self):
        self.assertEqual(classify(record(), record(guid="donor", model_guid="model2"), {("source", "donor")}),
                         "authored_same_vent_candidate")

    def test_other_orientation_rejected(self):
        self.assertEqual(classify(record(), record(model_guid="model2", vent_type="Ceiling"), set()),
                         "arrival_not_matched")

    def test_animations_stay_separate(self):
        self.assertEqual(arrival(record(active_animation="jump")), "authored_animation")

    def test_missing_and_duplicate_components(self):
        for components in ("", "ZombieVentComponent|ZombieVentComponent"):
            self.assertTrue(arrival(record(vent_components=components)).startswith("unverified:"))

    def test_protected_classes(self):
        self.assertEqual(classify(record(eligibility="protected", **{"class": "Guardian"}),
                                  record(model_guid="model2"), set()), "protected_class")

    def test_direct_requires_no_animation(self):
        source = record(context="default_NONE_descriptor")
        target = record(context="SPAWNMODE_NONE", model_guid="model2")
        self.assertEqual(classify(source, target, set()), "direct_candidate")
        target["inactive_animation"] = "idle"
        self.assertEqual(classify(source, target, set()), "arrival_not_matched")


if __name__ == "__main__":
    unittest.main()
