# Worker communication: executed development evidence

Date: 8 November 2026. Development only; the owner decides when to release.

Implementation: `93cdc196140e30a31b7e41b47eeb5a37d5e9ef30`.
Diagnostic refinement: `86ca377b7f5af3c091232a5ef2156243476d761f`.
Resource/publication checks: `4a2776bff4b5fc929784050b55e62985de7f2ea7`.
Validated product/test tree: `18a7159fa2764496b388ff7be73caef87bc26bfb`.
The local publication index matched each published Git tree exactly.

## Portable results

The baseline passed all 16 portable tests. The new worker-wire suite performed
705 assertions. All 17 CTest entries passed with GCC/codecs, GCC with external
codec libraries disabled, and Clang AddressSanitizer/UndefinedBehaviorSanitizer:
51 configuration-matrix executions. The full matrix was repeated after publication
with the same outcomes. Existing batch/kernel-semantics tests still perform
1,819 assertions on the CPU; neither count is a hardware-execution claim.

Wire tests cover independent six-codec reconstruction, empty payloads, full-cap
batches, private input ownership, exact framing, all truncations of a small
fixture, every one-byte reply mutation, bounded deterministic request mutations,
wrong identities/backends, failed or pending statuses, and exact CPU parity.

## Hosted Windows results

[Main CI run 42459898957](https://github.com/strmt7/FlitzFS/actions/runs/42459898957)
completed successfully with all six jobs: Linux codecs enabled/disabled,
Linux sanitizers, Windows minimal, Windows native callbacks/lifecycle, and
Windows driver-backed synthetic mounts/recovery. All four workflows at the
validated commit also completed successfully, including the existing source
snapshot, offline HIP compilation and pinned FlitzZip comparison workflows.
These are distinct results, not a substitution of portable tests for Windows. Windows worker tests launch
only their own trusted binary, exercising real mappings and process supervision.
They do not load HIP, query GPU performance or touch application data files.

The completed Windows tests check current-user protected DACLs, non-inheritable
handles, read-only request views, bounded process settings, byte parity, malformed
messages, repeated publication rejection, no result from failed/timed-out helpers,
and explicitly requested recovery using a fresh channel.

A post-exit diagnostic now observes whether a bounded response was published.
The timeout/failure fixtures must have published before their output is rejected;
an early startup failure cannot satisfy those assertions. The diagnostic never
returns their payload, never substitutes for content validation and is not proof
of hardware execution. Unconfirmed process exit still prevents shared-state reads.

## Failure investigated rather than hidden

The first hosted run failed its process-wide handle assertion, despite passing
the message/failure checks. Diagnostic run 42459531110 recorded 94 handles after
transport-only warmup, 95 after the first complete transaction, and 95 throughout
all subsequent success, rejection, timeout and recovery cases. That observation
alone does not identify the extra handle's owner and is not called a leak fix.

The test now warms the full exchange-and-validation path, retains the unchanged
final no-growth assertion, records bounded per-case counts, and repeats four
additional complete transactions. More importantly, after EVERY confirmed
exchange it tries opening both exact mapping names: both must be absent. This
directly detects a retained mapping handle or view instead of relying exclusively
on process-wide initialization-sensitive accounting. No numeric leak allowance,
resource-limit reduction or disabled test was introduced.

The nonsecret channel identifier is returned for correlation only; it is already
passed to the trusted child. No authentication claim is attached to it. The
random namespace and DACL are not defenses against hostile same-user/admin code.

## Scope and next integration gates

Normal CLI and mounted reads still use CPU decoding and do not launch workers.
Existing mounted recovery tests are regression evidence for that CPU path, not
GPU filesystem qualification. The kernel remains offline-compiled only.

The next runtime implementation must use an explicitly selected trusted AMD
runtime/module, obtain the allocating CHILD's own adapter-matched memory budget,
keep its device buffers and work bounded, preserve independent CPU comparison,
and return results only after the timing/identity/status gates pass. Driver
cleanup can block: process termination cannot promise kernel or GPU recovery.
No owner-PC or AMD GPU commands were executed. No release, tag, persistent
service, writable filesystem or encryption feature was created by this work.

Research and primary documentation are recorded in WORKER_CHANNEL.md. Windows
mapping lifetime rules additionally require closing object handles AND unmapping
views: https://learn.microsoft.com/en-us/windows/win32/memory/closing-a-file-mapping-object
