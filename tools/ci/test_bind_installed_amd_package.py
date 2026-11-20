#!/usr/bin/env python3
"""CPU-only checks for the approved AMD package manifest and deployment plan."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from bind_installed_amd_package import (
    ANCHOR_EVIDENCE, ANCHOR_MODULE, ANCHOR_WORKER, UNIFORM_MODULE, UNIFORM_WORKER, bind,
)


class PackageTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="flitzfs-amd-binding-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.manifest = dict(runtime_basename="amdhip64_7.dll", runtime_shipped=False,
                             kpack_shipped=False, files=[])
        imports = patch("bind_installed_amd_package.pe_import_names", return_value=(["kernel32.dll"], []))
        self.imported = imports.start()
        self.addCleanup(imports.stop)

    def file(self, name):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        contents = name.encode("ascii")
        with path.open("xb") as output:
            output.write(contents)
        digest = hashlib.sha256(contents).hexdigest()
        self.manifest["files"].append(dict(path=name, bytes=len(contents), sha256=digest))
        return digest

    def uniform(self):
        self.manifest["uniform_runtime_packaged"] = True
        for name in (UNIFORM_WORKER, UNIFORM_MODULE):
            self.file(name)

    def anchor(self):
        for name in (ANCHOR_WORKER, ANCHOR_MODULE):
            self.file(name)
        profile = dict(qualification_sha256=self.file(ANCHOR_EVIDENCE), runtime_sha256="ab" * 32,
                       vendor_id=0x1002, device_id=1, subsystem_id=0, revision=0,
                       minimum_members=4, maximum_members=16, worker_memory_mib=128,
                       memory_share_divisor=16, worker_cpu_rate=2000, worker_timeout_ms=5000,
                       maximum_identity_queries=4, duty_ppm=10000)
        self.manifest["shared_base_profile"] = profile
        return profile

    def bind(self, trusted=None):
        payload = json.dumps(self.manifest).encode("ascii")
        with (self.root / "qualification-manifest.json").open("xb") as output:
            output.write(payload)
        return bind(self.root, trusted if trusted else hashlib.sha256(payload).hexdigest())

    def test_uniform_only_manifest_keeps_its_route(self):
        self.uniform()
        header, files = self.bind()
        self.assertIn("has_uniform=true", header)
        self.assertIn("FLITZFS_INSTALLED_AMD_HAS_ANCHOR 0", header)
        self.assertEqual(files, ["qualification-manifest.json", UNIFORM_WORKER, UNIFORM_MODULE])

    def test_anchor_only_manifest_stages_just_the_qualified_route_and_notice(self):
        self.anchor()
        self.file("licenses/blake3/LICENSE_CC0")
        self.file("unqualified.dll")
        header, files = self.bind()
        self.assertIn("has_uniform=false", header)
        self.assertIn("anchor_minimum_members=4U", header)
        self.assertIn("anchor_dispatches_per_wait=1U", header)
        self.assertIn("anchor_maximum_dispatch_members=0U", header)
        self.assertIn("anchor_reference_xor=false", header)
        self.assertEqual(files, ["qualification-manifest.json", ANCHOR_EVIDENCE, ANCHOR_WORKER,
                                ANCHOR_MODULE, "licenses/blake3/LICENSE_CC0"])

    def test_combined_package_has_independent_workers(self):
        self.uniform()
        self.anchor()
        header, files = self.bind()
        self.assertIn("has_uniform=true", header)
        self.assertIn("FLITZFS_INSTALLED_AMD_HAS_ANCHOR 1", header)
        self.assertEqual(set(files), {"qualification-manifest.json", UNIFORM_WORKER, UNIFORM_MODULE,
                                     ANCHOR_WORKER, ANCHOR_MODULE, ANCHOR_EVIDENCE})

    def test_manifest_approval_is_required(self):
        self.anchor()
        with self.assertRaisesRegex(RuntimeError, "manifest digest mismatch"):
            self.bind(trusted="cd" * 32)

    def test_profile_cannot_refer_to_a_different_qualification(self):
        self.anchor()["qualification_sha256"] = "ef" * 32
        with self.assertRaisesRegex(RuntimeError, "qualification digest mismatch"):
            self.bind()

    def test_invalid_window_geometry_is_refused(self):
        self.anchor()["minimum_members"] = 17
        with self.assertRaisesRegex(RuntimeError, "Inverted anchor work range"):
            self.bind()

    def test_profile_can_cover_several_indexed_scan_windows(self):
        self.anchor().update(minimum_members=512, maximum_members=2048, dispatches_per_wait=4)
        header, _ = self.bind()
        self.assertIn("anchor_minimum_members=512U", header)
        self.assertIn("anchor_maximum_members=2048U", header)
        self.assertIn("anchor_dispatches_per_wait=4U", header)

    def test_profile_can_enable_versioned_wide_copy_dispatch(self):
        self.anchor()["maximum_dispatch_members"] = 12
        with patch("bind_installed_amd_package.pe_function_exports",
                   return_value={"flitzfs_rocm_anchor_window_api_v11"}):
            header, _ = self.bind()
        self.assertIn("anchor_maximum_dispatch_members=12U", header)

    def test_profile_rejects_a_legacy_copy_module_for_wide_dispatch(self):
        self.anchor()["maximum_dispatch_members"] = 12
        with patch("bind_installed_amd_package.pe_function_exports",
                   return_value={"flitzfs_rocm_anchor_window_api_v2"}):
            with self.assertRaisesRegex(RuntimeError, "lacks its dispatch entry"):
                self.bind()

    def test_profile_can_enable_device_reference_pricing(self):
        self.anchor().update(search_mode="reference-xor", maximum_dispatch_members=12)
        with patch("bind_installed_amd_package.pe_function_exports", return_value={
            "flitzfs_rocm_anchor_match_api_v1", "flitzfs_rocm_anchor_sparse_xor_arena_api_v8",
            "flitzfs_rocm_anchor_reference_api_v10", "flitzfs_rocm_anchor_window_api_v11",
        }):
            header, _ = self.bind()
        self.assertIn("anchor_reference_xor=true", header)
        self.assertIn("anchor_maximum_dispatch_members=12U", header)

    def test_reference_profile_requires_sparse_fallback_in_the_same_module(self):
        self.anchor().update(search_mode="reference-xor", maximum_dispatch_members=12)
        with patch("bind_installed_amd_package.pe_function_exports", return_value={
            "flitzfs_rocm_anchor_match_api_v1", "flitzfs_rocm_anchor_reference_api_v10",
            "flitzfs_rocm_anchor_window_api_v11",
        }):
            with self.assertRaisesRegex(RuntimeError, "flitzfs_rocm_anchor_sparse_xor_arena_api_v8"):
                self.bind()

    def test_reference_profile_requires_its_copy_search_entry(self):
        self.anchor().update(search_mode="reference-xor", maximum_dispatch_members=3)
        with patch("bind_installed_amd_package.pe_function_exports", return_value={
            "flitzfs_rocm_anchor_match_api_v1", "flitzfs_rocm_anchor_sparse_xor_arena_api_v8",
            "flitzfs_rocm_anchor_reference_api_v10", "flitzfs_rocm_anchor_window_api_v11",
        }):
            with self.assertRaisesRegex(RuntimeError, "flitzfs_rocm_anchor_window_api_v2"):
                self.bind()

    def test_reference_profile_requires_a_measured_dispatch_grant(self):
        self.anchor()["search_mode"] = "reference-xor"
        with self.assertRaisesRegex(RuntimeError, "Reference search requires a dispatch grant"):
            self.bind()

    def test_reference_profile_rejects_copy_queue_settings(self):
        self.anchor().update(search_mode="reference-xor", maximum_dispatch_members=12, dispatches_per_wait=2)
        with self.assertRaisesRegex(RuntimeError, "Reference search requires a single dispatch per wait"):
            self.bind()

    def test_profile_rejects_an_unknown_search_mode(self):
        self.anchor()["search_mode"] = "unqualified"
        with self.assertRaisesRegex(RuntimeError, "Invalid anchor profile field search_mode"):
            self.bind()

    def test_dispatch_must_fit_the_approved_batch(self):
        self.anchor()["maximum_dispatch_members"] = 17
        with self.assertRaisesRegex(RuntimeError, "Anchor dispatch exceeds approved batch work"):
            self.bind()

    def test_profile_refuses_an_unverified_dispatch_queue(self):
        self.anchor()["dispatches_per_wait"] = 5
        with self.assertRaisesRegex(RuntimeError, "Invalid anchor profile field dispatches_per_wait"):
            self.bind()

    def test_install_plan_cannot_alias_windows_paths(self):
        self.anchor()
        self.manifest["files"].append(dict(path=ANCHOR_WORKER.upper()))
        with self.assertRaisesRegex(RuntimeError, "repeated paths"):
            self.bind()

    def test_worker_may_not_import_the_optional_runtime(self):
        self.anchor()
        self.imported.return_value = (["kernel32.dll", self.manifest["runtime_basename"]], [])
        with self.assertRaisesRegex(RuntimeError, "Unexpected PE dependencies"):
            self.bind()


if __name__ == "__main__":
    unittest.main()
