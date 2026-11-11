# Historical experiment — retired from product scope

The archive reader/compatibility track described below has been removed. It
was outside the owner's filesystem objective. This document records earlier
work only; current mechanism research is in EXTENT_CODECS.md. Do not restore
archive commands, archive-backed mounts or archive compatibility requirements.

# Native FLZIP CPU recovery expansion — 7 November 2026

Baseline: FlitzFS c2f882102e047585b08c49136504ad52966369d7.
Reference contract: FlitzZip 4e6a1bb0d144611a09dcbc65276faa676e923882.

## Acceptance plan

1. Preserve the 34 MiB snapshot / 32 MiB logical / 16 MiB block admission,
   immutable input, verification-before-exposure, and main-only development.
2. Reproduce unsupported native decoding, then implement independent CPU
   static/adaptive prefix, Huffman, sparse patterns and LZ4 dictionary segments.
3. Implement closed, nonrecursive compound / byte-plane / per-plane stages.
   Maximum scratch is one additional decoded-block extent; never recursive.
4. Use LZ4_decompress_safe, not a new LZ4 implementation or an unsafe API.
   Optional dependency absence is explicit, including inside composed frames.
5. Exercise independent fixture encoders, malformed/truncated metadata and
   payloads, all documented widths/versions, exact original bytes and bounds.
6. Extend the commit-pinned upstream oracle with actual unmodified CPU primitive
   decoders extracted only for tests. No upstream build scripts or HIP loading.
7. Require codec-enabled, dependency-disabled, sanitizer, Windows callback,
   upstream-reference, and hosted synthetic mounted results before claims.

## Research rounds

A. Inspected FlitzZip resource constants, native writer/reader contract and
   prefix/adaptive/Huffman CPU materializers, sparse/dictionary parsers and
   composition/byte-plane validators at the exact pin. Preserve permissive
   legacy prefix segment padding; do not invent an incompatible canonicality
   requirement. CRC remains accidental-corruption detection, not authentication.
B. Verified LZ4 v1.11.0 (4 November 2026), commit
   c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc, safe-decode API and static CMake
   target. CI must build only the static library with threading/CLI disabled.
   Local distro codec versions are recorded separately, not called identical.
C. Rechecked AMD host/device memory and Windows TDR documentation. A CPU
   decoder of GPU-native bytes is NOT GPU execution or evidence of acceleration.
   Neither a child-process timeout nor CPU fallback guarantees driver recovery.
   This milestone must not initialize HIP or run anything on the owner's PC.

## Sources

- https://github.com/strmt7/FlitzZip/tree/4e6a1bb0d144611a09dcbc65276faa676e923882
- https://github.com/lz4/lz4/releases/tag/v1.11.0
- https://github.com/lz4/lz4/blob/c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc/lib/lz4.h
- https://github.com/lz4/lz4/blob/c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc/build/cmake/CMakeLists.txt
- https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/memory_management.html
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery

All changes remain experimental. No speed, authenticated-storage, Windows 11,
owner-GPU, full-namespace or production-safety claim follows from codec tests.

## Implementation and validation checkpoint

The new unsupported-codec regression failed on the baseline as intended. The
independent implementation now covers kinds 4, 5, 6, 7, 9, 10, 11, 12 and 13.
All 15 CTest entries pass locally in codec-enabled, external-codec-disabled and
ASan/UBSan configurations. The enabled native suite performs 1,178 assertions.
Do not count these as a GPU, reference-oracle or mounted Windows qualification.

Round D inspected actual baseline CI logs. Ubuntu's distribution zlib 1.3 did
not meet the existing 1.3.1 configure requirement, so its codec/sanitizer jobs
failed and the gated mount job never ran. Keep the requirement and stage pinned
zlib 1.4 on those lanes; do not hide the failure or disable the checks.

The native reference test verifies the exact Git blob identity before extracting
11 unchanged primitive functions from archive_blocks.cpp. Composition admission
uses upstream headers. It compares fixtures with both implementations; the
product does not link any extracted code. Windows mounted tests additionally
read and map a mixed-native file using all new families under unchanged resource
caps. The first published commit still requires its matching CI outcomes.

Completed implementation-commit CI results are recorded in
[NATIVE_CODEC_VALIDATION.md](NATIVE_CODEC_VALIDATION.md).
