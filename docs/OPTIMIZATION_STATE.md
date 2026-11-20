# Persistent advisory optimization state (experimental)

11 November 2026. L01 foundation, not a learner, autonomous scheduler or native
volume-provider integration. The implementation requires exact-source
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

### Payload scrub checkpoints

Capability 6 uses `FLTOP006` in the existing two-slot area. It keeps the funded
header, benchmark table and group scan wire layouts. The profile stride becomes
56 bytes, with four zero bytes after confidence in each row. This leaves room
for an ordered 48-byte checkpoint at offset 1856. It contains the 16-byte
image identity followed by four little-endian 64-bit fields for image
generation, verified-through key, pass-end key and pass-start UTC in FILETIME
ticks. The verified-through key must precede the pass end. The reserved bytes
before the benchmark table are zero. An absent group scan is represented by
zeros. A present group scan retains its original feature encoding and full
capacity, including projections.

The reader supports capabilities 1 through 7. Earlier readers can continue
recovering file data independently when they decline version 6 advice.
Only checked progress can advance the sequence without consuming new
committed input. A mixed old and new pair must keep identical compression
profiles, benchmark history and group state if the settlement advances by
less than the usual publication charge. Normal advice updates preserve the
latest scrub checkpoint.
[Mirror scrub and restart](MIRRORED_PAYLOADS.md#persistent-scan-progress)

### Indexed cold compression coverage

Capability 7 uses `FLTOP007` within the same mirrored reservation. It uses
capability 6's 56-byte profile stride. The scrub field is zero when absent.
The existing feature, projection and benchmark tables keep their capacity
and layout. The 56-byte cold-coverage record starts at offset 1904.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 1904 | 16 | Selected image identity |
| 1920 | 8 | Selector epoch |
| 1928 | 8 | Minimum checkpoint from the checked selector |
| 1936 | 8 | Highest covered source generation |
| 1944 | 4 | Cold encoding policy revision |
| 1948 | 4 | Runtime Zstandard version |
| 1952 | 4 | Foreground compression level |
| 1956 | 4 | Reserved zero |

The epoch is at least 2. Coverage is positive and precedes the minimum
checkpoint. After verified indexed compaction publishes a smaller image,
the session remembers coverage from its initial copy. A trial that failed
cuts coverage below the affected generation. Replayed foreground records
remain eligible at the next compaction. The next funded discovery, scrub
or controlled policy publication can carry the coverage in its existing
slot writes. The selector handoff and ordinary file operations add no
advice writes. A cold coverage change alone cannot advance the advice
sequence.

After reopening, a matching indexed selector and encoder policy let the
session skip repeated compression trials for covered records. Compaction
still verifies the bytes. An obsolete, damaged or unrecognized checkpoint
can be omitted and its trials repeated. Earlier builds continue to read
the selected user data when they encounter capability 7 advice. User
records and their independent CPU decoders use the existing encodings.
The checksums detect damage within the declared storage model.
[Restart and interrupted publication evidence](evidence/indexed-cold-coverage-2026-11-20.json)

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

## Windows helper consumer

The [new helper integration](ADVISORY_HELPER.md) uses the existing identical-state
check and failed-state rebuild through its real private memory provider. It
retains every admitted mapping and keeps saves/reads independent. Its new
qualification remains pending. This advances the process-lifetime backend;
physical mappings, lifetime write credit and autonomous learning stay unfinished.

## Input provenance for future rewrite credit

The optional [native workload capability](WORKLOAD_ACCOUNTING.md) records
accepted write input inside its actual namespace/data transaction. Advice does
not supply or reset this value, and normal legacy file paths do not enable it.
Its new publishing-source qualification is required. This establishes only
provenance: persisted spent-work/debt, safe lost-advice baselines and a bounded
authorized consumer must be implemented before it can govern publications.

## Controlled funded records

[OPTIMIZATION_BUDGET.md](OPTIMIZATION_BUDGET.md) defines optional capability2 and
private journal operations reached only through managed funded control. Ordinary
legacy bytes/default APIs remain. Healthy resume retains settlement; degraded
resume withholds policy and requires new input before charged peer-first repair.
Unavailable state forfeits historical entitlement before new-area admission.
The new-source suite must qualify this protocol; it does not provide a physical
mapping, authenticated authority or autonomous learner.

## Indexed discovery checkpoints

Funded discovery checkpoints preserve the profile and benchmark fields,
budget marker and settled-input counter of capability 2. Capability 3 uses
`FLTOP003` and allows 63 representatives. Capability 4 uses `FLTOP004` and
can use the remaining slot space for up to 373 representatives. Capability 5
uses FLTOP005 for scans that collect masked XOR signatures. A publication
without XOR sampling still uses capability 3 or 4 according to its row count.
Each of the two 64 KiB slots has a digest covering bytes 0–65503.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 2624 | 16 | Selected image identity |
| 2640 | 8 | Observed durable namespace sequence |
| 2648, 2656 | 8 each | Last visited inode and extent index |
| 2664 | 4 | 0 for a continuing pass, 1 for complete, 2 for revisit after the pass |
| 2668 | 4 | Feature schema 1 in capabilities 3 and 4, schema 2 in capability 5 |
| 2672 | 4 | Number of retained representatives within the selected capability |
| 2676 | 4 | Number of masked-signature rows in capability 5, zero in earlier versions |
| 2680 | 8 | Reserved zero |
| 2688 | 168 per representative | Sparse feature records in strict data-object order |
| After feature records | 40 per masked-signature row | 4-byte representative index, 4 zero bytes, four 8-byte signatures |

Each representative stores inode, extent index, data-object ID and physical
revision in four 8-byte fields. Its full 32-byte digest follows at offset 32,
then twelve 8-byte band values at offset 64 and an exact-base bit at offset 160
in an 8-byte field. Masked-signature rows use zero-based representative
indices in strictly increasing order. Up to 302 representatives can each
carry signatures in one slot. At higher representative counts the signature
tail uses the remaining space, with priority for reusable anchors. The
unused bytes before the slot digest are zero. Readers validate counts, flags,
schema, coordinates, object domain, ordering and padding before exposing
the checkpoint. The loader accepts capabilities 1–5 and keeps the original
63-row limit for capability 3. Slot sequence and budget rules apply during
a transition. The scanner revisits older checkpoints to populate signatures
under its ordinary work allowance. Earlier readers can rebuild advice while
reading user data independently.

The indexed controller checkpoints at settled window boundaries during an
admitted opportunity. Each publication costs 128 KiB and consumes 4 MiB of
durably accounted write input. Accepted input renews this budget across a
long-running session. Explicit profile publications and repairs use their
caller-provided allowance. Optional advice and its backing
remain outside file-data recovery.
[Restart and transition validation](evidence/indexed-representative-retention-2026-11-19.json)

### Discovery after committed edits

The indexed writer records changed extent coordinates after a successful
commit. Repeated writes to the same extent retain one entry. The history
holds as many distinct coordinates as one maximum native data transaction.
Eviction marks the point before which discovery needs an ordinary scan.
Lifetime transitions and edits to grouped extents also request a sweep
because they can change the surviving base representative.

An indexed scan with a covered change history tries those coordinates first.
It uses the same similarity features, catalog lookups, CPU encoding and
admitted AMD advice. A changed extent that stays ungrouped triggers the
ordinary scan to consider other representatives. The previous recoverable
checkpoint stays in place while the in-memory update pass is active. A
restart recovers from that checkpoint and the current volume generation.

In the 1,034-file fixture, two writes to one related extent required two
visits to attach it to a distant group. An unmatched write and an overflowed
history both exercised the wider scan. The independent CPU reader verified
every file after those passes.
[Change discovery evidence](evidence/indexed-change-discovery-2026-11-20.json)

Published change tracking in Windows and Git informed the choice to consult
recorded edits before traversing unchanged data.
[Windows change journal](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/fsutil-usn) ·
[Git 2.55 change monitoring](https://git-scm.com/docs/git-fsmonitor--daemon/2.55.0.html)

## Paged coherence catalog

A managed indexed session can bind a read-only snapshot of a separately
reserved catalog. The paged store identity binds the volume. A typed object
at key 1 binds the selected image and feature schema. Its 32-byte record has
`FLTZCOR1`, `FLTZCOR2` or `FLTZCOR3` at offset 0, the 16-byte image identity
at offset 8, and the matching schema at byte 24. Bytes 25–31 are reserved
zero. Each schema admits the preceding posting types. A newly admitted
posting and any required root promotion share one funded copy-on-write
transaction. The catalog is rebuildable advice. The file-data image uses its
own format capability when opened independently.

The first 16 bytes of each posting contain the signature at offset 0, the
band ordinal at byte 8, and seven reserved zero bytes.

| Signature | Bands | Decoded bytes | Body after the header |
|---|---:|---:|---|
| `FLTZCOE1` | 0–11 | 184 | Original 168-byte representative |
| `FLTZCOE2` | 12–15 | 96 | Four 8-byte extent binding fields, 32-byte content digest, 8-byte role, 8-byte masked signature |
| `FLTZCOE3` | 16–19 | 96 | Same extent binding, digest and role, followed by a modular signature for 1, 2, 4 or 8 byte words |

Each compact row stores its own signature. Its decoded representative
provides the extent binding and role with an empty legacy band array. The
high bit of each object key identifies a posting. Its other bits come from
the first eight BLAKE3 digest bytes over the one-byte band and eight-byte
little-endian fingerprint, interpreted as a little-endian word. Lookup
checks the typed row, its key, and the full requested band and fingerprint.
A collision can replace an advisory nomination. Publication requires its own
validation of the selected live extents and their complete encoded size.

The writer batches postings under an explicit maximum write-byte allowance.
It retains the latest observed physical revision and uses object ID to break
ties. Exact replays and older observations consume no write allowance. The
allowance includes appended objects, index pages, padding and both root writes.
The read path checks one paged lookup path without loading the whole catalog.
[Paginated object storage](PAGED_OBJECT_STORE.md#memory-and-capacity)

After ordinary window candidates are exhausted, the maintenance controller
can look up the primary band, up to four masked signatures in schema 2 or
later, and up to four modular signatures in schema 3. The controller advances
one point lookup per admitted step for ungrouped samples and skips missing
signatures. A result goes through the ordinary live
extent and CPU publication checks. An unreadable catalog is retired and the
ordinary window can continue. A fresh catalog can wake a settled controller.
Selecting another image retires the earlier catalog owner.

A managed session can take ownership of a provisioned writer. On settled
scan windows it offers a rotating batch of retained features using the same
band and signature rules as lookup. The primary and masked postings get
space first. Measured modular signatures can fill the remaining posting
slots and use the same input-funded write grant. If that plan exceeds
capacity before writing or settling the charge, the controller can retry
the earlier key families in the same reservation. It charges the full
published transaction, including root promotion, and refreshes the
snapshot for later windows. The modular signatures live in paged postings.
[Projected catalog validation](evidence/projected-coherence-catalog-2026-11-19.json)
[Modular catalog validation](evidence/modular-coherence-catalog-2026-11-19.json)

The owned indexed Windows memory volume now supplies a private reservation to
that scanner. The first useful funded batch creates the catalog in one paged
transaction with its typed root. A healthy writer lease survives a joined
remount of the same image. A failed or superseded catalog releases its
reservation after the last owner drops it.
[Windows backing](WINDOWS_MEMORY_VOLUME_FACTORY.md#automatic-coherence-catalog)

### Compaction at the append limit

A physical-space refusal schedules a bounded compaction. The planner validates
the typed root and walks the source catalog in key order. It checks each
posting's inode, extent, object and physical revision against one pinned
native snapshot. The snapshot stays valid across unrelated writes. Only
matched postings enter the packed copy, along with the typed root. A posting
can become stale after planning, and the lookup consumer still checks the
current file before using it.

The planner measures the complete target footprint before writing. It
proceeds only when the replacement is smaller and can spend the necessary
credit. The old catalog remains readable during all phases. The rebuilt store
keeps the same volume and image identity and advances its checkpoint
generation. A completed independent verification allows the owner to select
the replacement and release the old writer. Sampling and reclamation share
the same idle admission and alternate when both have pending work. A refused
or failed copy waits for new committed input proportional to the old
catalog's footprint before another attempt.

The June 2026 RocksDB compaction analysis describes using observed space and
read costs to limit write amplification. FlitzFS measures its own packed
footprint and uses its native write ledger for the decision.
[RocksDB compaction analysis](https://rocksdb.org/blog/2026/06/20/fifo-kv-ratio-compaction.html)

[Compaction validation](evidence/windows-catalog-compaction-2026-11-19.json) ·
[Staged rebuild](STAGED_PAGED_REBUILD.md#filtered-advisory-compaction)
[Write funding](OPTIMIZATION_BUDGET.md#funded-catalog-updates) ·
[Host validation](evidence/funded-paged-coherence-2026-11-19.json)
