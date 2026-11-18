"""Exercise the Windows runtime gate using inert files in an owned fixture."""

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class RuntimeStaging(unittest.TestCase):
    cmake = shutil.which("cmake")
    stage_script = Path(__file__).resolve().parents[2] / "cmake" / "StageWindowsAsanRuntime.cmake"
    ctest = None

    def test_staging_and_preflight(self):
        if not self.cmake:
            self.skipTest("Use CTest or supply the CMake executable to exercise runtime staging.")
        ctest = self.ctest or Path(self.cmake).with_name("ctest" + Path(self.cmake).suffix)
        work = Path(tempfile.mkdtemp(prefix="flitzfs-asan-check-"))
        self.addCleanup(work.rmdir)
        output = work / "test outputs"
        output.mkdir()
        self.addCleanup(output.rmdir)
        source = work / "clang_rt.asan_dynamic-x86_64.dll"
        staged = output / source.name
        manifest = work / "outputs.txt"
        for path in (source, staged, manifest):
            self.addCleanup(path.unlink, missing_ok=True)

        runtime_bytes = b"known runtime fixture"
        with manifest.open("x", encoding="utf-8") as file:
            file.write(output.as_posix() + "\n")
        command = [
            self.cmake,
            f"-Druntime={source.as_posix()}",
            f"-Dmanifest={manifest.as_posix()}",
            f"-Dexpected_sha256={hashlib.sha256(runtime_bytes).hexdigest()}",
        ]

        def run_stage(*, preflight=False):
            return subprocess.run(
                [*command, f"-Dpreflight={int(preflight)}", "-P", self.stage_script],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                encoding="utf-8", errors="replace", timeout=5, check=False,
            )

        def require_failure(result, message):
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn(message, result.stdout)

        require_failure(run_stage(preflight=True), "runtime is missing")
        self.assertFalse(staged.exists())
        with source.open("xb") as file:
            file.write(runtime_bytes)

        result = run_stage(preflight=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(staged.read_bytes(), runtime_bytes)

        staged.unlink()
        different_runtime = b"runtime from a different toolset"
        with staged.open("xb") as file:
            file.write(different_runtime)
        require_failure(run_stage(preflight=True), "runtime differs from the configured toolset")
        self.assertEqual(staged.read_bytes(), different_runtime)

        result = run_stage()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(staged.read_bytes(), runtime_bytes)
        source.unlink()
        result = run_stage(preflight=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        require_failure(run_stage(), "runtime is missing")

        with source.open("xb") as file:
            file.write(b"changed compiler runtime")
        require_failure(run_stage(), "runtime differs from the configured toolset")
        self.assertEqual(staged.read_bytes(), runtime_bytes)
        staged.unlink()
        require_failure(run_stage(preflight=True), "runtime differs from the configured toolset")
        self.assertFalse(staged.exists())

        # Exercise automatic fixture selection and dependency refusal through
        # CTest. The dependent command leaves evidence if CTest launches it.
        tests = work / "CTestTestfile.cmake"
        launched = work / "test-launched"
        self.addCleanup(tests.unlink, missing_ok=True)
        self.addCleanup(launched.unlink, missing_ok=True)

        def cmake_arguments(args):
            quoted = []
            for arg in map(str, args):
                delimiter = "="
                while f"]{delimiter}]" in arg:
                    delimiter += "="
                quoted.append(f"[{delimiter}[{arg}]{delimiter}]")
            return " ".join(quoted)

        setup = cmake_arguments([*command, "-Dpreflight=ON", "-P", self.stage_script])
        dependent = cmake_arguments([self.cmake, "-E", "touch", launched.as_posix()])
        with tests.open("x", encoding="utf-8") as file:
            file.write(f"add_test(asan_setup {setup})\n"
                       "set_tests_properties(asan_setup PROPERTIES FIXTURES_SETUP asan_runtime)\n"
                       f"add_test(asan_dependent {dependent})\n"
                       "set_tests_properties(asan_dependent PROPERTIES FIXTURES_REQUIRED asan_runtime)\n")

        def run_filtered():
            return subprocess.run(
                [ctest, "--test-dir", str(work), "--interactive-debug-mode", "0",
                 "--output-on-failure", "--no-tests=error", "--timeout", "5",
                 "--parallel", "1", "-R", "^asan_dependent$"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                encoding="utf-8", errors="replace", timeout=8, check=False,
            )

        def remove_ctest_logs():
            logs = work / "Testing" / "Temporary"
            for directory in (logs / "LastTestRun", logs):
                if directory.is_dir():
                    for log in directory.iterdir():
                        log.unlink()
                    directory.rmdir()
            if logs.parent.is_dir():
                logs.parent.rmdir()

        self.addCleanup(remove_ctest_logs)
        source.unlink()
        require_failure(run_filtered(), "runtime is missing")
        self.assertFalse(launched.exists())
        self.assertFalse(staged.exists())
        with source.open("xb") as file:
            file.write(runtime_bytes)
        with staged.open("xb") as file:
            file.write(different_runtime)
        require_failure(run_filtered(), "runtime differs from the configured toolset")
        self.assertFalse(launched.exists())
        self.assertEqual(staged.read_bytes(), different_runtime)
        staged.unlink()
        result = run_filtered()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(launched.is_file())
        self.assertEqual(staged.read_bytes(), runtime_bytes)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        RuntimeStaging.cmake, RuntimeStaging.stage_script, *ctest = sys.argv[1:]
        if ctest:
            RuntimeStaging.ctest, = ctest
    unittest.main(argv=sys.argv[:1])
