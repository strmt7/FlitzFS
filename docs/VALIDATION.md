# Validation ledger — experimental release 0.2

Date: **7 November 2026**. This page distinguishes implemented functionality,
executed tests and unpassed integration gates. It supersedes earlier statements
that Windows CPU/native callback validation was still pending. Historical
results remain in Git history and the earlier CI runs below.

## Initial callback-only code qualification (historical)

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

There is still no general-purpose mount application or full `.flzip` codec compatibility,
AMD HIP execution, encryption/authentication, transactional writable volume or
power-loss recovery. The synthetic hosted mount and process-crash qualifications
are recorded below; persistent plaintext catalogs are now implemented. These
do not qualify in-flight cancellation, cross-user ACL enforcement, Windows 11,
broad application compatibility, ThreadSanitizer, long soaks, broad fuzzing,
independent security audit or end-to-end performance comparisons. Parent-path
link/race containment is incomplete; lexical gates are not a general sandbox.

Passing these tests does not establish production safety, immunity to hangs or
resource exhaustion, or any GPU speed/compression advantage. Preserve original
data and do not install or mount this preview on the owner's PC automatically.

## Continuation: lifecycle, process limits and real-mount probe

At base `41cff2eea5d77f5b9582cca16c4b3c192f1e6a7b`, Actions run 42380771960
completed all five existing jobs successfully, including Windows Job Object and
injected lifecycle tests. The optional synthetic live probe is a new, separate
qualification gate. Read `LIVE_MOUNT_PROBE.md`; no previous callback-only result
counts as a real-mounted result. Run 42384272839 at
`913b9e2f980a85b7564a23bd2b825ee7dae51f3c` subsequently passed all six jobs,
including actual driver-backed synthetic mounted I/O.

## Actual process-crash/remount evidence

Run 42385143601 at `e4d5a1d9eb1b0f9b121841a7c053e8ac06aa3ab1` passed all six
jobs. In Windows job 128441114383 the supervised normal mount returned 0,
the deliberately abandoned mounted child returned timeout 1460 with termination
confirmed, and a separate recovery/remount child returned 0. The readiness
receipt was required, original/backing bytes matched, the mount disappeared,
and the same directory was mounted and read again before clean shutdown.

Recorded peak job committed bytes were 9,846,784 (normal), 9,842,688 (terminated)
and 9,547,776 (recovery), below the enforced 128 MiB cap. These are committed
process/job bytes, not total physical RAM, kernel memory or performance results.
This is not a power-loss, in-flight cancellation or Windows 11 qualification.

## Persistent catalog continuation

The new bounded manifest parser/loader, real CLI smoke tests and persisted
live-probe bindings require their matching commit's Windows CI. Portable
validation passed all 12 CTest entries (eight test executables plus four real
CLI invocations) in each of GCC/Zstandard, GCC/raw-only and Clang ASan/UBSan,
for 36 executions. The manifest tests reproduced and repaired a reserved-name
normalization gap before publication. These are correctness tests, not speed
benchmarks or proof of production safety.
No remote-control credentials, owner-PC logs or private repository content are
part of these artifacts. No commands were executed on the owner's PC.

## Bounded FLZIP CPU compatibility continuation

Baseline `a7eea03d33a4119e37023205d25cfa6c4909abd7` was retrieved as a
commit-bound artifact, verified by SHA-256, and its complete source tree matched
`c807d158a4e2a3b8dbd148b7355000e9c9cecedc`. All 12 baseline CTest entries
passed locally before implementation.

The added reader/parser, snapshot/cache, catalog/projection, and CLI tests cover
raw/fill/pattern and optional Deflate/Zstd. Negative cases include version/kind
mismatches, count/length limits, overlapping payloads, path conflicts, CRC errors
in unrequested files, exact-stream violations, unknown Zstd output size,
truncations and deterministic mutations. The CLI test runs 12 real subprocess
checks and confirms all five synthetic fixture hashes remain unchanged.

All 14 portable CTest entries passed in each of local GCC with Zstandard 1.5.7
and zlib 1.3.1, raw-only GCC, and Clang ASan/UBSan: 42 entry executions. This count
includes the CLI contract as one entry, not 12 separately counted CTest entries.
Windows/native constructor ABI, the upstream serializer oracle, zlib 1.4 static
staging and actual mounted FLZIP behavior still require the new commit's CI.
Pending checks must not be reported as passed. No owner-PC/GPU access or remote-control credential is
part of this change. See FLZIP_READONLY.md for scope and primary research.

The match-proposal tests require exact CPU fallback for malformed geometry and unavailable sequence support, independent byte decoding of accepted candidates and managed-session publication. Actual CPU/raw/sanitizer/Windows execution must be inspected on the matching commit. This is not GPU execution or a speed comparison.

The GPU-match CPU suite checks its exact prefix index against independent history scans, compares small searches with an exhaustive oracle and rejects malformed geometry, cyclic/forward links and corrupted results. A managed-session fixture feeds the validated proposals into real staged encoding and independent CPU reopen. These are CPU semantic tests. The matching HIP kernel and separate table require complete-SDK compile/link evidence on the same commit before claiming offline compilation.

The match wire suite rejects truncation, header/reserved changes, mismatched request/source/backend, malformed lanes and cross-protocol input. The real Windows memory-channel fixture drives a valid reply into managed-session publication, falls back on a malformed reply and verifies CPU publication before cancellation/retirement of a deliberately held child. Full SDK admission also checks malformed match input and a valid match request with the private bundle missing. Configuration is not an executed GPU result.

Match-controller tests cover disabled/unqualified work, overlapping requests, accepted proposals entering a native volume, missing/corrupt/late workers, a newer CPU source during retirement, repeated unavailable configurations, fresh-evidence pressure recovery, expiry/cancellation/device quarantine and invalid post-run telemetry. The fixture executes only CPU code. Existing decoder controller and policy tests remain enabled to check the shared classification and request-ID changes.

SDK runtime-binding qualification requires malformed static-PE refusal fixtures, the actual header-derived direct-function inventory and CPU fake-runtime identity/error tests. The admission child must clear its own inherited LLVM_PATH while leaving the probe parent unchanged. Inspect these receipts on the matching commit; they are not live driver, GPU or speed evidence. Existing six worker-admission exits and all decoder/match tests remain required.

The metadata qualification requires real signed OS-file byte identity, fake trust-policy failures/cleanup, malformed identity refusal, missing/wrong bytes and stable repeated handle accounting under the existing child budget. All six prior worker admission cases plus the new metadata/refusal children require nine confirmed exits. Results must match the executed commit and cannot be presented as AMD runtime execution or speed.

Installed-driver qualification requires all nine old metadata/private cases plus eight kernel-only/driver cases, with 17 confirmed child exits. Separate self-bounded metadata/decode/match diagnostics must refuse an unavailable/mismatched runtime without publication. Inspect actual module imports and numeric receipts on the matching commit. These checks contain no AMD GPU execution and cannot support a speed claim.
