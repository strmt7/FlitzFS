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

    def test_staging_and_preflight(self):
        if not self.cmake:
            self.skipTest("Use CTest or supply the CMake executable to exercise runtime staging.")
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
        with source.open("xb") as file:
            file.write(runtime_bytes)
        with manifest.open("x", encoding="utf-8") as file:
            file.write(output.as_posix() + "\n")
        command = [
            self.cmake,
            f"-Druntime={source.as_posix()}",
            f"-Dmanifest={manifest.as_posix()}",
            f"-Dexpected_sha256={hashlib.sha256(runtime_bytes).hexdigest()}",
        ]

        def run_stage(*, verify_only=False):
            return subprocess.run(
                [*command, f"-Dverify_only={int(verify_only)}", "-P", self.stage_script],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                encoding="utf-8", errors="replace", timeout=5, check=False,
            )

        def require_failure(result, message):
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn(message, result.stdout)

        require_failure(run_stage(verify_only=True), "runtime is missing")
        with staged.open("xb") as file:
            file.write(b"runtime from a different toolset")
        require_failure(run_stage(verify_only=True), "runtime differs from the configured toolset")

        result = run_stage()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(staged.read_bytes(), runtime_bytes)
        source.unlink()
        result = run_stage(verify_only=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        require_failure(run_stage(), "runtime is missing")

        with source.open("xb") as file:
            file.write(b"changed compiler runtime")
        require_failure(run_stage(), "runtime differs from the configured toolset")
        self.assertEqual(staged.read_bytes(), runtime_bytes)


if __name__ == "__main__":
    RuntimeStaging.cmake, RuntimeStaging.stage_script = sys.argv[1:]
    unittest.main(argv=sys.argv[:1])
