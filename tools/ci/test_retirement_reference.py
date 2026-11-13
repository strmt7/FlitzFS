"""CPU-only source regression; no subprocess, mount, dispatcher or GPU execution."""
import json
import os
from pathlib import Path
import unittest
from retirement_reference import ANCHOR, CPP_MAIN, PINS, blob, once, transform, reconcile, strict_json, same_git_root

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

    def test_git_root_normalization_uses_native_path_semantics(self):
        root = Path(__file__).resolve().parents[2]
        self.assertTrue(same_git_root(str(root), root))
        self.assertTrue(same_git_root(root.as_posix(), root))
        self.assertTrue(same_git_root(str(root / "."), root))
        self.assertFalse(same_git_root(str(root.parent), root))
        self.assertFalse(same_git_root(".", root))
        self.assertFalse(same_git_root("", root))
        self.assertFalse(same_git_root(str(root / "does-not-exist-reference-root"), root))
        self.assertFalse(same_git_root(None, root))
        if os.name == "nt":
            # The old string comparator fails for the Git spelling we observed
            # on both failed Windows runners; the corrected path comparator passes.
            spelling = root.as_posix()
            self.assertNotEqual(spelling.casefold(), str(root).casefold())
            self.assertTrue(same_git_root(spelling, root))
            self.assertTrue(same_git_root(spelling, root))

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


class ReconciliationTests(unittest.TestCase):
    def fixture(self, variant="held"):
        commit = "a" * 40
        phase = f"retirement_phase variant={variant} source_process=173 contexts={1 if variant == 'held' else 0}\n"
        ack = {"schema": 2, "profile": "crash-restart", "phase": "ack", "commit": commit,
               "source_process": 173, "observer_process": 173, "user_bytes": 7001, "inode": 2,
               "file_count": 1, "image_cap": 4194304, "contexts": 1,
               "explicit_flush_observed": True, "cpu_verified": True, "dispatcher_stopped": False,
               "gpu": False, "namespace_sequence": 7, "generation": 14, "image_bytes": 1572864,
               "creation_time": 134388713284516351, "last_access_time": 134388713284516351,
               "last_write_time": 134388713284516351, "change_time": 134388713284516351,
               "attributes": 32, "reservation": 8192, "data_digest": "1" * 64,
               "security_digest": "2" * 64, "keep_digest": "0" * 64}
        return commit, phase, ack

    def output(self, phase, acknowledgement, variant="held"):
        return (f"retirement_result variant={variant} source_process=173 exit=1460 timed_out=1 "
                "cancelled=0 exit_confirmed=1 peak_commit_bytes=4734976 controller_peak_commit_bytes=6000000\n"
                + phase + "Windows writable extended evidence: " + acknowledgement + "\n"
                + f"retirement_cpu_verified variant={variant} source_process=173 bytes=7001\n")

    def admit(self, variant="held", patch=None):
        commit, phase, ack = self.fixture(variant)
        if patch:
            patch(ack)
        raw = json.dumps(ack, separators=(",", ":"))
        return reconcile(self.output(phase, raw, variant), phase, raw, commit, variant)

    def test_both_context_variants_and_crlf_preserve_actual_observations(self):
        for variant, contexts in (("held", 1), ("closed", 0)):
            proof = self.admit(variant)
            self.assertEqual(proof["phase_contexts"], contexts)
            self.assertTrue(proof["retirement_confirmed"])
            self.assertEqual(proof["historical_cause"], "unproven")
            commit, phase, ack = self.fixture(variant)
            raw = json.dumps(ack, separators=(",", ":"))
            self.assertEqual(reconcile(self.output(phase, raw, variant).replace("\n", "\r\n"),
                                       phase, raw, commit, variant), proof)

    def test_every_missing_acknowledgement_field_rejects(self):
        for key in self.fixture()[2]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.admit(patch=lambda ack: ack.pop(key))

    def test_mixed_actor_commit_phase_and_invented_gpu_or_flush_reject(self):
        changes = {"source_process": 174, "observer_process": 174, "commit": "b" * 40,
                   "phase": "verify", "contexts": 0, "gpu": True, "cpu_verified": False,
                   "explicit_flush_observed": False, "dispatcher_stopped": True, "inode": 3,
                   "user_bytes": 7000, "creation_time": 0, "keep_digest": "1" * 64}
        for key, value in changes.items():
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.admit(patch=lambda ack: ack.update({key: value}))

    def test_unknown_exit_cannot_be_overruled_by_cpu_claim(self):
        commit, phase, ack = self.fixture()
        raw = json.dumps(ack, separators=(",", ":"))
        output = self.output(phase, raw)
        for old, new in (("exit_confirmed=1", "exit_confirmed=0"), ("timed_out=1", "timed_out=0"),
                         ("cancelled=0", "cancelled=1"), ("exit=1460", "exit=0"),
                         ("peak_commit_bytes=4734976", "peak_commit_bytes=0"),
                         ("controller_peak_commit_bytes=6000000", "controller_peak_commit_bytes=268435457")):
            with self.subTest(old=old), self.assertRaises(ValueError):
                reconcile(output.replace(old, new), phase, raw, commit, "held")

    def test_raw_receipt_swaps_duplicates_partial_and_extra_records_reject(self):
        commit, phase, ack = self.fixture()
        raw = json.dumps(ack, separators=(",", ":"))
        output = self.output(phase, raw)
        for text, phase_input, ack_input in ((output, phase.replace("173", "174"), raw),
                                             (output, phase, raw + " "),
                                             (output + output, phase, raw),
                                             ("\n".join(output.splitlines()[:-1]), phase, raw)):
            with self.assertRaises(ValueError):
                reconcile(text, phase_input, ack_input, commit, "held")
        duplicate = raw[:-1] + ',"source_process":173}'
        with self.assertRaises(ValueError):
            reconcile(self.output(phase, duplicate), phase, duplicate, commit, "held")
        for text in ('{"x":NaN}', '{"x":1,"x":2}', '[]'):
            with self.assertRaises(ValueError):
                strict_json(text)

    def test_filetime_precision_remains_exact_above_javascript_integer_range(self):
        first = self.admit()
        second = self.admit(patch=lambda ack: ack.update({"creation_time": 134388713284516352}))
        self.assertNotEqual(first["acknowledgement_sha256"], second["acknowledgement_sha256"])
        raw = '{"time":134388713284516351}'
        self.assertEqual(strict_json(raw)["time"], 134388713284516351)

    def test_closed_schema_integer_types_and_capacities_reject(self):
        for key, value in (("unexpected", 1), ("generation", True), ("contexts", True),
                           ("image_bytes", 4194305), ("reservation", 7000),
                           ("creation_time", -1), ("last_write_time", 1.5),
                           ("change_time", 18446744073709551616), ("data_digest", "X" * 64)):
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.admit(patch=lambda ack: ack.update({key: value}))

if __name__ == "__main__":
    unittest.main()
