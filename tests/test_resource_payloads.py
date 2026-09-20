import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from audit_resource_payloads import classify


class PayloadTests(unittest.TestCase):
    def test_identical_payloads(self):
        row = dict(sha256='abc', type='RCB')
        status, variants = classify([row, row])
        self.assertEqual(status, 'identical')
        self.assertEqual(len(variants[0]), 2)

    def test_identity_does_not_hide_payload_or_type_conflicts(self):
        row = dict(sha256='abc', type='RCB')
        for other in (dict(sha256='def', type='RCB'), dict(sha256='abc', type='VFX')):
            self.assertEqual(classify([row, other])[0], 'conflicting')


if __name__ == '__main__':
    unittest.main()
