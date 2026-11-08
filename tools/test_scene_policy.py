import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from build_scene_policy import ISAAC, campaign_actor_cues, message_hash


class ActorCueTests(unittest.TestCase):
    def candidates(self, extras=(), *, target="elle", fields=(), reserved=()):
        def record(guid, cls, attributes, group="ch04_asc"):
            return {"guid": guid, "asset": f"levels\\{group}\\area", "images": [{"class": cls, "fields": attributes}]}

        def text(key, value):
            return {"name": key, "string_candidate": value}

        def sequence(guid, actor, group="ch04_asc"):
            return record(guid, "ScriptedSequence", [
                text("CMD_SetTargetName", "test_cue"), {"name": "CMD_SetScriptedSequenceTarget", "guid_candidate": actor},
                text("CMD_SetActionAnimationString", "npc_idle"),
                *fields,
            ], group)

        records = [record("elle", "NeutralNPC", []), record(ISAAC, "Player", []), sequence("seq", target),
                   record("trigger", "TriggerPhysical", [text("CMD_SetBroadcast1Message", "test_cue")])]
        for kind, field, value in extras:
            if kind == "second_sequence":
                records.append(sequence("seq_two", field, value))
            else:
                records.append(record("other", kind, [text(field, value)]))
        with TemporaryDirectory() as temp:
            path = Path(temp) / "entities.jsonl"
            path.write_text("\n".join(map(json.dumps, records)), encoding="utf-8")
            return campaign_actor_cues([path], set(reserved))

    def test_only_world_actors(self):
        self.assertEqual(self.candidates(), [(message_hash("test_cue"), "test_cue")])
        self.assertFalse(self.candidates(target="unresolved"))
        self.assertFalse(self.candidates(extras=[("second_sequence", ISAAC, "ch04_asc")]))
        self.assertFalse(self.candidates(extras=[("second_sequence", "elle", "mp_detonate")]))
        self.assertFalse(self.candidates(reserved=["test_cue"]))

    def test_other_inputs_and_indirect_effects(self):
        for cls, key in [("PushCamera", "CMD_SetTargetName"), ("PopCamera", "CMD_AlternateInput"),
                         ("TriggerPhysical", "CMD_SetDeactivateMessage"), ("MultiManager", "CMD_SetTarget1")]:
            with self.subTest(cls=cls, field=key):
                self.assertFalse(self.candidates([(cls, key, "TEST_CUE")]))
        for key, value in [("CMD_SetTarget", "next_scene"), ("CMD_SetSyncCamera", "camera"),
                           ("CMD_SetSyncAnimated0", ISAAC), ("CMD_SetScriptedSequenceSettingsFlags", "SSEQFL_MESSAGE_SENDER_IS_TARGET")]:
            with self.subTest(field=key):
                self.assertFalse(self.candidates(fields=[{"name": key, "string_candidate": value}]))


if __name__ == "__main__":
    unittest.main()
