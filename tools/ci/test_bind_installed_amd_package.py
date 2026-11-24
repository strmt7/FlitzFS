#!/usr/bin/env python3
"""CPU-only checks for the approved AMD package manifest and deployment plan."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from bind_installed_amd_package import (
    ANCHOR_EVIDENCE, ANCHOR_MODULE, ANCHOR_WORKER, READ_EVIDENCE, READ_MODULE, READ_WORKER,
    UNIFORM_MODULE, UNIFORM_TRIAL_EVIDENCE, UNIFORM_WORKER, bind,
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

    def write_trial(self):
        self.uniform()
        profile = dict(qualification_sha256=self.file(UNIFORM_TRIAL_EVIDENCE), runtime_sha256="ab" * 32,
                       vendor_id=0x1002, device_id=1, subsystem_id=0, revision=0,
                       worker_memory_mib=128, memory_share_divisor=16, worker_cpu_rate=2000,
                       worker_timeout_ms=5000, maximum_identity_queries=4,
                       maximum_batch_chunks=16, maximum_launch_chunks=2)
        self.manifest["uniform_write_trial_profile"] = profile
        return profile

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

    def native_read(self):
        for name in (READ_WORKER, READ_MODULE):
            self.file(name)
        profile = dict(qualification_sha256=self.file(READ_EVIDENCE), runtime_sha256="cd" * 32,
                       vendor_id=0x1002, device_id=0x7550, subsystem_id=1, revision=0,
                       minimum_frames=2, maximum_frames=12, worker_memory_mib=160,
                       memory_share_divisor=16, worker_cpu_rate=1200, worker_timeout_ms=14000,
                       maximum_identity_queries=4, architecture="gfx1201", wave_mask=1,
                       maximum_slab_bytes=1024 * 1024, minimum_lead_ms=120)
        self.manifest["native_lz4_read_profile"] = profile
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
        self.assertIn("FLITZFS_INSTALLED_AMD_HAS_WRITE_TRIAL 0", header)
        self.assertIn("FLITZFS_INSTALLED_AMD_HAS_ANCHOR 0", header)
        self.assertEqual(files, ["qualification-manifest.json", UNIFORM_WORKER, UNIFORM_MODULE])

    def test_write_trial_binds_distinct_burst_and_launch_limits(self):
        self.write_trial()
        header, files = self.bind()
        self.assertIn("FLITZFS_INSTALLED_AMD_HAS_WRITE_TRIAL 1", header)
        self.assertIn("write_trial_maximum_batch_chunks=16U", header)
        self.assertIn("write_trial_maximum_launch_chunks=2U", header)
        self.assertIn("write_trial_separate_requests=false", header)
        self.assertIn("write_trial_await_later_writes=false", header)
        self.assertIn("write_trial_allow_smaller_writes=false", header)
        self.assertEqual(files, ["qualification-manifest.json", UNIFORM_WORKER, UNIFORM_MODULE, UNIFORM_TRIAL_EVIDENCE])

    def test_write_trial_binds_separate_request_admission(self):
        self.write_trial()["separate_requests"] = True
        header, _ = self.bind()
        self.assertIn("write_trial_separate_requests=true", header)

    def test_write_trial_rejects_non_boolean_admission(self):
        self.write_trial()["separate_requests"] = 1
        with self.assertRaisesRegex(RuntimeError, "Invalid write trial separate_requests"):
            self.bind()

    def test_write_trial_binds_arriving_request_admission(self):
        profile = self.write_trial()
        profile.update(separate_requests=True, await_later_writes=True)
        header, _ = self.bind()
        self.assertIn("write_trial_await_later_writes=true", header)

    def test_write_trial_rejects_arrival_without_separate_requests(self):
        self.write_trial()["await_later_writes"] = True
        with self.assertRaisesRegex(RuntimeError, "Invalid write trial await_later_writes"):
            self.bind()

    def test_write_trial_binds_smaller_arrivals(self):
        profile = self.write_trial()
        profile.update(separate_requests=True, await_later_writes=True, allow_smaller_writes=True)
        header, _ = self.bind()
        self.assertIn("write_trial_allow_smaller_writes=true", header)

    def test_write_trial_rejects_smaller_arrivals_without_a_finishable_worker(self):
        profile = self.write_trial()
        profile.update(separate_requests=True, allow_smaller_writes=True)
        with self.assertRaisesRegex(RuntimeError, "Invalid write trial allow_smaller_writes"):
            self.bind()

    def test_write_trial_rejects_non_boolean_smaller_arrivals(self):
        self.write_trial().update(separate_requests=True, await_later_writes=True, allow_smaller_writes=1)
        with self.assertRaisesRegex(RuntimeError, "Invalid write trial allow_smaller_writes"):
            self.bind()

    def test_write_trial_requires_uniform_worker(self):
        self.write_trial()
        self.manifest["uniform_runtime_packaged"] = False
        with self.assertRaisesRegex(RuntimeError, "Write trial profile requires the uniform worker"):
            self.bind()

    def test_write_trial_refuses_another_qualification(self):
        self.write_trial()["qualification_sha256"] = "ef" * 32
        with self.assertRaisesRegex(RuntimeError, "Write trial qualification digest mismatch"):
            self.bind()

    def test_write_trial_refuses_an_oversized_launch(self):
        self.write_trial()["maximum_launch_chunks"] = 17
        with self.assertRaisesRegex(RuntimeError, "Write trial launch exceeds its approved batch"):
            self.bind()

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

    def test_native_read_package_stages_the_separate_decoder_role(self):
        self.native_read()
        self.file("unqualified.dll")
        with patch("bind_installed_amd_package.pe_function_exports",
                   return_value={"flitzfs_native_lz4_decode_api"}):
            header, files = self.bind()
        self.assertIn("FLITZFS_INSTALLED_AMD_HAS_NATIVE_READ 1", header)
        self.assertIn("has_uniform=false", header)
        self.assertIn('read_architecture[]=L"gfx1201"', header)
        self.assertIn("read_minimum_lead_ms=120U", header)
        self.assertEqual(files, ["qualification-manifest.json", READ_EVIDENCE, READ_WORKER, READ_MODULE])

    def test_native_read_requires_verified_dispatch(self):
        self.native_read()
        with patch("bind_installed_amd_package.pe_function_exports", return_value=set()):
            with self.assertRaisesRegex(RuntimeError, "flitzfs_native_lz4_decode_api"):
                self.bind()

    def test_native_read_requires_a_measured_lead_grant(self):
        self.native_read()["minimum_lead_ms"] = 0
        with self.assertRaisesRegex(RuntimeError, "minimum_lead_ms"):
            self.bind()

    def test_native_read_rejects_mismatched_provenance(self):
        self.native_read()["qualification_sha256"] = "ef" * 32
        with self.assertRaisesRegex(RuntimeError, "Read qualification digest mismatch"):
            self.bind()

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
        with self.assertRaisesRegex(RuntimeError, "Invalid AMD profile field dispatches_per_wait"):
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
