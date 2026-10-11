#!/usr/bin/env python3
"""Explicit release-only schema tests; requires requirements-sbom.txt."""

import copy
import unittest

from generate_sbom import document_namespace
from test_sbom import fixture_document
from validate_sbom import validate_schema, validate_semantics


class SchemaTests(unittest.TestCase):
    def test_generated_document_passes_official_schema_and_semantics(self):
        document = fixture_document()
        validate_schema(document)
        validate_semantics(document)

    def test_missing_creation_time_fails_official_schema(self):
        document = fixture_document()
        document["creationInfo"].pop("created")
        with self.assertRaisesRegex(ValueError, "SPDX schema"):
            validate_schema(document)

    def test_schema_valid_defects_fail_semantic_validation(self):
        original = fixture_document()
        for defect in ("SHA1", "verification", "duplicate ID", "dangling relationship", "created format"):
            document = copy.deepcopy(original)
            if defect == "SHA1":
                document["files"][0]["checksums"].pop(0)
            elif defect == "verification":
                document["packages"][0].pop("packageVerificationCode")
            elif defect == "duplicate ID":
                document["files"][0]["SPDXID"] = document["files"][1]["SPDXID"]
            elif defect == "dangling relationship":
                document["relationships"][1]["relatedSpdxElement"] = "SPDXRef-Missing"
            else:
                document["creationInfo"]["created"] = "2026-09-07T12:34:56+00:00"
            document["documentNamespace"] = document_namespace(document)
            with self.subTest(defect=defect):
                validate_schema(document)
                with self.assertRaises(ValueError):
                    validate_semantics(document)


if __name__ == "__main__":
    unittest.main()
