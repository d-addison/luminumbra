#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from xml.sax.saxutils import escape


MODULE_PATH = Path(__file__).with_name("compare_ambient_receipts.py")
SPEC = importlib.util.spec_from_file_location("compare_ambient_receipts", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


BASE_FIELDS = {
    "seed": "424242",
    "ticks": "64",
    "ceiling": "256",
    "cpu_simd": "256",
    "active_simd": "256",
    "wind_ctor": "aa11",
    "wind_evolved": "bb22",
    "weather_ctor": "cc33",
    "weather_evolved": "dd44",
    "aether_ctor": "ee55",
    "aether_evolved": "ff66",
}


def receipt_line(**overrides):
    fields = dict(BASE_FIELDS)
    fields.update(overrides)
    ordered = ["version=1"] + [f"{key}={fields[key]}" for key in BASE_FIELDS]
    return "AMBIENT_RECEIPT " + " ".join(ordered)


def write_junit(
    directory,
    name,
    system_out_lines=(),
    property_values=(),
    attribute_value=None,
    failed=False,
):
    """Write a synthetic CTest JUnit file with optional receipt carriers."""
    quote = {'"': "&quot;"}
    body = "\n".join(escape(line) for line in system_out_lines)
    properties = "".join(
        '<property name="ambient_receipt" value="{}"/>'.format(escape(value, quote))
        for value in property_values
    )
    attribute = ""
    if attribute_value is not None:
        attribute = ' ambient_receipt="{}"'.format(escape(attribute_value, quote))
    failure = '<failure message="synthetic failure"/>' if failed else ""
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuites><testsuite name="common_tests" tests="1" failures="0">'
        '<testcase name="EmitsConstructorAndEvolvedHashes" '
        f'classname="AmbientDeterminismReceipt"{attribute}>'
        f"<properties>{properties}</properties>"
        f"{failure}"
        "</testcase>"
        f"<system-out>{body}</system-out>"
        "</testsuite></testsuites>\n"
    )
    path = Path(directory) / name
    path.write_text(xml, encoding="utf-8")
    return str(path)


def run_main(argv):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = MODULE.main(argv)
    return code, out.getvalue(), err.getvalue()


def add_probe(path, status="ran", level="256", carrier="output", outcome=None):
    tree = ET.parse(path)
    case = ET.SubElement(
        tree.getroot().find("testsuite"),
        "testcase",
        name=MODULE.PROBE_TEST,
    )
    if carrier == "output":
        ET.SubElement(case, "system-out").text = (
            f"AMBIENT_PROBE probe_status={status} active_simd={level}"
        )
    elif carrier == "attribute":
        case.set("probe_status", status)
        case.set("noise_simd_level", level)
    else:
        properties = ET.SubElement(case, "properties")
        ET.SubElement(properties, "property", name="probe_status", value=status)
        ET.SubElement(properties, "property", name="noise_simd_level", value=level)
    if outcome:
        ET.SubElement(case, outcome)
    tree.write(path, encoding="utf-8")


class CompareAmbientReceiptsTests(unittest.TestCase):
    def test_attributed_receipt_keeps_its_producers_execution_status(self):
        for carrier in ("attribute", "property", "output"):
            for outcome in (None, "failure", "error", "skipped", "disabled", "unknown"):
                with self.subTest(carrier=carrier, outcome=outcome), tempfile.TemporaryDirectory() as directory:
                    lane = write_junit(directory, "a.xml")
                    tree = ET.parse(lane)
                    suite = tree.getroot().find("testsuite")
                    producer = suite.find("testcase")
                    producer.attrib.update(status="run", result="completed")
                    if carrier == "attribute":
                        producer.set("ambient_receipt", receipt_line())
                    elif carrier == "property":
                        ET.SubElement(producer.find("properties"), "property",
                                      name="ambient_receipt", value=receipt_line())
                    else:
                        ET.SubElement(producer, "system-out").text = receipt_line()
                    if outcome in ("disabled", "unknown"):
                        producer.set("status", outcome)
                    elif outcome:
                        ET.SubElement(producer, outcome)
                    ET.SubElement(suite, "testcase", name="Unrelated", status="disabled")
                    tree.write(lane)
                    code, out, err = run_main([f"avx2={lane}"])
                    self.assertEqual(code, 0 if outcome is None else 2, out + err)

    def test_unattributed_receipt_still_uses_report_execution_status(self):
        for carrier in ("property", "output"):
            for outcome in (None, "failure", "error", "skipped", "disabled", "unknown"):
                with self.subTest(carrier=carrier, outcome=outcome), tempfile.TemporaryDirectory() as directory:
                    # An identical good attributed receipt must not hide a suite-level copy.
                    lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
                    tree = ET.parse(lane)
                    suite = tree.getroot().find("testsuite")
                    if carrier == "property":
                        properties = ET.SubElement(suite, "properties")
                        ET.SubElement(properties, "property", name="ambient_receipt", value=receipt_line())
                    else:
                        ET.SubElement(suite, "system-out").text = receipt_line()
                    other = ET.SubElement(suite, "testcase", name="Unrelated", status="run", result="completed")
                    if outcome in ("disabled", "unknown"):
                        other.set("status", outcome)
                    elif outcome:
                        ET.SubElement(other, outcome)
                    tree.write(lane)
                    code, out, err = run_main([f"avx2={lane}"])
                    self.assertEqual(code, 0 if outcome is None else 2, out + err)

    def test_unevaluated_receipts_and_probes_are_rejected(self):
        metadata = (("status", "disabled"), ("status", "notrun"),
                    ("status", "skipped"), ("status", "unknown"), ("status", ""),
                    ("result", "suppressed"), ("result", "skipped"),
                    ("result", "unknown"), ("result", ""))
        for field, value in metadata:
            for source in ("attribute", "property", "output", "suite", "probe", "duplicate"):
                with self.subTest(field=field, value=value, source=source), tempfile.TemporaryDirectory() as directory:
                    line = receipt_line()
                    lane = write_junit(directory, "a.xml", attribute_value=line)
                    tree = ET.parse(lane)
                    suite = tree.getroot().find("testsuite")
                    case = suite.find("testcase")
                    if source == "property":
                        case.attrib.pop("ambient_receipt")
                        ET.SubElement(case.find("properties"), "property", name="ambient_receipt", value=line)
                    elif source in ("output", "suite"):
                        case.attrib.pop("ambient_receipt")
                        ET.SubElement(case if source == "output" else suite, "system-out").text = line
                    elif source == "duplicate":
                        # A good duplicate must never hide an unevaluated producer.
                        case = ET.SubElement(suite, "testcase", name="Other", ambient_receipt=line)
                    if source != "probe":
                        case.set(field, value)
                    tree.write(lane)
                    if source == "probe":
                        add_probe(lane)
                        tree = ET.parse(lane)
                        tree.getroot().find("testsuite")[-1].set(field, value)
                        tree.write(lane)
                    code, out, err = run_main([f"asan={lane}"] + (["--require-probe", "asan"] if source == "probe" else []))
                    self.assertEqual(code, 2, out)
                    self.assertIn("unevaluated", err)

    def test_explicit_success_execution_metadata_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
            add_probe(lane)
            tree = ET.parse(lane)
            for case in tree.getroot().iter("testcase"):
                case.attrib.update(status="run", result="completed")
            tree.write(lane)
            code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
            self.assertEqual(code, 0, err)

    def test_suite_only_receipts_require_successful_suite_metadata(self):
        metadata = (None, ("status", "run"), ("result", "completed"), ("tests", "1"),
                    ("failures", "0"), ("errors", "0"), ("skipped", "0"), ("disabled", "0"),
                    ("status", "disabled"), ("status", "notrun"), ("status", "skipped"),
                    ("status", "unknown"), ("status", ""), ("result", "suppressed"),
                    ("result", "skipped"), ("result", "unknown"), ("result", ""),
                    ("tests", "0"), ("tests", "-1"), ("tests", "unknown"), ("tests", ""))
        metadata += tuple((key, value) for key in ("failures", "errors", "skipped", "disabled")
                          for value in ("1", "unknown", ""))
        for carrier in ("output", "property"):
            for wrapper in (False, True):
                for attributes in metadata:
                    with self.subTest(carrier=carrier, wrapper=wrapper, metadata=attributes), \
                            tempfile.TemporaryDirectory() as directory:
                        suite = ET.Element("testsuite")
                        root = ET.Element("testsuites") if wrapper else suite
                        if wrapper:
                            root.append(suite)
                        if attributes:
                            root.set(*attributes)
                        if carrier == "output":
                            ET.SubElement(suite, "system-out").text = receipt_line()
                        else:
                            properties = ET.SubElement(suite, "properties")
                            ET.SubElement(properties, "property", name="ambient_receipt", value=receipt_line())
                        path = Path(directory) / "receipt.xml"
                        ET.ElementTree(root).write(path, encoding="utf-8")
                        code, out, err = run_main([f"avx2={path}"])
                        successful = attributes in metadata[:8]
                        self.assertEqual(code, 0 if successful else 2, out + err)
                        if not successful:
                            self.assertEqual(out, "")
                            self.assertIn("unevaluated", err)
        for body, message in (("", "cannot read"), ("<testsuite>", "cannot read"),
                              ("<testsuite/>", "no receipt"), ("<testsuites/>", "no receipt")):
            with self.subTest(body=body), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "receipt.xml"
                path.write_text(body, encoding="utf-8")
                code, out, err = run_main([f"avx2={path}"])
                self.assertEqual(code, 2, out + err)
                self.assertIn(message, err)

    def test_suite_outcomes_and_nested_suites_cannot_qualify_unattributed_receipts(self):
        for carrier in ("output", "property"):
            for outcome in ("failure", "error", "skipped"):
                for nested in (False, True):
                    with self.subTest(carrier=carrier, outcome=outcome, nested=nested), \
                            tempfile.TemporaryDirectory() as directory:
                        root = ET.Element("testsuites")
                        suite = ET.SubElement(root, "testsuite", tests="1", failures="0")
                        if carrier == "output":
                            ET.SubElement(suite, "system-out").text = receipt_line()
                        else:
                            properties = ET.SubElement(suite, "properties")
                            ET.SubElement(properties, "property", name="ambient_receipt", value=receipt_line())
                        if nested:
                            suite = ET.SubElement(suite, "testsuite")
                        ET.SubElement(suite, outcome)
                        path = Path(directory) / "receipt.xml"
                        ET.ElementTree(root).write(path, encoding="utf-8")
                        code, out, err = run_main([f"avx2={path}"])
                        self.assertEqual(code, 2, out + err)
                        self.assertIn("unevaluated", err)

    def test_agree_when_lane_simd_levels_match(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(
                directory,
                "a.xml",
                ["[  PASSED  ] 1 test.", receipt_line(cpu_simd="512")],
            )
            lane_b = write_junit(
                directory,
                "b.xml",
                [receipt_line(cpu_simd="1", active_simd="1")],
            )
            code, out, err = run_main([f"avx2={lane_a}", f"scalar={lane_b}"])
        self.assertEqual(code, 0, err)
        document = json.loads(out)
        self.assertEqual(document["schema"], "luminumbra.ambient_receipt_comparison.v1")
        self.assertEqual(document["verdict"], "agree")
        self.assertEqual(document["mismatches"], [])
        self.assertEqual(document["labels"]["avx2"]["cpu_simd"], "512")
        self.assertEqual(document["labels"]["avx2"]["active_simd"], "256")
        self.assertEqual(document["labels"]["scalar"]["cpu_simd"], "1")
        self.assertEqual(document["labels"]["scalar"]["active_simd"], "1")

    def test_mislabeled_active_simd_is_rejected(self):
        for label, active, expected in (("avx2", "4", "256"), ("scalar", "256", "1")):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                lane = write_junit(directory, "a.xml", [receipt_line(active_simd=active)])
                code, out, err = run_main([f"{label}={lane}"])
            self.assertEqual(code, 2)
            self.assertEqual(out, "")
            self.assertIn(f"{label}: active_simd={active}", err)
            self.assertIn(f"expected SIMD level {expected}", err)

    def test_cpu_must_support_the_lane_simd_level(self):
        for cpu in ("4", "2", "0", "3", "65536"):
            with self.subTest(cpu=cpu), tempfile.TemporaryDirectory() as directory:
                lane = write_junit(directory, "a.xml", [receipt_line(cpu_simd=cpu)])
                code, out, err = run_main([f"avx2={lane}"])
            self.assertEqual(code, 2)
            self.assertEqual(out, "")
            self.assertIn(f"cpu_simd={cpu} does not support active_simd=256", err)

    def test_missing_simd_fields_are_rejected(self):
        for key in ("cpu_simd", "active_simd"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                line = receipt_line().replace(f" {key}=256", "")
                lane = write_junit(directory, "a.xml", [line])
                code, out, err = run_main([f"avx2={lane}"])
            self.assertEqual(code, 2)
            self.assertEqual(out, "")
            self.assertIn(f"missing fields {key}", err)

    def test_non_integer_simd_fields_are_rejected(self):
        for key in ("cpu_simd", "active_simd"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                lane = write_junit(directory, "a.xml", [receipt_line(**{key: "avx2"})])
                code, out, err = run_main([f"avx2={lane}"])
            self.assertEqual(code, 2)
            self.assertEqual(out, "")
            self.assertIn(f"field '{key}' must be a base-10 integer", err)

    def test_active_simd_cannot_exceed_dispatch_ceiling(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line(ceiling="4")])
            code, out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("active_simd=256 exceeds dispatch ceiling=4", err)

    def test_unknown_lane_label_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line()])
            code, out, err = run_main([f"unknown={lane}"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("unknown lane label", err)

    def test_existing_ci_lanes_require_avx2(self):
        for label in ("linux-gcc", "windows-msvc", "windows-ucrt64", "asan"):
            for active in ("256", "4"):
                with self.subTest(label=label, active=active), tempfile.TemporaryDirectory() as directory:
                    lane = write_junit(directory, "a.xml", [receipt_line(active_simd=active)])
                    code, out, err = run_main([f"{label}={lane}"])
                if active == "256":
                    self.assertEqual(code, 0, err)
                else:
                    self.assertEqual(code, 2)
                    self.assertEqual(out, "")
                    self.assertIn("expected SIMD level 256", err)

    def test_required_probe_accepts_ctest_and_gtest_evidence(self):
        for carrier in ("output", "attribute", "property"):
            with self.subTest(carrier=carrier), tempfile.TemporaryDirectory() as directory:
                if carrier == "output":
                    lane = write_junit(directory, "a.xml", [receipt_line()])
                else:
                    lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
                add_probe(lane, carrier=carrier)
                if carrier == "attribute":
                    tree = ET.parse(lane)
                    case = tree.getroot().find("testsuite").findall("testcase")[-1]
                    case.set("name", "UnpaddedTwoElementInputFaultsAtActiveIsa")
                    case.set("classname", "AmbientInputBounds")
                    tree.write(lane)
                code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
            self.assertEqual(code, 0, err)
            self.assertEqual(json.loads(out)["verdict"], "agree")

    def test_missing_required_probe_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line()])
            code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("requires exactly one", err)

    def test_required_probe_must_have_ran_status(self):
        for status in (
            "not_applicable",
            "not_applicable:guard-page probe is Linux only",
            "not_applicable:active SIMD level has no wide tail load",
            "ran_without_guard_page",
            "",
        ):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
                add_probe(lane, status=status, carrier="attribute")
                code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
            self.assertEqual(code, 2)
            self.assertEqual(out, "")
            self.assertIn("requires probe_status=ran", err)

    def test_non_applicable_marker_passes_only_without_required_probe(self):
        for label, active, reason in (
            ("windows-msvc", "256", "guard-page probe is Linux only"),
            ("windows-ucrt64", "256", "guard-page probe is Linux only"),
            ("scalar", "1", "active SIMD level has no wide tail load"),
            ("asan", "256", "guard-page probe is Linux only"),
        ):
            with self.subTest(label=label), tempfile.TemporaryDirectory() as directory:
                lane = write_junit(directory, "a.xml", [receipt_line(active_simd=active)])
                add_probe(lane, status=f"not_applicable:{reason}", carrier="attribute")
                tree = ET.parse(lane)
                case = tree.getroot().find("testsuite").findall("testcase")[-1]
                del case.attrib["noise_simd_level"]
                ET.SubElement(case, "system-out").text = (
                    f"AMBIENT_PROBE probe_status=not_applicable:{reason}"
                )
                tree.write(lane, encoding="utf-8")
                code, out, err = run_main([f"{label}={lane}"])
                self.assertEqual(code, 0, err)
                self.assertEqual(json.loads(out)["verdict"], "agree")
                code, out, err = run_main([f"{label}={lane}", "--require-probe", label])
                self.assertEqual(code, 2)
                self.assertEqual(out, "")
                self.assertIn("malformed unpadded guard-page probe evidence", err)

    def test_failed_or_skipped_required_probe_is_rejected(self):
        for outcome in ("failure", "error", "skipped"):
            with self.subTest(outcome=outcome), tempfile.TemporaryDirectory() as directory:
                lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
                add_probe(lane, outcome=outcome)
                code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
            self.assertEqual(code, 2)
            self.assertEqual(out, "")
            self.assertIn("unpadded guard-page probe failed, errored or was skipped", err)

    def test_required_probe_simd_must_match_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
            add_probe(lane, level="4")
            code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("probe SIMD level differs from receipt", err)

    def test_padded_probe_cannot_qualify_the_unpadded_probe(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
            add_probe(lane)
            tree = ET.parse(lane)
            case = tree.getroot().find("testsuite").findall("testcase")[-1]
            case.set("name", "AmbientInputBounds.SixteenElementPaddedInputDoesNotFault")
            tree.write(lane)
            code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("requires exactly one", err)

    def test_require_probe_label_must_be_present(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line()])
            code, out, err = run_main([f"avx2={lane}", "--require-probe", "asan"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("--require-probe names a lane that is not present", err)

    def test_disagree_reports_the_mismatching_key(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line(wind_evolved="0badc0de")])
            code, out, err = run_main([f"avx2={lane_a}", f"linux-gcc={lane_b}"])
        self.assertEqual(code, 1, err)
        document = json.loads(out)
        self.assertEqual(document["verdict"], "disagree")
        self.assertEqual(len(document["mismatches"]), 1)
        mismatch = document["mismatches"][0]
        self.assertEqual(mismatch["key"], "wind_evolved")
        self.assertEqual(mismatch["values"], {"avx2": "bb22", "linux-gcc": "0badc0de"})

    def test_missing_receipt_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", ["no receipt printed here"])
            code, out, err = run_main([f"avx2={lane_a}", f"linux-gcc={lane_b}"])
        self.assertEqual(code, 2)
        self.assertIn("no receipt", err)
        self.assertEqual(out, "")

    def test_two_different_receipts_in_one_file_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(
                directory,
                "a.xml",
                [receipt_line(), receipt_line(wind_ctor="0000")],
            )
            code, out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertIn("multiple receipts", err)

    def test_duplicate_labels_are_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, _out, err = run_main([f"same={lane_a}", f"same={lane_b}"])
        self.assertEqual(code, 2)
        self.assertIn("duplicate label", err)

    def test_expect_labels_must_match_exactly(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, _out, err = run_main(
                [f"avx2={lane_a}", f"linux-gcc={lane_b}", "--expect-labels", "avx2,linux-gcc,asan"]
            )
        self.assertEqual(code, 2)
        self.assertIn("do not match", err)

    def test_expect_labels_accepts_exact_set(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, out, err = run_main(
                [f"linux-gcc={lane_b}", f"avx2={lane_a}", "--expect-labels", "avx2,linux-gcc"]
            )
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["verdict"], "agree")

    def test_receipt_found_only_in_property_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [], [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, out, err = run_main([f"avx2={lane_a}", f"linux-gcc={lane_b}"])
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["verdict"], "agree")

    def test_identical_line_in_output_and_property_is_one_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line()], [receipt_line()])
            code, out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["labels"]["avx2"]["wind_ctor"], "aa11")

    def test_non_version_one_receipt_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(
                directory,
                "a.xml",
                [receipt_line().replace("version=1", "version=2")],
            )
            code, _out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertIn("version", err)

    def test_empty_hash_value_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line(wind_ctor="")])
            code, out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("wind_ctor", err)

    def test_non_hex_hash_value_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line(wind_evolved="xyz!")])
            code, _out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertIn("wind_evolved", err)

    def test_non_integer_seed_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line(seed="4x")])
            code, _out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertIn("seed", err)

    def test_receipt_from_failed_testcase_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(
                directory,
                "a.xml",
                [],
                [receipt_line()],
                attribute_value=receipt_line(),
                failed=True,
            )
            code, out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("failed, errored or skipped", err)

    def test_unevaluated_status_rejects_attributed_and_suite_level_receipts(self):
        for status in (None, "run", "disabled", "notrun", "", "unknown"):
            for attributed in (False, True):
                with self.subTest(status=status, attributed=attributed), tempfile.TemporaryDirectory() as directory:
                    lane = write_junit(directory, "a.xml", [] if attributed else [receipt_line()],
                                       attribute_value=receipt_line() if attributed else None)
                    tree = ET.parse(lane)
                    if status is not None:
                        next(tree.getroot().iter("testcase")).set("status", status)
                    tree.write(lane)
                    code, out, err = run_main([f"asan={lane}"])
                    if status in (None, "run"):
                        self.assertEqual(code, 0, err)
                    else:
                        self.assertEqual(code, 2, err)
                        self.assertEqual(out, "")
                        self.assertIn("unevaluated", err)

    def test_unevaluated_status_rejects_required_probe(self):
        for status in (None, "run", "disabled", "notrun", "", "unknown"):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as directory:
                lane = write_junit(directory, "a.xml", attribute_value=receipt_line())
                add_probe(lane)
                tree = ET.parse(lane)
                if status is not None:
                    tree.getroot().find("testsuite").findall("testcase")[-1].set("status", status)
                tree.write(lane)
                code, out, err = run_main([f"asan={lane}", "--require-probe", "asan"])
                if status in (None, "run"):
                    self.assertEqual(code, 0, err)
                else:
                    self.assertEqual(code, 2, err)
                    self.assertEqual(out, "")
                    self.assertIn("unevaluated", err)

    def test_suite_level_receipt_with_failed_testcase_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line()], failed=True)
            code, out, err = run_main([f"avx2={lane}"])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("failed, errored or skipped", err)

    def test_attribute_form_receipt_is_accepted_and_compared(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", attribute_value=receipt_line())
            lane_b = write_junit(
                directory,
                "b.xml",
                attribute_value=receipt_line(wind_evolved="0badc0de"),
            )
            code, out, err = run_main([f"avx2={lane_a}", f"linux-gcc={lane_b}"])
        self.assertEqual(code, 1, err)
        document = json.loads(out)
        self.assertEqual(document["labels"]["avx2"]["wind_evolved"], "bb22")
        self.assertEqual(len(document["mismatches"]), 1)
        self.assertEqual(document["mismatches"][0]["key"], "wind_evolved")

    def test_unreadable_file_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = str(Path(directory) / "absent.xml")
            code, _out, err = run_main([f"avx2={missing}"])
        self.assertEqual(code, 2)
        self.assertIn("cannot read", err)


if __name__ == "__main__":
    unittest.main()
