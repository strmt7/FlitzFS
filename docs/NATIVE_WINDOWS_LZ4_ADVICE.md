# Native Windows LZ4 advice forwarding — experimental

This change connects independently checked compression advice to owned session
writes and the synchronous Windows SDK data bridge. It starts no worker, loads
no GPU runtime, mounts nothing and changes no stored format. Ordinary callbacks
retain their existing CPU behavior until a qualified caller supplies advice.

## Current source contract

The native GitHub release inventory was checked on 14 November 2026. The newest
WinFsp prerelease remains v2.2B5, published 13 October 2026, with the already
qualified source at 14d6e0280ae15e2550c44d6dbc476be51649727f. The stable-only
latest endpoint returns v2.1; it does not identify the newest prerelease.
No dependency or ABI is changed.

The pinned winfsp.h Write contract and actual fsop.c FspFileSystemOpWrite were
read through native repository APIs. They require append at UINT64_MAX,
constrained writes that do not extend EOF, the accepted count and complete
FileInfo on synchronous success. The new bridge preserves those requirements
and all 72 FileInfo bytes. It introduces no asynchronous SDK completion claim.

## APIs and admission

NativeFileSession::write_storage_with_lz4_result retains opaque handle ownership,
active-phase admission and data-write grants before forwarding advice to the
existing native inode hook. The original write_storage_result signature is
preserved and supplies an empty advice span.

dispatch_native_write_with_lz4 retains the original pure input, append-sentinel,
metadata-clock, output-pointer and explicit allocation-policy admission. It then
uses validate_native_lz4_extent_candidates before any metadata read. That bounded
public validator delegates to the SAME descriptor admission used by the writer:
at most 63 descriptors, 1 MiB per payload and 4 MiB aggregate. It reads descriptor
sizes only. The ordinary Windows bridge retains its signature and supplies none.

Only after pure admission does the bridge verify active write ownership and
complete FileInfo prerequisites. It forwards advice in the SAME storage mutation.
Malformed, stale, ambiguous or non-saving bytes cannot replace the CPU-merged
extent; the existing encoder independently compares recovered bytes and keeps its
CPU fallback and strict nonexpansion rules. Raw mode retains historical bytes.
Advice spans must remain valid and immutable through the serialized call.

Count and FileInfo come from the committed result. No backing/path/clock query,
heap action or context release follows publication. Unexpected captured-fact
failure zeroes output/count and fences as before. Invalid descriptor budgets
map to insufficient resources without backing I/O or poisoning healthy state.
No grant escalation, implicit provisional adoption, new capacity or I/O retry is
introduced.

## Configured qualification

Portable enabled/raw tests pass external hand-constructed LZ4 bytes through the
owned session, check exact stored representation and CPU recovery, preserve a
prior snapshot, reject unauthorized/default handles without I/O, and retain late
detached paging identity and last-Close cleanup.

The actual WinFsp user-mode dispatcher exercises raw/fast and fixed/compact
profiles. It checks accepted count and every FileInfo field, candidate selection,
CPU fallback for stale/duplicate/partial advice, append/constrained semantics,
budget/grant/closed-handle rejection before reads, and late detached writes.
Four new sealed profiles deny all backing access after the third publication
barrier to prove that returning the advised result needs no query.

A separate bounded lz4 fault group enumerates every observed write/flush under
all three existing RAM failure models and raw/fast, fixed/compact profiles.
Failure must acknowledge no bytes or FileInfo, retain owned contexts, fence
without retry, and independently recover complete old/new bytes, metadata,
reservation and security while preserving unrelated acknowledged data. A CPU
writer must then continue safely.

All existing data-bridge cases, sealed profiles, lifecycle/handle checks and
limits remain. The new cases require matching-source execution before any
qualification claim. They execute CPU semantics only; no actual GPU, mounted
owner volume, latest Windows 11 client or physical power-loss proof follows.

## Remaining product work

The asynchronous controller qualified at f08e6b8, and the separate bounded actual
GPU-to-native-file diagnostic qualified at a047952. Those facts do not establish
normal Windows GPU routing or useful end-to-end gains. The controlled helper
must still connect fresh resource/workload admission, request ownership,
candidate lifetime, immediate CPU selection and exact retirement to file I/O.
Do not repeat an unchanged hardware trial or remove verification to improve
timings. The owner alone decides when to release.

## Retained initial qualification failure

At 7153ed1, Windows native passed 146/147 CTest groups. The four advised
invariant profiles and four additional sealed profiles passed, with handle count
123 before and after. The new advised fault campaign failed because its inherited
oracle requested only 100,000 bytes while comparing a 131,072-byte expected file.

The [retained failure](evidence/windows-lz4-oracle-7153ed1.json) includes verified
artifact/log digests and all preceding data-bridge evidence. The correction
asserts exact EOF, reads the full expected length and checks the un-faulted
reference before enumerating failures. It preserves every operation/fault mode,
metadata/security/reservation check and resource limit. Production source is
unchanged by this test correction. Matching-source qualification is required;
the original failure is not hidden by a rerun.

## Qualified checkpoint at 25ea952

All 13 core jobs passed in run 43584141229. Windows native passed 147 CTest
groups, 631 complete data-bridge invariant checks, the four advised and four
additional sealed profiles, and unchanged handle counts 123 to 123. The new
advised campaign passed 432 cases: raw fixed59/compact14 and fast fixed57/
compact14 observed I/O boundaries, each under three fault models. The original
1,299 data-bridge cases and 28 original sealed cases remain separately preserved.

Full SDK qualification passed at production source 7153ed1 in run 43583139445.
The correction changes only the SDK test and documentation; Git comparison
confirms identical production, CMake and SDK inputs. No redundant SDK or owner
hardware trial was substituted for the corrected 147-group Windows execution.
[Retained evidence](evidence/windows-lz4-advice-25ea952.json) binds artifacts and
native log digests. These are CPU-only tests and modeled failures, not a new
GPU execution, Windows 11 client or physical-power qualification.
