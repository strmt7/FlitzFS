#!/usr/bin/env python3
"""Emit test-only, byte-preserved functions from an exact reviewed upstream blob.

Inputs: path to the pinned FlitzZip src/core/archive_blocks.cpp. Output: a C++
translation unit on stdout; never modifies the reference checkout or product.
This is a fixed-source extractor, not a general C++ parser.
"""
from __future__ import annotations

import hashlib
from pathlib import Path
import sys

BLOB_SHA = "b1f92fb6b358fbcb37f365de2845092892d2aeda"
FUNCTIONS = (
    "void materialize_pattern_cpu(",
    "std::uint32_t read_segment_u32(",
    "std::uint32_t read_prefix_bit(",
    "std::uint32_t read_prefix_bits(",
    "std::byte decode_prefix_byte_cpu(",
    "std::byte decode_adaptive_prefix_byte_cpu(",
    "void materialize_prefix_cpu(",
    "void materialize_adaptive_prefix_cpu(",
    "void materialize_huffman_cpu(",
    "void materialize_dictionary_cpu(",
    "void materialize_sparse_pattern_cpu(",
)
HEADER = r'''// GENERATED TEST ONLY: exact FlitzZip 4e6a1bb function bodies.
// Derived from strmt7/FlitzZip, AGPL-3.0. Not linked into the product.
#include "core/archive_block_types.hpp"
#include "core/resource_limits.hpp"
#include "core/result.hpp"
#include "core/dictionary_block.hpp"
#include "core/sparse_pattern_block.hpp"
#include "core/huffman_lookup.hpp"
#include "core/compound_block.hpp"
#include "core/byte_plane_block.hpp"
#include <lz4.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace flitzzip {
namespace {
'''


def extract(source: bytes) -> str:
    """Check exact Git blob identity before extracting complete original functions."""
    if len(source) > 65536:
        raise ValueError("upstream source exceeds the reviewed bound")
    identity = hashlib.sha1(b"blob " + str(len(source)).encode("ascii") + b"\0" + source).hexdigest()
    if identity != BLOB_SHA:
        raise ValueError("upstream native decoder blob is not the reviewed source")
    text = source.decode("utf-8")
    bodies = []
    for signature in FUNCTIONS:
        marker = "\n" + signature
        if text.count(marker) != 1:
            raise ValueError("reference function identity is ambiguous")
        start = text.index(marker) + 1
        end = text.find("\n}\n", start)
        if end < 0:
            raise ValueError("reference function is unterminated")
        bodies.append(text[start:end + 3])
    return HEADER + "\n".join(bodies)


def main() -> int:
    """Emit a bounded reference unit or fail without producing partial output."""
    if len(sys.argv) != 2:
        raise ValueError("usage: extract_extent_codec_oracle.py ARCHIVE_BLOCKS_CPP")
    path = Path(sys.argv[1])
    if path.stat().st_size > 65536:
        raise ValueError("upstream source exceeds the reviewed bound")
    with path.open("rb") as source:
        unit = extract(source.read(65537))
    tail = Path(__file__).resolve().parents[2] / "tests" / "extent_codec_oracle_tail.inc"
    suffix = tail.read_text(encoding="utf-8")
    if len(suffix) > 16384:
        raise ValueError("reference adapter exceeds its bound")
    sys.stdout.write(unit + suffix)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, UnicodeError) as error:
        print(f"native oracle rejected: {error}", file=sys.stderr)
        raise SystemExit(1) from error
