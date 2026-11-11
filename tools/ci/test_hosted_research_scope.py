#!/usr/bin/env python3
"""Regression: an HTTP-200 retired AMD page must not qualify as current research."""
import unittest
from hosted_research import qualify_source_scope

class SourceScopeTests(unittest.TestCase):
    def setUp(self):
        self.row=dict(success=True,native_success=True,http_status=200,final_http_status=200,
            expected_content=True,failure_category="",error="",capture="original.native.json",
            capture_sha256="a"*64)

    def test_actual_retired_banner_preserves_native_evidence(self):
        text=("This ROCm on Radeon™ and Ryzen™ documentation covers releases through 7.2.1. "
              "Starting with ROCm Core SDK 7.13.0, documentation is unified across all supported hardware.")
        result=qualify_source_scope("rocm-windows-hardware",self.row,text)
        self.assertFalse(result["success"])
        self.assertFalse(result["expected_content"])
        self.assertEqual(result["failure_category"],"retired_documentation_scope")
        self.assertIn("7.2.1",result["error"])
        for key in ("native_success","http_status","final_http_status","capture","capture_sha256"):
            self.assertEqual(result[key],self.row[key])
        self.assertTrue(self.row["success"])

    def test_case_and_layout_do_not_hide_the_banner(self):
        for group in ("amd","rocm-current","rocm-windows-hardware"):
            with self.subTest(group=group):
                result=qualify_source_scope(group,self.row,
                    "DOCUMENTATION\n COVERS\t RELEASES through 7.2.1.")
                self.assertFalse(result["success"])

    def test_current_and_windows_sources_preserve_the_original_receipt(self):
        self.assertIs(qualify_source_scope("rocm-current",self.row,
            "ROCm Core SDK 10.1.0 release notes, 2026-10-05"),self.row)
        self.assertIs(qualify_source_scope("windows-adapter-identity",self.row,
            "documentation covers releases through 7.2.1"),self.row)

    def test_existing_native_failure_keeps_its_original_reason(self):
        row=dict(self.row,success=False,native_success=False,error="robots denied",
            failure_category="robots_denied")
        self.assertIs(qualify_source_scope("rocm-current",row,
            "documentation covers releases through 7.2.1"),row)
        self.assertEqual(row["error"],"robots denied")

if __name__=="__main__":
    unittest.main()
