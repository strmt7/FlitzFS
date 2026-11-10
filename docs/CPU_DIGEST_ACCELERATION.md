# Runtime CPU integrity acceleration (experimental)

This changes how the existing 32-byte unkeyed BLAKE3 record digest executes.
It changes no stored byte, checksum meaning, codec, namespace, acknowledgement,
durability barrier or API result layout. It is CPU work toward sustained native
filesystem throughput and independent GPU fallback, not GPU execution or a
claim of filesystem superiority.

## Current primary source and provenance

On 10 November 2026 the official native release/tag APIs confirmed BLAKE3 1.8.7
as current, published 20 August 2026, at
f3149ec5bb5449af877ba20377a11008ff499fa2. The existing version is retained.
Eight additional official implementation files join the five existing C files,
three licenses and published vector data. All seventeen files are byte-bound
to immutable upstream Git blob IDs by verify_blake3_sources.py. Upstream source
is copied unmodified, not patched or reimplemented.

- [Current release](https://github.com/BLAKE3-team/BLAKE3/releases/tag/1.8.7)
- [Official C build rules](https://github.com/BLAKE3-team/BLAKE3/blob/f3149ec5bb5449af877ba20377a11008ff499fa2/c/CMakeLists.txt)
- [C implementation guidance](https://github.com/BLAKE3-team/BLAKE3/blob/f3149ec5bb5449af877ba20377a11008ff499fa2/c/README.md)
- [Actual feature dispatcher](https://github.com/BLAKE3-team/BLAKE3/blob/f3149ec5bb5449af877ba20377a11008ff499fa2/c/blake3_dispatch.c)
- [Published vectors and input recipe](https://github.com/BLAKE3-team/BLAKE3/blob/f3149ec5bb5449af877ba20377a11008ff499fa2/test_vectors/test_vectors.json)

The prior build explicitly disabled SSE2, SSE4.1, AVX2 and AVX-512. The new
FLITZFS_CPU_SIMD option defaults ON. Qualified Windows MSVC x64 builds use the
four official MASM implementations and the existing baseline C dispatcher.
No global /arch, -march=native or forced runtime CPU mask is added. Missing
qualified assembler/compiler support selects the retained portable backend;
FLITZFS_CPU_SIMD=OFF explicitly selects it as well. This is independent of
FLITZFS_PORTABLE_CPU, which controls a self-contained static executable.

Existing Linux algorithm/sanitizer analysis uses the official C intrinsics,
with exact per-file SSE2/SSE4.1/AVX2/AVX-512 flags from upstream. It does not add
a Linux product or installer. Unqualified architectures/toolchains remain
portable. Windows GNU/ARM product expansion is not introduced.

## Dispatch, ownership and fallback

The unmodified upstream dispatcher checks CPU features, OSXSAVE and XGETBV
state before AVX paths. It retains scalar C on every build. Feature caching uses
upstream atomics/Windows Interlocked operations; hash state remains local to
each call. No worker, device search, module load, allocation, file I/O or helper
launch is added to record_digest. oneTBB and NEON remain disabled. One digest
uses one calling thread; existing concurrency belongs to the filesystem caller.

CPU detection is upstream code, not a guessed host model or an installed-app
path. A worker/GPU failure still has the same independent CPU digest/read path.
This change does not implement live HIP coordination or qualify GPU recovery.
BLAKE3 here is integrity detection, not authentication, encryption or lost-byte
reconstruction.

## Independent byte and boundary checks

A new bounded test executable links:

1. The actual production record_digest/default dispatcher.
2. A separately renamed scalar-only library from the same verified C files.
3. A separately renamed TEST-ONLY dispatcher with the upstream testing hook.

All public/internal C exports are prefixed in the two test libraries. The
test override cannot alter the production cache and is never linked into the
filesystem, helper or CLI. Requested test profiles are intersected with ACTUAL
detected host features; no unsupported instruction is forced. Scalar-only
builds report their true degree rather than pretending to exercise SIMD.

The test checks the published 35 known answers, empty/unaligned inputs, block/
chunk/tree boundaries through 1 MiB+1, noncontiguous two-span concatenation,
incremental update boundaries, guarded input/output pages and concurrent first
production use. It exercises five bounded feature ceilings, including scalar.
Guard pages detect reads/writes outside valid boundaries even in assembly.
Linux sanitizer instrumentation covers the C intrinsic implementations. These
checks are not a formal proof or a substitute for future CPU/client matrices.

The existing 45-second test ceiling, all old cases and all library/store/
process budgets remain. No existing fault point is removed or checksum
verification skipped.

## Controlled hosted measurements

The digest-acceleration workflow builds identical current source twice with
the SAME pinned codec/compiler settings: automatic runtime SIMD and explicit
scalar fallback. Windows-only benchmark children require hosted admission and
a private kill-on-close job: one process, 512 MiB, 20% CPU and 15 seconds CPU.
The controller enforces 20 seconds wall per child, 180 seconds overall,
6 MiB stdout/16 KiB stderr, bounded pipe readers and confirmed exit before the
next child. Native image bytes stay in RAM; reports/build outputs are confined
to disposable VM storage. Nothing runs on the owner's PC.

Five alternating microbenchmark samples compare production and scalar-oracle
hashing at 4 KiB, 64 KiB and 1 MiB, with exact digest checks. Separately, the
same-source object-store executable checks twelve COMPLETE encoded image pairs
(six synthetic corpora, raw/automatic), then measures three alternating samples
per policy for write, reopen/verify and read/verify of 4 MiB objects. Stored
image bytes and payload totals must be identical. This isolates the CPU digest
policy without advancing any historical comparator/reference commit.

Keep all raw samples, unfavorable/noisy observations, host/control metadata,
source commit and complete-image hashes. A complete report requires every
planned row; partial failure evidence is retained rather than promoted to a
success. Negative report tests reject omitted, duplicate, relabelled, unsafe or
nonfinite results. Process creation is never retried to conceal a resource
failure.

These are CPU microbenchmarks and RAM-object transactions under a CPU quota,
not physical storage or mounted-filesystem benchmarks. Quota scheduling, cache,
host noise and code path sizes affect elapsed times. Do not infer free-running
throughput, compression-ratio improvement, GPU benefit or conventional-FS
superiority from them. Representative DiskSpd/common-filesystem comparisons
remain a later product gate.

## Qualification boundary

Configured checks need inspected exact-commit executions on Windows, raw/codec,
sanitizer and portable/static lanes, with same-source measurement artifacts and
all legacy interruption/mount regressions retained. The earlier 45-second
sanitizer timing miss in NATIVE_SECURITY.md is historical evidence; this change
must not be described as its cause or cure without a controlled result.

Windows writable allocation/security/FileInfo/helper/client qualification,
scalable capacity/indexing/reclaim, NS04 real writer termination/mounted ACLs,
live AMD benefit, redundant payload repair and low-priority automatic learning
remain open.
