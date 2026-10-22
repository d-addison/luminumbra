"""Bounded case clocks for directly executed unittest contract suites.

START/STOP delimit unittest callbacks, including per-case setup and cleanup;
STOP does not imply success. Imports and discovery precede SUITE_START. Class
fixtures run outside case intervals, so gaps cannot identify a fixture phase.
No subtest parameters, exception messages, paths or test descriptions are logged.
"""

import os
import time
import unittest


class CaseDiagnosticResult(unittest.TextTestResult):
    def __init__(self, stream, descriptions, verbosity, **kwargs):
        super().__init__(stream, descriptions, verbosity, **kwargs)
        # Retain the runner channel before a case can redirect sys.stderr.
        self._diagnostic_stream = stream
        self._diagnostic_pid = os.getpid()
        self._diagnostic_case = "-"

    @staticmethod
    def _case_name(test):
        # Bound both input slices and emitted identity to 193 ASCII characters.
        # Use declared names, never id()/str(), which can include subtest data.
        parts = (type(test).__name__[:96], test._testMethodName[:96])
        return ".".join("".join(c if c.isascii() and (c.isalnum() or c == "_")
                               else "_" for c in part) for part in parts)

    def _record(self, event, identity):
        record = (f"\n[contract-case] {event} pid={self._diagnostic_pid} "
                  f"case={identity} monotonic_ns={time.monotonic_ns()} "
                  f"process_cpu_ns={time.process_time_ns()}\n")
        if self._diagnostic_stream.write(record) != len(record):
            raise OSError("incomplete contract diagnostic write")
        self._diagnostic_stream.flush()

    def startTestRun(self):
        super().startTestRun()
        self._record("SUITE_START", "imports_and_discovery_precede;class_fixtures_outside_cases")

    def stopTestRun(self):
        super().stopTestRun()
        self._record("SUITE_STOP", "runner_stop_is_not_completion_or_success")

    def startTest(self, test):
        super().startTest(test)
        self._diagnostic_case = self._case_name(test)
        self._record("START", self._diagnostic_case)

    def stopTest(self, test):
        super().stopTest(test)
        self._record("STOP", self._diagnostic_case)


class CaseDiagnosticRunner(unittest.TextTestRunner):
    resultclass = CaseDiagnosticResult

    def _makeResult(self):
        result = super()._makeResult()
        # An incompatible custom result must fail visibly before running tests.
        if not isinstance(result, CaseDiagnosticResult):
            raise TypeError("contract diagnostics require CaseDiagnosticResult")
        return result
