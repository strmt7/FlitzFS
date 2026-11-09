# Experimental transactional object core

This bounded C++ library is a storage foundation, not a writable mounted
filesystem. It has no CLI route, GPU dependency, authentication or encryption.
It stores raw objects only; it provides no compression improvement. Existing
.flfs, .flcatalog and FLZIP paths and bytes are unchanged. Never use it for
sole-copy data. Limits are development admission bounds, not the final product
capacity or a production format commitment.

## API and ownership

include/flitzfs/object_store.hpp exposes StoreByteIo, ObjectStoreWriter and
ObjectStoreSnapshot. The writer can initialize only an empty exclusively owned
backend or create a new ordinary file with OS-enforced exclusive creation.
There is no API to reopen an existing image writable. Creation never replaces
an existing path; failure may leave a partial NEW output for inspection.

Use one control thread. Do not race the writer, snapshots, caller mutation
inputs or backend. A shared_ptr keeps the backing handle alive; it does not
make arbitrary implementations exclusive or immutable. Custom backends must
enforce exclusive ownership. The file backend keeps its own handle; on POSIX
its process-level exclusivity does not exclude an unrelated process modifying
the same inode. This is not hostile-directory or hostile-process isolation.

A transaction supplies 1..16 unique nonzero object IDs, puts and/or erases.
Each payload is at most 1 MiB; new payload bytes total at most 4 MiB; the final
namespace has at most 16 objects (16 MiB logical maximum). Inputs are privately
copied and validated before backing writes. Erasing a missing ID is an error.
A snapshot preserves its generation and old immutable record references;
updates do not replace bytes referenced by an earlier snapshot. No reclaim,
compaction, overwrite-existing reopening, filesystem names or ACLs exist yet.

The physical history is capped at 64 MiB. Full history, invalid arguments and
resource admission failures reject before writes and leave a writer usable.
Exceptions from actual writes/flushes permanently poison the writer. The
outcome of a failed commit is indeterminate: a complete new transaction may
already have persisted. Never retry that writer. Inspect with an independent
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
| 44 | 4 | Object count, at most 16 |
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
u32 logical bytes at 24, u32 complete record bytes at 28, digest at 32.
IDs must be strictly increasing/nonzero. Object generation is positive and no
newer than the root. Each record lies before the selected manifest; referenced
records may not overlap. Count, sizes, offsets and sums are admitted before
payload allocation. Maximum manifest size is 1,088 bytes.

### Object record: 128-byte header followed by raw payload

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | ASCII FLTXDAT1 |
| 8 | 4 | Format field 1 |
| 12 | 4 | Codec 0 = raw; all others rejected |
| 16 | 8 | Object ID |
| 24 | 8 | Object generation |
| 32 | 4 | Logical bytes, at most 1 MiB |
| 36 | 4 | Stored bytes, must equal logical bytes |
| 40 | 16 | Store identity |
| 56 | 40 | Zero |
| 96 | 32 | Digest of header bytes 0..95 concatenated with payload |
| 128 | Variable | Payload, empty allowed |

The manifest repeats this record digest. Opening verifies every reachable
record before exposing a snapshot. Each read verifies the entire object again;
there is no unchecked read or shared decoded cache.

## Ordinary-file adapter and resource admission

CreateFileW CREATE_NEW (Windows) or openat O_CREAT|O_EXCL|O_NOFOLLOW (POSIX)
creates new regular files only. Final reparse points/symlinks are rejected.
The POSIX parent directory is pinned and fsynced with initial publication;
file fsync handles each durability barrier. Windows uses FlushFileBuffers.
Windows creation-name power-loss durability remains unqualified.

The adapter refuses unknown memory telemetry. It requires at least 608 MiB
physical AND commit headroom, covering a 512 MiB reserve plus bounded cache
and transaction allowances. It checks filesystem free space before creation
and before each transaction, retaining a 1 GiB reserve. These are admission
checks, not reservations against other processes or guarantees against OOM.
Only Windows and Linux telemetry are admitted. File size remains capped at
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

Still required before writable filesystem exposure: compressed extent design,
qualified physical persistence assumptions, process-kill and OS fault tests,
durable namespace operations, writable reopen/recovery, capacity/reclamation,
authentication/key design, filesystem semantics and extensive mounted VM tests.
Actual AMD hardware/runtime fault qualification and comparative compression/
throughput evidence remain separate unmet gates.
