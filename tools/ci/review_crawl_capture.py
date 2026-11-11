#!/usr/bin/env python3
"""Inspect exact admitted native Crawl4AI captures remotely; never refetch a page."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource

EXPECTED = json.loads(r'''[{"url":"https://www.sqlite.org/testing.html","success":true,"native_success":true,"http_status":200,"final_http_status":200,"markdown_bytes":52519,"markdown_sha256":"3b623da9701b14a5884370a002c92534109c55dcd2d36d3ece49a1ac5e5c525b","expected_content":true,"failure_category":"","error":"","seconds":0.341,"capture":"sqlite-testing.native.json","capture_sha256":"923ee821882db3f124b545421e9ad105f909149c413c412d7840e9b7212a2ae4","name":"sqlite-testing"},{"url":"https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching","success":true,"native_success":true,"http_status":200,"final_http_status":200,"markdown_bytes":22863,"markdown_sha256":"d528744cabcc2d9aada50539f4a8385800cb5f43119963fb36bb5c1841233244","expected_content":true,"failure_category":"","error":"","seconds":1.273,"capture":"windows-cache.native.json","capture_sha256":"d8080a364fbfb44d6f8ce0d3f97e74c5c1077506448b7bb03c7705715140b285","name":"windows-cache"},{"url":"https://docs.openssl.org/3.5/man3/EVP_DigestInit/","success":true,"native_success":true,"http_status":200,"final_http_status":200,"markdown_bytes":700005,"markdown_sha256":"3657aca0a416faf7c12f3c97f9d7d398bedc9ffe220621d94a9b66379128a018","expected_content":true,"failure_category":"","error":"","seconds":2.887,"capture":"openssl-digest.native.json","capture_sha256":"4d76f6506cd4cc1e0f9c59a49a0de7b774de3aa8c2354475bae4c0d213fb20b0","name":"openssl-digest"}]''')
FOCUS = {
    "windows-cache": ("FILE_FLAG_WRITE_THROUGH", "FlushFileBuffers"),
    "openssl-digest": ("EVP_Digest()", "## RETURN VALUES"),
    "sqlite-testing": ("Crash Testing",),
}


def main():
    if os.environ.get("GITHUB_ACTIONS") != "true" or os.environ.get("RUNNER_ENVIRONMENT") != "github-hosted":
        raise RuntimeError("Requires a disposable GitHub-hosted runner")
    resource.setrlimit(resource.RLIMIT_AS, (256 * 1024**2, 256 * 1024**2))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 10))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    root = args.directory.resolve(strict=True)
    for row in EXPECTED:
        path = root / row["capture"]
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 8 * 1024**2:
            raise RuntimeError("Invalid capture file")
        raw = path.read_bytes()
        if hashlib.sha256(raw).hexdigest() != row["capture_sha256"]:
            raise RuntimeError("Native capture digest changed")
        document = json.loads(raw)
        if document["requested_url"] != row["url"] or document["provider_version"] != "0.9.5+flitzzip.portable2":
            raise RuntimeError("Native source identity changed")
        native = document["native_result"]
        markdown = native.get("markdown") or ""
        if isinstance(markdown, dict):
            markdown = markdown.get("raw_markdown", "")
        if not isinstance(markdown, str) or hashlib.sha256(markdown.encode()).hexdigest() != row["markdown_sha256"]:
            raise RuntimeError("Extracted text changed")
        headings = [(match.start(), match.group(0)) for match in re.finditer(r"(?m)^#{1,6}\s+[^\n]+", markdown)]
        # Restrict API review to real Markdown section headings after site navigation.
        relevant = [(pos, heading) for pos, heading in headings
                    if any(term in heading.lower() for term in ("description", "synopsis", "return values", "file caching", "crash testing"))]
        print(json.dumps({"source": row["name"], "native_sha256": row["capture_sha256"],
                          "relevant_sections": relevant[-12:]}), flush=True)
        if not relevant:
            raise RuntimeError("No substantive section found; review cannot use navigation")
        if row["name"] == "openssl-digest":
            descriptions = [pos for pos, heading in relevant if "description" in heading.lower()]
            if not descriptions:
                raise RuntimeError("Missing digest description")
            body_start = descriptions[-1]
        elif row["name"] == "sqlite-testing":
            body_start = next(pos for pos, heading in relevant if "crash testing" in heading.lower())
        else:
            body_start = next(pos for pos, heading in relevant if "file caching" in heading.lower())
        for term in FOCUS[row["name"]]:
            pos = markdown.lower().find(term.lower(), body_start)
            if pos < 0:
                raise RuntimeError("Relevant API text absent: " + term)
            print(json.dumps({"review": row["name"], "term": term,
                              "excerpt": markdown[max(body_start, pos-160):pos+1200]}), flush=True)


if __name__ == "__main__":
    main()
