#!/usr/bin/env python3
"""CPU-only staging boundary regressions, restricted to private temporary fixtures."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from stage_lz4_runtime_bundle import stage

class BundleTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix="flitzfs-lz4-bundle-")
        self.addCleanup(self.temp.cleanup)
        self.root=Path(self.temp.name)
        self.module=self.root/"candidate.dll";self.module.write_bytes(b"private module fixture")
        self.obj=self.root/"candidate.obj";self.obj.write_bytes(b"qualified object fixture")
        self.proof=self.root/"coverage.json"
        self.record=dict(control_commit="a"*40,complete=True,object_inspected=True,
            physical_inputs_unchanged=True,variant="bounded-native-wave-entry",
            gpu_executed=False,module_loaded=False,synthetic_compile_target="gfx1100",
            offload_target_ids=["hip-amdgcn-amd-amdhsa--gfx1100"],entry_object=dict(complete=True),
            runtime_target_fixture=True,compile_targets=["gfx1100"],
            entry_objects={"gfx1100":dict(complete=True,architecture="gfx1100",loaded=False,executed=False,
                symbols=["flitzfs_candidate_lz4_wave32","flitzfs_candidate_lz4_wave64"])},
            object=dict(bytes=self.obj.stat().st_size,sha256=hashlib.sha256(self.obj.read_bytes()).hexdigest()))
        self.save()
        self.imports=patch("stage_lz4_runtime_bundle.pe_import_names",
            return_value=(["amdhip64_7.dll","kernel32.dll"],[]))
        self.exports=patch("stage_lz4_runtime_bundle.pe_function_exports",
            return_value={"flitzfs_candidate_lz4_api"})
        self.import_mock=self.imports.start();self.addCleanup(self.imports.stop)
        self.export_mock=self.exports.start();self.addCleanup(self.exports.stop)
    def save(self):
        self.proof.write_text(json.dumps(self.record),encoding="utf-8")
    def run_stage(self):
        return stage(self.module,self.obj,self.proof,self.root/"hip-lz4",
            self.root/"bundle.hpp",self.root/"receipt.json","amdhip64_7.dll","a"*40)
    def rejected(self):
        with self.assertRaises((ValueError,RuntimeError,OSError)):
            self.run_stage()
    def test_pins_complete_copy_and_exact_coverage(self):
        before=self.module.read_bytes(),self.obj.read_bytes(),self.proof.read_bytes()
        result=self.run_stage()
        self.assertEqual((self.root/"hip-lz4/flitzfs_rocm_lz4_candidate.dll").read_bytes(),before[0])
        self.assertEqual(result["coverage"]["targets"][0]["wave_mask"],1)
        self.assertFalse(result["runtime_shipped"])
        self.assertFalse(result["module_loaded"])
        self.assertIn(hashlib.sha256(before[0]).hexdigest(),(self.root/"bundle.hpp").read_text())
        self.assertEqual(before,(self.module.read_bytes(),self.obj.read_bytes(),self.proof.read_bytes()))
    def test_wrong_source_lease(self):
        self.record["control_commit"]="b"*40;self.save();self.rejected()
    def test_changed_object(self):
        self.obj.write_bytes(b"other object fixture");self.rejected()
    def test_unqualified_target(self):
        self.record["compile_targets"]=["gfx1201"];self.save();self.rejected()
    def test_missing_or_extra_images(self):
        self.record["offload_target_ids"].append("hip-amdgcn-amd-amdhsa--gfx1201")
        self.save();self.rejected()
    def test_unconfirmed_native_metadata(self):
        self.record["entry_objects"]["gfx1100"]["complete"]=False;self.save();self.rejected()
    def test_previously_loaded_or_incomplete_object(self):
        self.record["module_loaded"]=True;self.save();self.rejected()
    def test_unexpected_import(self):
        self.import_mock.return_value=(["amdhip64_7.dll","other.dll"],[]);self.rejected()
    def test_delayed_import(self):
        self.import_mock.return_value=(["amdhip64_7.dll"],["kernel32.dll"]);self.rejected()
    def test_missing_or_extra_getter(self):
        self.export_mock.return_value={"flitzfs_candidate_lz4_api","unexpected"};self.rejected()
    def test_exclusive_output_collision_preserves_existing_bytes(self):
        header=self.root/"bundle.hpp";header.write_bytes(b"preserve")
        self.rejected();self.assertEqual(header.read_bytes(),b"preserve")
        self.assertFalse((self.root/"hip-lz4").exists())
    def test_hardlinked_source_rejected(self):
        import os
        alias=self.root/"alias.dll";os.link(self.module,alias);self.rejected()

if __name__=="__main__":
    unittest.main()

