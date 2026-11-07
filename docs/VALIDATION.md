# Validation ledger — experimental release 0.2

Date: **7 November 2026**. This page distinguishes implemented functionality,
executed tests and unpassed integration gates. It supersedes earlier statements
that Windows CPU/native callback validation was still pending. Historical
results remain in Git history and the earlier CI runs below.

## Latest code qualification

Code commit: **`dcb3a8b313af6e7b77453acf9474e506ef5bc83f`**.
GitHub Actions: [run 42377524934](https://github.com/strmt7/FlitzFS/actions/runs/42377524934)
completed **success**. The preceding physical-path safety commit
`62907088f767b2d67481bfb36ffff9bc3ef9bbfe` also passed all five jobs in
[run 42376916260](https://github.com/strmt7/FlitzFS/actions/runs/42376916260).

| Lane | Actual coverage | Result |
|---|---|---|
| Ubuntu 24.04 + CPU Zstandard | Five portable CTest executables | PASS |
| Ubuntu 24.04 raw-only | Five portable executables without codec dependency | PASS |
| Windows Server 2022 / MSVC raw-only | Five portable executables on Windows | PASS |
| Linux Clang ASan + UBSan | Five portable executables, fail on sanitizer report | PASS |
| Windows Server 2022 / MSVC + Zstandard + WinFsp SDK | Portable tests plus native callback executable (six total) | PASS |

The final portable sources were also rebuilt and tested locally with GCC 14.2
plus Zstandard, GCC raw-only, and Clang 17 ASan/UBSan: five executables in each
configuration, **15 passed executions**. This is a count of executable suites,
not a claim of thousands of independent tests or a performance benchmark.

The native lane builds commit-pinned Zstandard 1.5.7 and asserts that its
compressible fixture actually generated Zstandard chunks. It uses the official
WinFsp 2.1 user-mode headers/library/DLL, not substitute SDK declarations.
The tests invoke callbacks directly against a user-mode FSP_FILE_SYSTEM struct.
**No filesystem is created, dispatcher started, mount assigned or kernel driver
installed/loaded by the test code.** No operations ran on the owner's PC/GPU.

## What this development round added and checked

- **Entire filename validation:** a regression reproduced the previous bug
  where validation stopped at the first dot. ADS syntax, wildcards, control/NUL,
  separators and unsupported high bytes in extensions now fail validation.
- **Pre-allocation admission:** index size is checked against the remaining
  catalog allowance before reading or allocating it. Tiny forged headers prove
  rejection before reaching absent index/footer payloads. Empty indices can
  fit a zero budget. Serialized-index size is not a full process-memory cap.
- **Native callbacks:** typed NTSTATUS failures, Open/Close, stable metadata,
  short/EOF reads, cross-chunk byte parity, readonly Flush and explicit denials
  for create/write/delete/truncate/rename/ACL/label mutations.
- **Native lifecycle and bounds:** Cleanup preserves later reads until Close;
  two concurrent readers are byte-exact; 256-context exhaustion fails closed;
  stale-close tokens do not close a newly reused slot. Invalid read sizes and
  null buffers are rejected. Construction refuses a corrupted fixture.
- **Security descriptors:** valid protected single-owner read/execute ACE,
  correct size negotiation, no write grant and no undersized-buffer overwrite.
  This is not qualification of actual mounted authorization or source ACL parity.
- **Directory continuation:** SDK-packed records, short output buffers, markers,
  case folding and dot entries. A portable regression reproduced skipped
  punctuation/space-prefixed names with plain lexical markers. The shared
  explicit dot-rank comparator and expanded native pagination test now pass.
- **Physical host paths:** pure no-I/O tests cover device/extended namespaces,
  reserved aliases including superscript digits, streams, NUL, ambiguous suffixes
  and length limits. Synthetic integration tests ensure NUL paths cannot create
  a truncated-name output. No test opens a physical device.

## Dependency and CI failures retained, not hidden

The first native run stopped on duplicate DLL names in the official MSI. A
narrow correction then exposed a duplicated import-library basename: the SDK
and optional fsext payloads have the same filename. Both failures were in SDK
staging, not suppressed tests. The final extractor selects exact component IDs
from pinned v2.1 Product.wxs, verifies the release checksum, and still rejects
ambiguous payloads. It never executes installer/custom actions.

The corrected initial callback milestone passed in
[run 42376512623](https://github.com/strmt7/FlitzFS/actions/runs/42376512623).
The Windows build logs include an upstream WinFsp alignment-padding warning
(C4324), and the pinned checkout v4 action triggers a Node20-deprecation warning
while the hosted runner forces Node24. Neither is hidden or presented as a
warning-free build. No broad warning suppression was added.

## Research rounds and decisions

The detailed ABI/lifetime/security/dependency research is in
[WINFSP_CALLBACKS.md](WINFSP_CALLBACKS.md). Subsequent rounds checked:

1. Exact SDK component identities, rather than selecting the first matching
   filename: [WinFsp v2.1 Product.wxs](https://github.com/winfsp/winfsp/blob/v2.1/build/VStudio/installer/Product.wxs).
2. Microsoft namespace/device alias semantics before physical I/O:
   [Naming Files, Paths, and Namespaces](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file).
3. Directory markers must use the same order as emitted directory records,
   including special dot entries: [WinFsp native API](https://winfsp.dev/doc/WinFsp-API-winfsp.h/).

The prior review of all 13 other repositories remains in
[RESEARCH_2026-11-07.md](RESEARCH_2026-11-07.md). No other repository was modified
and no private code was copied into this public project.

## Earlier retained milestones

M1: CPU core `84efaa8`; M1.5: handle/catalog commit `51adb76`, Windows CI
42369383337; M1.6: concurrency commit `63ec05f`, CI 42370249875; M1.7: projection
commit `f39e927`, CI 42370565055. Older local-only qualification notes describe
their point in time, not the current native callback state.

## Explicitly NOT qualified or implemented

There is still no mount executable, persistent directory manifest, `.flzip`
compatibility, AMD HIP execution, encryption/authentication, transactional
writable volume or power-loss recovery. Mounted Windows I/O, mmap, shutdown and
cancellation, real ACL enforcement, Windows 11, application compatibility,
ThreadSanitizer, long soaks, broad fuzzing, independent security audit and
end-to-end performance comparisons remain unpassed. Parent-path link/race
containment is incomplete; lexical gates are not a general sandbox.

Passing these tests does not establish production safety, immunity to hangs or
resource exhaustion, or any GPU speed/compression advantage. Preserve original
data and do not install or mount this preview on the owner's PC automatically.
