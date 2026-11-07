# Native FLZIP CPU recovery: executed validation

Date: 7 November 2026. This records completed checks, not projected performance.

Implementation commit: `3761e87cb3105cdd7d3261816c42c363bfd34f23`.
Reviewed/published Git tree: `0f26ed8e7f7c2cc211a4fa5bd5b9f81f30c13ff8`.
The local publication index produced that same tree before the main ref moved.

## Local CPU checks

The baseline passed its existing 14 CTest entries. A new native-codec fixture
failed with the expected unsupported-codec error before implementation. After
implementation, all 15 entries passed in each of GCC with codecs, GCC with all
external codec libraries disabled, and Clang with AddressSanitizer plus
UndefinedBehaviorSanitizer: 45 configuration-matrix executions. The enabled
native-codec executable performed 1,178 assertions. This count includes exact
bytes, boundary reads, invalid tables, framing, version gates and rejection;
it is not 1,178 independent algorithms or a security certification.

Local codec versions were LZ4 1.10.0, zlib 1.3.1 and Zstandard 1.5.7. Hosted
lanes use the documented source pins or distribution libraries; do not describe
all environments as identical. Build parallelism was requested as two workers;
CTest was serial with per-test timeouts. No GPU runtime was initialized.

## Completed hosted runs at the implementation commit

[Main validation run 42407562315](https://github.com/strmt7/FlitzFS/actions/runs/42407562315)
completed with all six jobs successful:

| Lane | Result |
|---|---|
| Ubuntu 24.04, codec-enabled | Passed |
| Ubuntu 24.04, external codecs disabled | Passed |
| Windows Server 2022 / MSVC, external codecs disabled | Passed |
| Linux ASan/UBSan | Passed |
| Windows native callbacks, lifecycle and process controls | Passed |
| Windows driver-backed synthetic mounted I/O | Passed |

[Reference run 42407562259](https://github.com/strmt7/FlitzFS/actions/runs/42407562259)
also completed successfully. Its job 128508446886 passed all 17 CTest entries,
including the real pinned FlitzZip serializer and the separate native CPU
decoder comparison. Upstream reference commit:
`4e6a1bb0d144611a09dcbc65276faa676e923882`; native decoder source blob:
`b1f92fb6b358fbcb37f365de2845092892d2aeda`.
The primitive bodies were extracted unchanged only after checking that identity;
composition admission used the reference headers. The product does not link
this generated reference code. These tests do not execute the full FlitzZip CLI
or establish parity with captures from an actual HIP encoder.

## Real mounted assertions and recorded accounting

Main-run Windows mount job 128508950850 completed all probe stages. It read a
synthetic FLZIP collection containing all newly implemented native families,
compared mapped bytes, exercised random/concurrent reads, rejected mutation,
shut down, and checked that its source bytes were unchanged.

| Helper | Exit/result | Peak job committed bytes |
|---|---|---:|
| FLZIP native/CPU codec mount | 0; normal exit confirmed | 11,517,952 |
| FLFS mounted reads | 0; normal exit confirmed | 9,875,456 |
| Deliberately abandoned FLFS mount | 1460; timeout and termination confirmed | 9,891,840 |
| Independent FLFS recovery/remount | 0; normal exit confirmed | 9,596,928 |

The last two rows qualify the separate read-only FLFS process-termination
scenario, not FLZIP power-loss or writable recovery. The helpers retained the
128 MiB committed-memory, 20% CPU, one-process and bounded wall-time policies.
Reported bytes do not measure total physical RAM, kernel memory or VRAM.

## Inherited CI repair

Baseline run 42396654751 had failed Linux codec/sanitizer configuration because
the distribution zlib did not satisfy the existing 1.3.1 minimum; its mount job
was skipped. The implementation stages pinned zlib 1.4 in those lanes without
system installation, weakening the requirement, or skipping the failed tests.
Their successful outcomes above are new evidence, not a reinterpretation of
the failed baseline run. Windows/reference LZ4 is pinned to 1.11.0 commit
`c72c7c0ec132ad2ce2fe9fad7b0c728d9faee2fc`.

## Release boundary

All defined block kinds from the pinned v1-11 contract now have CPU decoding
paths when their optional libraries are present. Archive size, namespace,
count and resource restrictions still apply; see FLZIP_READONLY.md.
This is CPU recovery of GPU-native representations, not GPU acceleration.
No owner-PC commands, HIP operations, user-data mounts, encryption, authenticated
storage or transactional writes were performed or qualified. A finite synthetic
suite cannot guarantee no freeze, no corruption or arbitrary application safety.
Use this as the independent CPU reference for future bounded GPU parity work,
not as permission to enable unqualified GPU or writable-volume operations.
