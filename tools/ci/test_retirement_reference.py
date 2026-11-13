"""CPU-only source regression; no subprocess, mount, dispatcher or GPU execution."""
import json
from pathlib import Path
import unittest
from retirement_reference import ANCHOR, CPP_MAIN, PINS, blob, once, transform

SOURCE = None  # The bounded in-memory test harness may supply the same exact blob.

class ReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        data = SOURCE
        if data is None:
            path = Path(__file__).resolve().parents[2] / ".ci-retirement/tests/winfsp_write_probe.cpp"
            if path.stat().st_size > 128 * 1024:
                raise ValueError("reference fixture cap")
            data = path.read_bytes()
        if blob(data) != PINS["tests/winfsp_write_probe.cpp"]:
            raise ValueError("test fixture is not the canonical historical source")
        cls.original = data.decode()

    def test_exact_old_body_is_retained_except_closed_transformations(self):
        original = self.original
        for variant in ("held", "closed"):
            result = transform(original, variant)
            historical = original[original.index("void restart_source(bool killed)"):
                                  original.index("void append_continuation(")]
            before, after = historical.split(ANCHOR)
            self.assertIn(before, result)
            self.assertIn(after, result)
            self.assertEqual(result.count("int wmain("), 1)
            self.assertIn("int historical_unused_entrypoint(", result)
        self.assertEqual(blob(original.encode()), PINS["tests/winfsp_write_probe.cpp"])

    def test_closed_changes_real_handle_before_phase_marker_and_wait(self):
        result = transform(self.original, "closed")
        close = result.index("reference checked client handle close")
        marker = result.index("const auto text=reference_phase_text(GetCurrentProcessId())")
        wait = result.index("wait_for_supervised_termination(session);", marker)
        self.assertLess(close, marker)
        self.assertLess(marker, wait)
        self.assertIn("std::exchange(file.value,INVALID_HANDLE_VALUE)", result)
        self.assertIn("reference client contexts did not retire", result)
        self.assertIn(" contexts=0\\n", result)

    def test_held_never_inserts_close(self):
        result = transform(self.original, "held")
        self.assertNotIn("reference checked client handle close", result)
        self.assertIn(" contexts=1\\n", result)

    def test_missing_and_duplicate_source_anchors_reject(self):
        for needle in (ANCHOR, '#include "winfsp_session.hpp"', "int wmain(int argc,wchar_t** argv)"):
            with self.subTest(needle=needle):
                with self.assertRaises(ValueError):
                    transform(self.original.replace(needle, ""), "held")
                with self.assertRaises(ValueError):
                    transform(self.original + needle, "closed")

    def test_no_open_variant_or_guessed_replacement(self):
        for variant in ("", "HELD", "cpu", "external", "../held"):
            with self.assertRaises(ValueError):
                transform(self.original, variant)
        self.assertEqual(once("abc", "b", "x"), "axc")
        for text in ("ac", "abbc"):
            with self.assertRaises(ValueError):
                once(text, "b", "x")

    def test_fixed_one_phase_contract(self):
        self.assertIn("budget.timeout_ms=15000", CPP_MAIN)
        self.assertEqual(CPP_MAIN.count("run_bounded_process("), 1)
        self.assertIn("result.exit_confirmed", CPP_MAIN)
        self.assertIn("lease->wait_for_exit(0)", CPP_MAIN)
        self.assertNotIn("run_extended(", CPP_MAIN)
        self.assertNotIn("reference_original_wmain(", CPP_MAIN)
        self.assertNotIn("historical_unused_entrypoint(", CPP_MAIN)

    def test_unknown_retirement_precedes_all_backing_access(self):
        refusal = CPP_MAIN.index('"reference retirement unqualified"')
        self.assertLess(refusal, CPP_MAIN.index("emit_extended("))
        self.assertLess(refusal, CPP_MAIN.index("Fixture fixture("))
        self.assertLess(refusal, CPP_MAIN.index("open_file("))
        self.assertNotIn("request_stop(", CPP_MAIN)

    def test_reference_ack_precedes_variant_marker(self):
        for variant in ("held", "closed"):
            result = transform(self.original, variant)
            marker = result.index("const auto text=reference_phase_text(GetCurrentProcessId())")
            ack = result.rindex('extended_report(fixture,profile,"ack"', 0, marker)
            self.assertLess(ack, marker)
        self.assertIn("view.read(\"sample.bin\",0,10000)==expected_data()", CPP_MAIN)
        self.assertIn("bytes=7001", CPP_MAIN)

if __name__ == "__main__":
    unittest.main()
