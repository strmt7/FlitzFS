# Persistent advisory optimization state (experimental)

11 November 2026. L01 foundation, not a learner, autonomous scheduler or native
volume-provider integration. The implementation requires exact-source hosted
qualification before results are claimed. Normal file operations do not call it.

## Ownership and data independence

The helper/controller may persist workload summaries, confidence estimates and
a short benchmark history in a separately reserved custom backend. The provider
must qualify its nonaliasing with user images, volume heads and other areas,
serialize global ownership and account for the complete reservation.
The existing per-instance StoreByteIo writer lease is enforced; it cannot
authenticate a provider or detect two facades for the same physical bytes.

No ordinary-file factory, path, mount, driver, worker, GPU launch, service,
timer, deletion, truncation or reset API exists. Creation requires a NEW empty
backend. Resumption requires the exact independently observed volume/sequence
and actual retirement of the previous owner. A poisoned writer retains its
lease and cannot publish or expose its cached policy.

User-image readers never consult this area. An absent, damaged, unsupported,
foreign or unreadable area supplies no optimization policy. It does not supply
namespace authority or a codec dependency. Decode-critical dictionaries/models
must remain immutable, integrity-protected referenced data with independent
retention/redundancy rules; they MUST NOT be stored here.

Both copies lost means rebuilding advisory information in a NEW qualified area;
there is no automatic overwrite of damaged or unsupported bytes. Durable
provider reservation/mapping and controlled helper integration remain required.
This component alone does not implement that integration.

## Fixed accounted encoding

Exactly 131072 bytes, in two disjoint 65536-byte slots. The qualified backend
must report successful-flush persistence and a power-of-two write-failure unit
from 512 through 65536 bytes dividing the slot. This is the existing core
contract, not discovered hardware geometry or physical power-cut evidence.

All integers are little endian. Each slot contains:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | FLTOP001 |
| 8 | 4 | Internal encoding capability 1 |
| 12 | 4 | Physical slot index |
| 16 | 8 | Positive publication sequence |
| 24 | 16 | Stable external volume identity |
| 40 | 4 | Advisory policy/table capability 1 |
| 44 | 4 | Profile count, at most 32 |
| 48 | 4 | Benchmark count, at most 16 |
| 52 | 12 | Reserved zero |
| 64 | 2048 | 32 fixed 64-byte profile records |
| 2112 | 512 | 16 fixed 32-byte benchmark records |
| 2624 | 62880 | Reserved zero |
| 65504 | 32 | Existing unkeyed BLAKE3 integrity over preceding bytes |

Unused records and reserved fields must be zero. Profile records contain a
nonzero workload key at 0; strategy at 8; observation count at 12; aggregate logical,
stored, CPU-nanosecond and GPU-nanosecond counters at 16/24/32/40; confidence in
millionths at 48; and 12 reserved zero bytes at 52. Profiles are strictly ordered
by (workload key,strategy), with no duplicate tuple. Observation count is 1..1000000,
logical bytes are positive and at most observations*65536, stored bytes cannot
exceed logical bytes, and confidence is 0..1000000.

Benchmark records contain workload key at 0, strategy at 8, logical bytes at 12,
stored bytes at 16, backend at 20 and elapsed nanoseconds at 24. Logical size is
1..65536; stored size may be zero but cannot exceed input; elapsed time is
positive. Backend1 denotes CPU and 2 GPU. The history is chronological and bounded;
a caller evicts old entries before publication.

Strategy identifiers1..4 reserve raw, fast CPU, dense CPU and match-guided
candidate policies. These fields do not implement those policies or attest to
hardware execution, measurement quality or statistical calibration. The future
controller must establish trustworthy observations and fresh resource/benefit
admission; persisted confidence alone authorizes nothing. No paths, file bytes,
external LLM or host-specific installation identity are required.

Integrity is not authentication or anti-replay protection. Intact unsupported
capabilities, wrong volume, malformed tables and contradictory copies return no
policy even if another slot contains an older interpretable state. A bad-integrity
peer can be ignored with explicit degradation. Equal sequences must have exactly
equal state; different valid sequences must be adjacent, selecting the higher.

## Publication, recovery and work bounds

Creation establishes the complete zeroed extent, publishes an empty sequence 1
record in the first slot and flushes, then publishes its peer and flushes.
An update validates and prepares bounded records, checks the latest exact
current state and resource admission, writes/flushed one new complete copy,
then writes/flushed the other, and independently reads both before success.
Sequence advances once. Identical current advice is rechecked and performs no
writes, sequence advance or allowance consumption. Snapshot getters return owned
bounded copies, so later publication does not invalidate a retained snapshot.
The old acknowledged copy remains in a disjoint failure
unit until a complete successor has become durable.

An interrupted update may expose the old complete policy or the complete new
one. User bytes are independent in either case. After acknowledged publication,
loss of either copy must retain that same acknowledged policy. Resume fences
and rechecks the observed state. A degraded repair writes the missing/stale peer
FIRST, preserving the surviving current copy, then refreshes the other copy.

Any metadata/readback or post-admission publication failure terminally fences
the writer. Invalid arguments, exhausted allowance and initial resource pressure
perform no writes; initial resource rejection leaves an existing writer healthy.
No failure causes an automatic retry, scan, reset or background process.

A caller must explicitly grant 1..4 publications per writer lease, including
creation or degraded repair. Every pair writes 128KiB. Initial zeroing adds 128KiB,
so a creation lease can write at most 640KiB. The reservation never grows.
This is a per-lease bound, NOT a lifetime wear governor: a future controlled helper
must tie new allowances to independently accounted workload/idle opportunities,
resource admission and a persistent rewrite budget. No learner is enabled yet.

The full 128KiB reservation and write/flush costs must be included in compression
and throughput comparisons. Small-file payload savings cannot hide this overhead.

## Configured deterministic RAM tests

The new optimization_state_crash_model CTest group uses bounded synthetic RAM
backends only. It covers each creation/update/repair write and flush boundary,
before-write failure, persisted half writes, failure-unit damage, failed flushes
with partial/full persistence and discarded unflushed state. Repair is checked
with either peer missing. Failed writers retain leases, retries do no I/O, and
independent recovery must continue publishing under a new explicit owner.

Other cases cover limits, pressure before mutation, exhausted allowances,
competing/moved/poisoned owners, unsupported capability, foreign volume, invalid
counts/order/confidence/backend, nonzero padding, equal-sequence disagreement,
nonadjacent sequences, exact extent and failed reads. A separate transactional
user image remains independently readable when both advisory records are lost.

These are configured tests until the publishing-commit results are inspected.
They do not establish physical-media durability, hostile-provider isolation,
Windows helper integration, learning quality, background interference or GPU gain.

## Source basis and remaining L01 work

The design reuses the current core's qualified failure-unit contract, BLAKE3
record implementation and two-copy publication ordering; it does not introduce
a new cryptographic primitive or guess device persistence. Inspected source
b3e90480cdfdd1151f08ab8c0969b6b9cb38122e includes object_store.hpp,
object_store.cpp, volume_head.hpp and volume_head.cpp. Actual source-bound
semantic query 1 of artifact 13864803939 routed the transaction sources; relevant
candidate byte hashes were verified against current source. The indexed inputs
are unchanged from 8122bf5 because b3e9048 changes only excluded tests/docs.

L01 remains in progress. Required next work includes provider-reserved-area
binding/nonaliasing qualification, controlled helper/session transactions,
persistent workload-derived write credit, safe new-area rebuild/mapping,
interrupted native compaction and held-out learning consumers. No production
filesystem capability or full L01 exit is claimed by this foundation.

## Native volume/session control integration

The new NativeFileSession control methods bind a provider-reserved area to the
same serialized volume owner. create_optimization_area requires a new area;
resume_optimization_area requires its exact independently observed sequence.
The provider must already reserve/account the 128-KiB region, qualify nonaliasing
and authorize the helper. No normal file callback discovers or opens it.

Known head, active-image and candidate aliases reject before area I/O. Binding
defers while staged recompression is active. There is one admitted binding
attempt per session, including failures. The attempted backing remains retained
and is excluded from later compaction/recompression targets. This does not
attest to global provider mappings or different facades for the same storage.

publish_optimization_state uses the existing validated transaction writer.
Identical advice still performs no writes. Invalid arguments and exhausted
allowances preserve a healthy advisory owner. Storage/readback failure fences
ONLY that owner. The file writer, grants, live handles and volume head are not
fenced or changed by advisory failure. Unavailable advice exposes no cached
policy, including after an allocation failure during resume.

The scalar optimization_info getter performs no allocation or backing I/O.
Its reserved_bytes accounts the provider-declared reservation; it is not proof
of physical allocation. session_ready distinguishes a fenced data owner from
advisory diagnostics. optimization_snapshot returns an owned cached copy.
Neither getter authorizes scheduling, uses a GPU or changes user-file reads.

Image handoff preserves the advisory owner and stable volume identity.
Session destruction retires both data/advisory writers before the head owner.
A new controller explicitly resumes the area only after actual prior-controller
and context retirement. Persisted workload-derived write credit, rebuild mapping,
global provider qualification, helper IPC authorization and learning remain
required; per-lease publication limits alone are not a lifetime rewrite budget.

The expanded deterministic tests retain every old crash case and add managed
binding/resume, retained handle/move/handoff, source preservation, target aliases,
staging deferral and backend read/write counters. Pressure, torn creation, both
lost records, foreign/unsupported state, stale sequence, failed read, injected
allocation refusal, failed publication and failed final readback must leave
user-data I/O healthy. A failed empty area is made otherwise eligible as a target
so the exclusion regression cannot pass merely due to a nonempty backend.

These new integration cases require their own publishing-commit qualification.
The prior journal at c30d01c passed all 13 core hosted jobs, including 278 model
assertions and raw/codec/Windows/native/sanitizer coverage. Source-bound evidence
is recorded separately. These results do not establish full L01 completion.


The single direct binding attempt is the current experimental interface, not a
requirement to restart a mounted volume when advice is lost. Automatic in-session
replacement needs a separately qualified new-area mapping/publication transition
that retires only the old advisory owner and preserves live data handles. That
transition remains L01 work; resetting failed bytes or restarting user data is
not its substitute.

## Separate in-session failed-area rebuild

[ADVISORY_REBUILD.md](ADVISORY_REBUILD.md) defines the new controlled exception to
restarting after unavailable advice. Original create/resume calls retain their
one-binding-attempt behavior. Only the separate rebuild API may install NEW empty
advice after an unavailable/poisoned owner, retaining old bytes/leases and all
live data handles. Healthy/exhausted advice cannot be rebuilt to reset allowances.
Four admitted areas cap total declared reservations at 512 KiB per session. This is
not persistent write credit, area recycling, a durable provider mapping or an
automatic learner. Publishing-source execution remains pending.
