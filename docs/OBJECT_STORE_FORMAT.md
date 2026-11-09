# Experimental transactional object core

This bounded C++ library is a storage foundation, not a writable mounted
filesystem. It has no CLI route, GPU dependency, authentication or encryption.
It supports native raw, zero, repeated-pattern and optional Zstandard object
records. Compression happens before transactional publication; it is not an
archive format or writable mounted volume. Existing
.flfs and .flcatalog native bytes are unchanged. Never use it for
sole-copy data. Limits are development admission bounds, not the final product
capacity or a production format commitment.

## API and ownership

include/flitzfs/object_store.hpp exposes StoreByteIo, ObjectStoreWriter and
ObjectStoreSnapshot. The writer can initialize only an empty exclusively owned
backend or create a new ordinary file with OS-enforced exclusive creation.
Custom qualified backends can now resume with explicit identity/generation
leases; public ordinary-file factories remain new-file-only. See
STORE_RESUME_AND_INTERRUPTION.md for the ownership, orphan and repair protocol. Creation never replaces
an existing path; failure may leave a partial NEW output for inspection.

Use one control thread. Do not race the writer, snapshots, caller mutation
inputs or backend. A shared_ptr keeps the backing handle alive; it does not
make arbitrary implementations exclusive or immutable. Custom backends must
enforce exclusive ownership. The file backend keeps its own handle; on POSIX
its advisory locks do not exclude a noncooperating process modifying
the same inode. Store input handles now acquire shared nonblocking locks on the
same opened POSIX descriptor; cooperating exclusive file writers are excluded. This is not hostile-directory or hostile-process isolation.

A transaction supplies 1..256 unique nonzero object IDs, puts and/or erases.
Each payload is at most 1 MiB; new payload bytes total at most 4 MiB; the final
namespace has at most 256 objects AND at most 16 MiB total decoded live bytes.
The decoded budget is explicit and independent of compression ratio/object
count. Final-inventory admission precedes codec work, permits put-before-erase
replacement at capacity and rejects aggregate overflow before writes. Inputs are privately
copied and validated before backing writes. Erasing a missing ID is an error.
A snapshot preserves its generation and old immutable record references;
updates do not replace bytes referenced by an earlier snapshot. Verified
new-volume compaction copies are now available; COMPACTED_COPY.md defines source
leases, identity rebinding and publication. There is no automatic reclaim,
ordinary-file writable reopen, filesystem names or ACL support in the object API.

The physical history is capped at 64 MiB. Full history, invalid arguments and
resource admission failures reject before writes and leave a writer usable.
Exceptions from actual writes/flushes permanently poison the writer. The
outcome of a failed commit is indeterminate: a complete new transaction may
already have persisted. Never retry that writer or bypass its live lease. Destroy it and inspect with an independent
read-only open; its generation may be newer than the last acknowledgement.
A snapshot from a poisoned writer retains the last acknowledged references,
verifies their bytes and reports degraded_root. It is not a new acknowledgement.

## Conditional persistence protocol

StoreIoContract is a caller-supplied, externally qualified assumption:
successful flush persists preceding writes, and failed-write collateral stays
within the declared power-of-two failure units (512..65,536 bytes).
Unknown contracts reject creation before output. The declaration is NOT
hardware attestation; a file API cannot discover or guarantee the real
controller/firmware/parent-filesystem power-loss failure unit. No physical
power-cut qualification has been performed.

Two 4 KiB root records occupy separate 64 KiB zones at offsets 0 and 65,536.
Immutable records start at 131,072. Initialization seals an empty manifest in
a complete 64 KiB segment, flushes, writes both generation-zero roots, then
flushes again. Later transactions:

1. Append all changed object records, a complete namespace manifest and padding.
2. Flush dependencies.
3. Publish the new root in slot generation modulo two.
4. Flush the root, then return success.

Each transaction seals its physical end to the next 64 KiB boundary. Subsequent
writes never reuse padding or any acknowledged data segment. This protects old
records from collateral damage inside an admitted failure unit; root slots
also occupy separate units. This deliberately spends space while establishing
the protocol. It is not a compression-optimized allocation policy.

Successful acknowledgement is conditional on the stated backend contract.
Software cannot compensate for a device that reports durable success while
losing acknowledged data. Post-write media decay, arbitrary controller damage,
hostile modification and loss of both roots are outside this contract.

## Canonical bytes

All integers are unsigned little-endian, all reserved bytes zero. Digests are
32-byte unkeyed BLAKE3. Their purpose is integrity and torn-record detection;
anyone able to modify bytes can recompute them. They are not authentication,
rollback protection or encryption. Record magic separates framing domains.

### Root: 4,096 bytes at slot times 65,536

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | ASCII FLTXR001 |
| 8 | 4 | Format field 1 |
| 12 | 4 | Slot 0 or 1 |
| 16 | 8 | Generation; positive values must match slot parity |
| 24 | 8 | Committed physical end, 64 KiB aligned |
| 32 | 8 | Manifest offset |
| 40 | 4 | Manifest byte count |
| 44 | 4 | Object count, at most 256 |
| 48 | 16 | Caller-provided nonzero store identity; not a secret |
| 64 | 32 | Digest of complete manifest |
| 96 | 3,968 | Zero |
| 4,064 | 32 | Digest of root bytes 0..4,063 |

A root with a digest mismatch is an unavailable/torn candidate. A digest-valid
root with unsupported magic/format, invalid fields or nonzero reserved bytes
fails explicitly. Two valid roots must share identity and have adjacent
generations, or agree on the initial generation-zero manifest. One valid root
reports degraded_root. The newest valid root is selected. If its referenced
manifest or data is corrupt, opening fails rather than silently rolling back.
An unavailable newest root may select the older root with degraded_root; this
is not an arbitrary-bit-rot recovery guarantee.

### Manifest: 64-byte header plus 64 bytes per object

Header: magic FLTXIDX1 at 0; u32 format 1 at 8; u32 count at 12; u64 generation
at 16; u64 sum of logical bytes at 24; identity at 32; sixteen zero bytes at 48.

Each entry: u64 ID at 0, u64 object generation at 8, u64 record offset at 16,
u32 logical bytes at 24, u32 complete STORED record bytes at 28, digest at 32.
Stored record size is 128 plus encoded payload bytes, at most 128 plus logical
bytes. It may differ from the decoded size.
IDs must be strictly increasing/nonzero. Object generation is positive and no
newer than the root. Each record lies before the selected manifest; referenced
records may not overlap. Validation sorts bounded offset ranges and checks
adjacent ranges in O(N log N), independently of object-ID ordering. Count,
sizes, offsets and the explicit 16 MiB decoded sum are admitted before
payload allocation. Maximum manifest size is 16,448 bytes at the current 256-object cap.

### Object record: 128-byte header followed by encoded payload

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | ASCII FLTXDAT1 |
| 8 | 4 | Format field 1 |
| 12 | 4 | Codec 0 raw, 1 Zstandard, 2 implicit zero, 3 repeated pattern |
| 16 | 8 | Object ID |
| 24 | 8 | Object generation |
| 32 | 4 | Logical bytes, at most 1 MiB |
| 36 | 4 | Stored payload bytes, at most logical bytes |
| 40 | 16 | Store identity |
| 56 | 32 | Decoded-content digest for compressed records; zero for raw |
| 88 | 8 | Zero |
| 96 | 32 | Digest of header bytes 0..95 concatenated with STORED payload |
| 128 | Variable | Encoded payload; raw empty object allowed |

The manifest repeats the stored-record digest. Raw records retain all forty
zero bytes at 56..95, so raw-mode output preserves the previous bytes and is
readable by the previous reader. New codec IDs and decoded-digest fields are
explicitly unsupported by the previous reader. There is no silent migration.

Codec 0 requires stored size equal to logical size and preserves arbitrary
bytes, including empty objects. Codec 2 requires positive logical size and no
payload, reconstructing zeros. Codec 3 stores a motif of 1..4,096 bytes shorter
than logical size, repeated with a possible partial final motif. Codec 1 stores
one complete independent Zstandard frame, with known content size equal to the
logical size and no external dictionary ID. Concatenated, skippable, unknown-size,
truncated and trailing frames are rejected. Unknown codecs fail explicitly.

Opening verifies every reachable stored record and its full decoded content
before exposing a snapshot. Each compressed read verifies both digests again;
raw reads verify the raw record digest. There is no unchecked read or shared
decoded cache. Decoders discard private output on any failure.

## Ordinary-file adapter and resource admission

CreateFileW CREATE_NEW (Windows) or openat O_CREAT|O_EXCL|O_NOFOLLOW (POSIX)
creates new regular files only. Final reparse points/symlinks are rejected.
The POSIX parent directory is pinned and fsynced with initial publication;
file fsync handles each durability barrier. Windows uses FlushFileBuffers.
Windows creation-name power-loss durability remains unqualified.

The adapter refuses unknown memory telemetry. It requires at least 640 MiB
physical AND commit headroom, covering a 512 MiB reserve plus 64 MiB cache
and 64 MiB staging allowances for the bounded native file layer and core codecs. It checks filesystem free space before creation
and before each transaction, retaining a 1 GiB reserve. These are admission
checks, not reservations against other processes or guarantees against OOM.
Only Windows and Linux telemetry are admitted. Resource admission runs before
codec staging and again before the projected writes. File size remains capped at
64 MiB; positional short reads/writes and flush errors throw. No deletion,
raw-device, volume, partition or driver operation is part of this adapter.

The file-I/O test is registered only on disposable GitHub-hosted runners and
rechecks that environment before creating a fixture. No owner-PC process is
authorized by this library or its tests. Memory-only model tests do not imply
that host OS paging would avoid disk activity.

## Digest dependency and qualification gates

The portable C implementation is unmodified BLAKE3 1.8.7 at
f3149ec5bb5449af877ba20377a11008ff499fa2 from
[BLAKE3-team/BLAKE3](https://github.com/BLAKE3-team/BLAKE3/tree/f3149ec5bb5449af877ba20377a11008ff499fa2).
Its upstream licensing files, source blob inventory and official vector
fixture are retained under third_party/blake3. CI verifies the exact Git blob
bytes before building. SIMD, NEON and TBB are disabled; there are no GPU calls.
Thirty-five official unkeyed known-answer vectors and split-input variations
check the wrapper. This is source provenance and functional evidence, not an
independent cryptographic audit.

The RAM fault test covers API/resource admission, every observed second-commit
write/flush failure, partial persistence, whole-unit damage, all 4,097 root
prefix/suffix boundaries, payload corruption, intact malformed framing and
physical-history exhaustion with sparse RAM backing. A separate hosted test
checks actual file I/O, independent reopen and exclusive no-clobber creation.
The earlier PERSISTENCE_MODEL.md enumerates additional abstract reorderings.
Neither experiment proves all fault interleavings or physical power survival.

Still required before writable filesystem exposure: file-to-extent mapping,
qualified physical persistence assumptions, process-kill and OS fault tests,
durable namespace operations, writable reopen/recovery, capacity/reclamation,
authentication/key design, filesystem semantics and extensive mounted VM tests.
Actual AMD hardware/runtime fault qualification and comparative compression/
throughput evidence remain separate unmet gates.

## Native compression policy

StoreCompressionOptions defaults to automatic CPU selection. Raw mode preserves
the previous raw-only record bytes. Automatic selection examines all input for
zero runs, uses bounded KMP prefix analysis over at most 8 KiB to propose a
motif up to 4 KiB, then verifies EVERY byte against that motif. Sampling alone
never establishes equality. When available, bounded single-threaded Zstandard
also competes for the smallest payload, except when zero/one-byte motifs are
already unbeatable. Zstandard must save at least 16 bytes; no representation
may expand the stored payload. Metadata, padding and retained history still
count toward physical storage.

Zstandard levels 1..9 are admitted; default is 6. Window log is 20, hash and
chain logs 18, workers zero; frames carry content size and checksum. Reused
contexts have custom allocators capped at 16 MiB encoder and 2 MiB decoder,
including allocation headers. The build verifies that the selected library
actually links these custom-allocation APIs. If optional support is absent,
automatic selection still provides raw/zero/pattern storage; require_zstd rejects
creation before output instead. A reader unable to decode a stored codec fails.

All compressed candidates must round-trip through the CPU recovery decoder and
match the submitted private bytes before backing writes. Compressed records
bind a separate BLAKE3 decoded-content digest into the stored-record digest.
This protects recovery from incorrect decoded output; it remains unkeyed
integrity, not authentication. Codec/resource failure before writes leaves the
writer healthy; backing I/O failure remains terminal and indeterminate.

The library allocator caps are not a whole-process memory limit. Private
transaction vectors remain bounded by the object/transaction limits, and the
ordinary-file adapter requires physical/commit headroom. Failed allocation or
unavailable telemetry rejects work; no GPU runtime or worker is initialized.

## Source and measured-comparison contract

The API review used native source bytes from
[Zstandard 1.5.7 zstd.h](https://github.com/facebook/zstd/blob/f8745da6ff1ad1e7bab384bd1f9d742439278e99/lib/zstd.h),
Git blob b8c0644a7ec6533916e06aa8392a2727cbe55573. The documented custom-memory
callbacks govern library allocations; context reset/parameters, single-pass
output bounds and exact frame/content-size checks guide the implementation.
Single-pass decompression does not rely on streaming window-limit parameters.
CI uses its recorded system/pinned library; the reviewed source is not itself
a claim that every job links that exact version.

The hosted native-store comparison uses the exact previous raw engine at
5c7d7a0a1aea82cbcf3f8d70b1a1d340a00d2f1e, current raw mode and automatic levels
1/6/9. Six synthetic corpora each contain four 1 MiB objects. Three rotating
samples per policy report write time, CPU reopen/full verification, complete
read/byte verification and physical image end including roots, manifests,
initialization and sealed padding. Active payload bytes are a separate metric.
Full raw images are compared byte-for-byte and independently reopened by both
readers using hosted-only new temporary files.

Timed storage is an immediately persistent RAM model, with no physical device
or mounted filesystem. It does not measure physical flush latency, actual disk
allocation, application workloads, sustained multi-transaction steady state or
comparative filesystem performance. Corpus selection is synthetic and not a
compression-frontier benchmark. Per-child address space is capped at 512 MiB,
CPU time 15 seconds, wall time 20 seconds; output streams and total run time are
bounded. All execution stays on disposable hosted VMs. Inspect the exact
commit's Native compressed-store RAM comparison workflow and retained JSON;
results are not inferred before execution.

The first comparison's measured CPU costs and the subsequent bounded bulk-pattern
optimization are recorded in NATIVE_STORE_MEASUREMENTS.md. The follow-up compares
against both the raw engine and the pinned pre-optimization compressed engine on
the same VM; physical image end and active payload remain separate metrics.

## Qualified resume development

STORE_RESUME_AND_INTERRUPTION.md specifies the new exclusive custom-backend
resume, aligned orphan preservation, root-pair repair and hosted forced-process
interruption gates. The ordinary-file resume adapter is internal and hosted-test
only; default file/CLI/mount behavior remains new-file-only or read-only.

## Native file namespace client

NATIVE_FILESYSTEM.md defines the separate FLTXNS01 metadata stored as object 1
and fixed 64 KiB data extents in objects 2..256. It uses the existing atomic object
commit and qualified resume; object record/root bytes are unchanged. It adds no
ordinary-file reopening factory, mount mutation route, authentication or GPU
runtime. A private writer-only staging snapshot copies last-ack references
without rereading every payload; every read through it still verifies a complete
record. The public snapshot/open APIs retain full verification. See the native
file document for the immutable-backend assumption and delayed detection of
untouched-media corruption. The 256-object admission envelope keeps the existing
record/root encoding and explicit 16 MiB decoded limit. Existing images remain
readable; older bounded readers reject inventories above their own limit.
The Zstandard reader now rejects noncanonical frame magic before
library parsing; all current writer-produced modern frames remain readable.

## Replicated current publication extension

REPLICATED_CHECKPOINTS.md defines root format field 2. Data records/manifests and
legacy root field 1 remain compatible. Generic legacy commit bytes are retained;
commit_replicated upgrades publication and later commits preserve that mode.
Native file transactions and positive compacted copies use two current roots,
two independently sealed manifest units and three barriers. New status fields
report checkpoint identity/mode/degradation. Verified same-state peer metadata
may repair a selected catalog loss; no older object set is accepted as repair.
The legacy single-publication descriptions above remain the legacy protocol.

## Native unchanged-record reuse

The native file layer can omit a payload put when exact verified bytes are
unchanged at the same inode/block. Its metadata transaction still publishes
through the normal replicated protocol. Existing immutable record identities
and wire bytes remain valid; this requires no object-format change, shared
payload ownership or new compression dependency. See NATIVE_FILESYSTEM.md.


ObjectStoreSnapshot::source_bytes reports admitted backing logical length
separately from info().physical_end, including retained uncertain tails. It
checks the selected end and physical envelope without mutating or scanning
those tails. This is not allocated-cluster accounting. The explicit Windows
read-only native projection consumes this metadata; no codec, record, root or
publication bytes change.


Native inode I/O entry points (NATIVE_INODE_IO.md) reuse the existing object
transaction/publication path without record, root, manifest, namespace or codec
changes. The historical NativeFileMutation and NativeFsCommit layouts remain
unchanged; append/constrained selection and accepted-count results are separate
in-memory APIs. No new stored format or product version is introduced.
