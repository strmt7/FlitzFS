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
