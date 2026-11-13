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

Adapter-identity qualification requires an actual bounded DXGI enumeration, real field/uniqueness checks, no HIP initialization and a correctly represented empty AMD list when applicable. The diagnostic's additional DXGI import is audited locally to its SDK target; the general portable CPU import policy remains unchanged. Metadata cannot establish GPU execution or speed.

## First installed-driver host refusal and diagnostic qualification

On 11 November 2026, commit 37aaed8f075383b8ae47cd9e10fe1dd668315a52
made one explicit 12,288-byte synthetic decode attempt on an RX 9070 XT using the
installed, unchanged 32.0.32015.12012 driver. The worker exited with
0x46530003 (unsupported_configuration), with exit confirmed, no timeout and no
candidate publication or accepted GPU result. Wall time was 142 ms; reported
worker time was 119 ms. Child/aggregate peak commitment was
9,662,464/11,071,488 bytes. The planned 16,492 device bytes do not prove allocation.
The exact failure stage and whether HIP initialized are unknown. No match attempt
followed. These are refusal/resource observations, not speed measurements.

That commit's ordinary CPU, raw-only, Windows-native and sanitizer jobs passed.
Its hosted crash-restart mount job 130339172334 failed: the extended writer timed
out and its exit could not be confirmed within the existing termination window.
The gate correctly refused recovery. This resembles an earlier intermittent
hosted failure; its cause remains unresolved. Other mounted lanes passed. Do not
convert this failure to a pass or transfer those mounted tests to the owner PC.

The new diagnostics require portable category/stage invariants, fake-API stage
checks, actual Windows child-exit/classifier checks and no-HIP DXGI budget
inspection. They preserve all existing seventeen admission children and refusal
probes. Qualification is pending until exact matching hosted receipts pass.

### Diagnostic qualification completed; node refusal observed

Commit 9c1637a3144b743271de9c521070d2c2765ae78d passed all 13 triggered
workflows. Windows-native CTest passed 126 groups; the real Windows transport
fixture passed 867 checks, including staged failure-category handling. The SDK
fake API probe passed 108 checks and retained all 17 confirmed admission exits.
Linux CPU/raw-only, Windows raw-only and sanitizer checks passed. Its hosted
crash-restart job 130350565766 confirmed writer termination, resumed the store
and independently verified the preserved 7,001 bytes and continued state. This
successful run does not explain the earlier intermittent unconfirmed exit.

On the owner PC, fresh no-HIP DXGI budget inspection passed in 33 ms (peak process
job commitment 1,871,872 bytes). Installed-runtime metadata passed in 167 ms.
One separately bounded diagnostic decode then returned raw 0x46530703, category
0x46530003 and stage adapter_node: 161 ms wall time, 110 ms internal elapsed,
12,288 CPU bytes verified, no candidate publication, no timeout and confirmed
child exit. Child/aggregate peak commitment was 9,662,464/11,067,392 bytes.
The actual HIP node mask remains unknown. No stream or kernel call was reached
by the worker after the failed identity check. Driver version/status remained
32.0.32015.12012/OK; no owned diagnostic process remained. These observations
are not a GPU speed result, mounted-owner test or general safety qualification.

The following batch adds hosted private-runtime import/license inventory and
fixed-choice current AMD research. Its own matching execution receipts are
required before interpreting the inventory; it ships no AMD binaries.

### Private runtime inspection and research-scope correction

Commit c8e69f9288533ce85b49243ee6db25e34d177a48 completed the hosted SDK
inventory without loading a runtime or exporting AMD binaries. Its three staged
DLLs total 16,777,120 bytes. Normal imports of rocm_kpack.dll include
msvcp140.dll, vcruntime140.dll and vcruntime140_1.dll, which are outside that
staged closure. Their deployment and provenance must be qualified before
calling a private package host-independent. The inventory observed 17,864
entries and 27 license-named files totaling 75,742 bytes; this does not qualify
redistribution of the runtime or its dependencies.

All six requested AMD pages returned native HTTP 200 with substantive text.
Review nevertheless rejected the old Radeon/Ryzen requirements page: its banner
explicitly limits coverage to releases through 7.2.1 and redirects readers to the
unified documentation. Its native successful capture remains preserved, but it
must not establish ROCm 10.1 compatibility. The current release notes identify
10.1.0 dated 5 October 2026. The observed-driver release notes map driver-store
32.0.32015.12012 to Adrenalin 26.10.2; the ROCm-specific page identifies a different
26.10.41.05 package. Those facts alone do not prove compatibility of the private
SDK with the owner's unchanged driver.

The next research batch uses the unified matrix and current license page, rejects
explicit retired-documentation banners while retaining native receipts, and
adds a fixed Microsoft adapter-topology source group. Its regression and native
capture results remain required; no new host experiment follows from this change.

## Native compression profile probe

The new advisory probe measures at most one existing profile per advance on a
sample of at most 65,536 bytes. It preserves the existing CPU verifier, optional
hint fallback and allocator caps. Its regression tests include a committed RAM
object flowing through measurement and the existing recompressor, with unchanged
source bytes/write/flush counters and independent target readback. All matching
CPU, raw-only, Windows-native and sanitizer results are pending until the new
commit executes. These are correctness checks, not speed/ratio superiority or a
completed automatic learning/scheduling system.

## Installed-runtime properties-only diagnostic

The owner explicitly requested another GPU check. Fresh DXGI budget and runtime
metadata passed; one bounded 12,288-byte decode trial again refused at adapter_node
with no candidate publication, timeout or unconfirmed exit. Wall time was 171 ms;
child/aggregate peak commitment was 9,682,944/11,083,776 bytes. The CPU reference
passed, driver 32.0.32015.12012 remained OK and no owned process remained.

The new diagnostic captures the raw fields without device selection or a
FlitzFS kernel load/launch. Portable wire, real Windows CPU transport and fake
HIP identity-only tests must pass on its exact commit. The SDK refusal matrix
retains all 17 original admission children plus the existing diagnostic cases.
Matching execution results are pending; no compute gate is relaxed.

## Single physical-node proof preparation, 11 November 2026

77931001b8a1238fd29f9e41081d2025ab657f23 passed complete-SDK run 42979952055
and core run 42979952021, including all isolated hosted mount scenarios. The
owner properties-only result confirmed raw HIP node mask zero with exact LUID;
no compute was requested. GPU_NODE_PROOF.md records the measurements, bounded
new native collector and current primary-source receipts. New proof-policy,
fake-API and hosted software-device checks must pass on the exact subsequent
commit before owner topology or compute execution. At this source checkpoint
actual FlitzFS GPU execution and speed are still unproven.

## Executed native node proof and retained decode refusal

At 96cb919, SDK run 42983895333 and CPU/native/sanitizer prerequisites passed.
The owner OS-only collector verified the exact AMD LUID, healthy single physical
node and confirmed retirement. One tiny decode then failed with device_fault /
none, retaining verified CPU data and no published GPU reply. Exact measurements
are in evidence/gpu-node-proof-96cb919.json. Subsequent changes only preserve
which existing memory predicate failed; they need their own exact CI. Current
Microsoft memory documentation will be captured with isolated Crawl4AI before
changing residency/admission rules. GPU execution and speed remain unqualified.

## Worker memory observation preparation

The b7de640 fixture correction passed core run 42988002142, including its
hosted mount matrix; the earlier 96cb919 crash-restart retirement timeout remains
unresolved and retained. The pinned 9028be3 worker then reached the final
completed_nonlocal_growth check after successful GPU launch/completion/statuses
and exact 12,288-byte CPU comparison. Evidence is in
evidence/gpu-internal-decode-9028be3.json. No candidate was accepted.

GPU_MEMORY_OBSERVATION.md defines a new explicit metadata-only diagnostic with
three process-local budget samples and execution/retirement flags. Source now
adds portable golden/malformed/cross-protocol checks, Windows CPU-channel cases
and a separate SDK bad-runtime refusal. These new configured checks require
exact executed CI before another owner diagnostic. All existing admission and
fallback behavior remains unchanged; this is not a performance pass.

## Native Windows application and controlled owner check

At f4928c2d2510cacb55da664a0338c8592194d9a0, application run43341114027/
job131609363436 passed the separate PE audit,377 model checks/three strictly
balanced ownership cycles and actual native GUI-control workflow. The same
hashed artifact then passed owner Windows build26300 checks: three179-to179
model cycles, working-set restoration, healthy provider cleanup and a clean GUI
create/save/read/optimize/discard/exit workflow. Its GUI child peaked at14209024
committed bytes. Neither host run needed forced termination. Postflight found
no remaining FlitzFS process; WinFsp remained Stopped/Manual.

These numbers are bounded qualification observations, not throughput/compression
benchmarks or complete Windows/client/driver/accessibility/power-loss coverage.
The GUI's dynamic readiness polling yields different assertion counts per run;
the complete required command and byte-comparison sequence passed in each.
The app is unmounted and initializes no FlitzFS HIP compute. Preserve prior
zero-benefit GPU evidence and all full-product gates.
[Exact artifact/source/resource/failure record](evidence/volume-app-f4928c2.json).

## Executed priority capability checkpoint

At b2cf9899417d52c2266d86fa1246b874001376a3, SDK run43432703582 passed235 CPU
assertions across12 priority scenarios. The exact historical probe reported
category1179844613 with one query; the corrected probe reported1179844611 with
two. Neither loaded HIP or executed GPU work. Every original SDK/worker gate,
including20 confirmed exits, passed; core run43432703721 passed all13 jobs.
[Source-bound evidence](evidence/priority-capability-b2cf989.json).


## Historical retirement reconstruction

The manual retirement-reference workflow is isolated from product targets and
normal tests. Read [RETIREMENT_REFERENCE.md](RETIREMENT_REFERENCE.md) for exact
old-source pins, one trial per disposable VM, phase receipts and original limits.
No owner driver/mount/GPU operation or stored-format change follows. Unknown
retirement remains terminal; a green reconstruction cannot explain the old kernel
state by itself. Publishing-source execution is still required.

At497066c all13 ordinary jobs and8 Linux source tests passed. Both historical
comparison variants failed preparation before driver installation or trial.
The correction binds actual directory identities and independently reconciles raw
phase/acknowledgement/result/CPU records. Windows/Linux source tests and actual
controlled trials require new-source qualification; D01 remains unresolved.

[Exact gate, source-preparation failure and recency review](evidence/retirement-preparation-497066c.json).


At400f74c Linux passed16 source cases and Windows passed7 record cases before
rejecting fixture bytes; no manual driver/trial was run. Canonical Git capture
with separately verified exact/CRLF checkout identity is the next18-case gate.
The source fixture failure is not a historical retirement result or GPU evidence.


At ab37c4a, all18 source cases passed on both OSes, all13 core jobs passed, and
both historical held/closed-handle trials retired under original limits with
exact7001-byte CPU recovery. Their one-shot outcome remains inconclusive for D01.
[Raw source-bound evidence](evidence/retirement-reference-ab37c4a.json).
The separate [native synchronization API probe](HIP_WARP_SYNC_PROBE.md) now needs
its nine-case CPU and exact SDK compiler qualification. Vendor kernels, runtime
routes and stored encodings remain unchanged; useful GPU gain is unproven.


The native synchronization API probe passed its9 CPU cases, actual selected header,
compiler/dependency/object checks and original SDK/20-worker gates ataf2f714
([evidence](evidence/hip-warp-offline-af2f714.json)). Exact physical-input stability
and complete automatic trigger coverage are the next narrow qualification;
no vendor kernel, runtime or GPU-output behavior changes.


At 2fdd01c, all 13 ordinary jobs and the SDK/worker gates passed, including
physical-input stability of the native synchronization probe
([evidence](evidence/hip-warp-stable-2fdd01c.json)).
The separate [native-wave candidate](HIPCOMP_NATIVE_SYNC.md) is experimental,
OFF by default and awaiting its own hosted qualification: seven composition
cases, a compiled CPU index oracle and strict offline object inspection.
No GPU object is loaded, stored codec added or runtime path enabled. D01 remains open.
