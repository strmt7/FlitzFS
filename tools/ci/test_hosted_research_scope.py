#!/usr/bin/env python3
"""Regression: an HTTP-200 retired AMD page must not qualify as current research."""
import unittest
from hosted_research import qualify_source_scope, ROCM_WARP_SOURCES, COMPRESSION_LEARNED_SOURCES, COMPRESSION_SYSTEMS_SOURCES

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
        for group in ("amd","rocm-current","rocm-windows-hardware","rocm-warp"):
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

class WarpSourceAdmission(unittest.TestCase):
    def test_three_closed_versioned_amd_pages(self):
        self.assertEqual(len(ROCM_WARP_SOURCES),3)
        names=set();urls=set()
        for name,url,marker,term in ROCM_WARP_SOURCES:
            self.assertNotIn(name,names);self.assertNotIn(url,urls)
            names.add(name);urls.add(url)
            self.assertTrue(url.startswith("https://rocm.docs.amd.com/projects/HIP/en/docs-10.1.0/"))
            self.assertNotIn("/latest/",url);self.assertNotIn("/develop/",url)
            self.assertTrue(marker);self.assertTrue(term)
    def test_warp_scope_preserves_original_native_failure(self):
        row=dict(success=False,native_success=False,http_status=403,error="robots denied",
                 failure_category="robots_denied",capture="denied.native.json",capture_sha256="b"*64)
        self.assertIs(qualify_source_scope("rocm-warp",row,
            "documentation covers releases through 7.2.1"),row)

class WarpContentScope(unittest.TestCase):
    def setUp(self):
        self.row=dict(success=True,native_success=True,http_status=200,final_http_status=200,
            expected_content=True,error="",failure_category="",url=ROCM_WARP_SOURCES[0][1],
            capture="retained.native.json",capture_sha256="c"*64)
        # Short markers observed in the retained native 10.1.0 language capture.
        self.content="HIP 7.16.0 Documentation\n# HIP C++ language extensions\n__syncthreads\nwarp"
    def test_observed_language_page_does_not_require_absent_syncwarp(self):
        self.assertNotIn("__syncwarp",self.content)
        self.assertIs(qualify_source_scope("rocm-warp",self.row,self.content),self.row)
    def test_wrong_version_and_uri_preserve_native_capture_identity(self):
        for row,text in ((self.row,self.content.replace("7.16.0","7.15.0")),
                         (dict(self.row,url="https://example.invalid/page"),self.content)):
            with self.subTest(row=row,text=text):
                result=qualify_source_scope("rocm-warp",row,text)
                self.assertFalse(result["success"])
                self.assertEqual(result["failure_category"],"unexpected_documentation_scope")
                for key in ("native_success","http_status","final_http_status","capture","capture_sha256"):
                    self.assertEqual(result[key],row[key])
                self.assertTrue(row["success"])
    def test_title_only_page_does_not_qualify(self):
        result=qualify_source_scope("rocm-warp",self.row,self.content.replace("__syncthreads",""))
        self.assertFalse(result["success"])
        self.assertEqual(result["failure_category"],"unexpected_documentation_scope")


class CompressionSourceScope(unittest.TestCase):
    def test_closed_recent_primary_urls_and_unique_names(self):
        rows=COMPRESSION_LEARNED_SOURCES+COMPRESSION_SYSTEMS_SOURCES
        self.assertEqual(len(rows),6)
        self.assertEqual(len({row[0] for row in rows}),6)
        self.assertEqual(len({row[1] for row in rows}),6)
        for _,url,marker,term in rows:
            self.assertTrue(url.startswith(("https://arxiv.org/html/26",
                "https://dl.acm.org/doi/10.1145/3802078",
                "https://www.mdpi.com/2079-9292/15/20/4712")))
            self.assertTrue(marker and term)
    def test_all_six_full_scope_admissions_and_rejections(self):
        for group,rows in (("compression-learned",COMPRESSION_LEARNED_SOURCES),
                           ("compression-systems",COMPRESSION_SYSTEMS_SOURCES)):
            for _,url,marker,term in rows:
                row=dict(success=True,url=url,native_success=True,http_status=200,
                    capture="retained.native.json",capture_sha256="d"*64)
                content=marker+"\n"+term+"\n"+("primary body "*200)
                with self.subTest(url=url):
                    self.assertIs(qualify_source_scope(group,row,content),row)
                    for changed,text in ((row,marker+" "+term),
                                         (row,content.replace(marker,"wrong title")),
                                         (dict(row,url="https://example.invalid/page"),content)):
                        result=qualify_source_scope(group,changed,text)
                        self.assertFalse(result["success"])
                        self.assertEqual(result["failure_category"],"unexpected_compression_source_scope")
                        self.assertEqual(result["capture_sha256"],row["capture_sha256"])
                        self.assertTrue(changed["success"])
    def test_native_denials_remain_failures(self):
        row=dict(success=False,url=COMPRESSION_LEARNED_SOURCES[0][1],
            native_success=False,error="robots denied",failure_category="robots_denied")
        self.assertIs(qualify_source_scope("compression-learned",row,"x"*3000),row)

if __name__=="__main__":
    unittest.main()
