"""Pure, portable licence precedence and independent admission evidence."""
from copy import deepcopy
import itertools
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from luminumbra_author.contracts import digest, Refusal
from luminumbra_author.meshgen_backend import MOCK_CAPABILITY, validate_capability
from luminumbra_author.meshgen_licence import gate, candidate_eligibility
from luminumbra_author.meshgen_registry import defaults


class LicenceTests(unittest.TestCase):
    def setUp(self):
        self.card = deepcopy(MOCK_CAPABILITY)
        self.policy = deepcopy(defaults()["policy"]["licence"])
        self.policy["allowed_status"] = ["shippable", "internal_ok", "internal_only", "counsel"]
        self.licence = self.card["components"][0]["licence"]
        self.notes = {}

    def assertFindings(self, result, expected):
        self.assertEqual({item["rule_id"] for item in result["findings"]}, set(expected))
        self.assertEqual(result["runnable"], not expected)

    def ack(self):
        self.policy["internal_acknowledgements"] = [{"licence_id": self.licence["id"],
            "text_sha256": self.licence["text_sha256"], "acknowledged_by": "fixture-reviewer",
            "acknowledged_at": "2026-10-13", "scope": "internal_only"}]

    def attest(self, name, *, verified=True):
        self.policy["attestations"][name] = {"value": True, "attested_by": "fixture-reviewer",
            "attested_at": "2026-10-13", "note_id": name, "note_sha256": digest(b"fixture evidence")}
        if verified:
            self.notes[name] = b"fixture evidence"

    def test_all_precedence_combinations_retain_reasons(self):
        triggers = [("non_commercial", "blocked"), ("territory_restricted", "internal_only"),
                    ("unknown", "counsel"), ("copyleft_dependency", "internal_ok")]
        for subset in itertools.product((False, True), repeat=len(triggers)):
            with self.subTest(subset=subset):
                chosen = [trigger for use, trigger in zip(subset, triggers) if use]
                self.licence["clauses"] = [clause for clause, _ in chosen]
                result = gate(self.card, self.policy)
                self.assertEqual(result["status"], chosen[0][1] if chosen else "shippable")
                self.assertEqual(len(result["reasons"]), len(chosen))
                self.assertEqual(result, gate(self.card, self.policy))
                expected = set()
                if chosen: expected.add("licence.ack_missing")
                if "non_commercial" in self.licence["clauses"]: expected.add("licence.status_not_allowed")
                if "unknown" in self.licence["clauses"]: expected.add("licence.counsel_missing")
                self.assertFindings(result, expected)

    def test_audited_unaudited_and_backend_specific_waiver(self):
        self.licence["clauses"] = ["attribution_required", "use_restrictions"]
        self.assertEqual(gate(self.card, self.policy)["status"], "shippable")
        self.card["dataset_chain"]["audited"] = False
        self.assertEqual(gate(self.card, self.policy)["status"], "internal_ok")
        self.policy["dataset_chain_waivers"] = [{"backend_id": "other", "reason": "fixture",
            "recorded_by": "fixture-reviewer", "recorded_at": "2026-10-13"}]
        self.assertEqual(gate(self.card, self.policy)["status"], "internal_ok")
        self.policy["dataset_chain_waivers"][0]["backend_id"] = "mock"
        self.assertEqual(gate(self.card, self.policy)["status"], "shippable")

    def test_acknowledgements_bind_id_text_hash_and_scope(self):
        self.licence["clauses"] = ["territory_restricted"]
        self.assertFindings(gate(self.card, self.policy), {"licence.ack_missing"})
        self.ack()
        self.assertTrue(gate(self.card, self.policy)["runnable"])
        for key, changed in (("licence_id", "other"), ("text_sha256", "0"*64)):
            with self.subTest(key=key):
                original = self.policy["internal_acknowledgements"][0][key]
                self.policy["internal_acknowledgements"][0][key] = changed
                self.assertFindings(gate(self.card, self.policy), {"licence.ack_missing"})
                self.policy["internal_acknowledgements"][0][key] = original
        self.assertFindings(gate(self.card, defaults()["policy"]["licence"]),
                            {"licence.status_not_allowed", "licence.ack_missing"})

    def test_verified_and_false_attestations_never_bypass_prerequisites(self):
        self.licence["clauses"] = ["revenue_cap", "no_improve_ai", "gated_access"]
        self.ack()
        for name in ("revenue_under_cap", "no_ai_improvement", "registration_done"):
            self.attest(name, verified=False)
        result = gate(self.card, self.policy, self.notes)
        self.assertEqual(result["status"], "internal_only")
        rules = {item["rule_id"] for item in result["findings"]}
        self.assertEqual({"licence.prerequisite_unverified", "config.attestation_unverified"}, rules)
        for name in self.policy["attestations"]:
            self.notes[name] = b"fixture evidence"
        result = gate(self.card, self.policy, self.notes)
        self.assertEqual(result["status"], "internal_ok")
        self.assertTrue(result["runnable"])
        self.assertFalse(result["shippable"])
        for name in self.policy["attestations"]:
            with self.subTest(name=name):
                self.notes[name] = b"changed note"
                self.assertFindings(gate(self.card, self.policy, self.notes),
                                    {"licence.prerequisite_unverified", "config.attestation_unverified"})
                self.notes[name] = b"fixture evidence"

    def test_revenue_clause_requires_registration_independently(self):
        self.licence["clauses"] = ["revenue_cap"]
        self.ack()
        self.attest("revenue_under_cap")
        self.assertFindings(gate(self.card, self.policy, self.notes), {"licence.prerequisite_unverified"})
        self.attest("registration_done")
        self.assertTrue(gate(self.card, self.policy, self.notes)["runnable"])

    def test_counsel_notes_verify_bytes_and_never_upgrade_shipping(self):
        self.licence["clauses"] = ["unknown", "copyleft_dependency"]
        self.ack()
        self.policy["counsel_notes"] = [{"counsel_note_id": "counsel", "covers_licence_ids": [self.licence["id"]],
            "text_sha256": digest(b"cleared internally"), "recorded_by": "fixture-reviewer",
            "recorded_at": "2026-10-13", "outcome": "cleared_internal"}]
        self.assertFindings(gate(self.card, self.policy), {"licence.counsel_missing"})
        self.notes["counsel"] = b"cleared internally"
        result = gate(self.card, self.policy, self.notes)
        self.assertTrue(result["runnable"])
        self.assertEqual(result["status"], "counsel")
        self.assertFalse(result["shippable"])

    def test_missing_conflicting_and_unexecuted_inventory(self):
        self.card["components"] = []
        self.assertEqual(gate(self.card, self.policy)["status"], "counsel")
        self.card["components"] = [{"name": "undeclared"}]
        self.assertEqual(gate(self.card, self.policy)["status"], "counsel")
        self.card = deepcopy(MOCK_CAPABILITY)
        other = deepcopy(self.card["components"][0])
        other["licence"]["text_sha256"] = "0"*64
        self.card["components"].append(other)
        self.assertEqual(gate(self.card, self.policy)["status"], "counsel")
        other["licence"].update(executed=False, clauses=["non_commercial"])
        self.assertEqual(gate(self.card, self.policy)["status"], "shippable")

    def test_nc_component_or_dataset_cannot_be_waived(self):
        self.ack()
        self.policy["allowed_status"].append("blocked")
        for location in (self.licence, self.card["dataset_chain"]):
            with self.subTest(location=location):
                location["clauses"] = ["non_commercial"]
                self.assertFindings(gate(self.card, self.policy), {"licence.status_not_allowed"})
                self.assertEqual(gate(self.card, self.policy)["status"], "blocked")
                location["clauses"] = []

    def test_mock_permission_and_forced_nonshipping_are_distinct(self):
        result = gate(MOCK_CAPABILITY, defaults()["policy"]["licence"])
        self.assertTrue(result["runnable"])
        self.assertTrue(result["shippable"])
        self.assertEqual(candidate_eligibility(result, "mock", "mock"), {"shippable": False, "origin": "backend:mock"})
        validate_capability(MOCK_CAPABILITY, "mock", {"kind": "mock"})
        forged = deepcopy(MOCK_CAPABILITY)
        forged["resources"]["vram_peak_mib_estimate"] = 0
        with self.assertRaises(Refusal) as refusal:
            validate_capability(forged, "mock", {"kind": "mock"})
        self.assertEqual(refusal.exception.finding["rule_id"], "backend.pin_mismatch")


if __name__ == "__main__":
    unittest.main()
