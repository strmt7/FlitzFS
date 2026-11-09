# Native directory, inode and extent layer

Experimental CPU filesystem core; no driver, mount application or GPU runtime is
enabled. This is a native filesystem namespace over the compressed transactional
object store, not an archive reader or archive-backed mount.

## Implemented operations and lifetime

NativeFilesystemWriter consumes an exclusively owned ObjectStoreWriter.
Initialize accepts only a NEW empty generation-zero store. Resume accepts a
writer whose custom backend has already passed identity/generation verification,
exclusive ownership and the recovery fence. There is no new ordinary-file
reopening factory or owner-host permission.

A transaction applies 1..16 ordered operations: create file, create directory,
write a range, truncate, rename/replace, or remove. All metadata and changed
extents are staged privately and published by ONE object-store commit. Admission
failures perform no backing writes; read/validation and resource errors can occur
before publication. Backing I/O failures retain the core's terminal/indeterminate
semantics. Destroy a poisoned writer before independent recovery.

Snapshots retain their original namespace and immutable data references across
writes, truncation, replacement, deletion and object-ID reuse. Inode IDs never
reuse previously committed IDs. Rename preserves the source inode. A new child
requires an existing directory; automatic parent creation is not implicit.
Directories can be removed/replaced only when empty. File/directory replacement
types must match. Moving a directory beneath itself fails. Case-only rename
preserves identity while changing display spelling.

Paths are root-relative '/' strings. The root is the empty string, inode 1.
The current conservative ASCII/case-insensitive component rules reuse the pure
catalog validator: at most 240 path bytes, 100 per component and depth 16, with
unsafe Windows aliases/separators rejected. This is NOT full Unicode/NTFS or
POSIX name qualification. All file CONTENT bytes remain arbitrary and lossless.

Created/modified sequence fields are persistent logical transaction counters,
not wall-clock timestamps. Namespace sequence is distinct from object-store root
generation: root repair can advance the latter without changing user contents.
ACLs, real timestamps, open-handle sharing/deletion, links, streams, mapped I/O,
durable mounted application writes and concurrent dispatcher integration remain
unimplemented. One control thread is required.

## Sparse and partial data semantics

Files consist of independent 64 KiB logical extents. Missing extents are zeros.
A partial write loads only affected old extents, preserves other bytes, and
leaves unwritten gaps zero. A shrink removes later references and zeroes the
remaining tail beyond EOF. Growth exposes zeros, never discarded bytes. Completely
zero extents are omitted instead of allocating zero-filled data objects.

Reads are bounded to 4 MiB, short at EOF, and iterate allocated overlapping
extents rather than scanning a potentially huge sparse hole. Every returned
allocated extent passes the underlying stored/decoded integrity checks before
its bytes escape. Unknown codecs or corrupt data fail; GPU availability is not
needed for read, write or recovery.

## Present admission limits

- At most 256 live inodes including root, and 255 live NONZERO extents across the
  namespace. These limits follow the 256-object store (one metadata object).
- Each extent is 65,536 bytes decoded. Nonzero live data capacity is presently
  16,711,680 bytes, despite a 1 TiB per-file sparse LOGICAL size allowance.
- At most 1 MiB input per write and 4 MiB input per batch; 63 private dirty
  extent buffers (4,128,768 bytes) during staging. A full 64 KiB is reserved for
  namespace metadata inside the core's 4 MiB put budget. Unaligned writes may
  exhaust the dirty-buffer allowance before the input allowance.
  Temporary staging/node limits may reject
  a batch whose operation order exceeds them; delete before allocation at capacity.
- Namespace bytes are capped at 64 KiB. Current graph bounds imply much smaller
  ordinary metadata; all counts, references and extents are admitted.
- The object store retains old physical records and its 64 MiB history cap.
  Unlink/truncate release logical references, not physical history. A compacted
  copy can omit history in a NEW destination while retaining the source. No
  in-place GC, automatic reclaim/handoff, quota/statfs model or production
  capacity claim is implemented.

These are bounded development gates, not the requested final filesystem
capacity. Aggregate decoded live bytes remain capped at 16 MiB in the object
core; the object-count increase does not multiply that budget. At the combined
node/name/extent ceiling, namespace encoding is at most 46,128 bytes, below its
64 KiB cap. Static assertions bind these budgets. Ordinary-file admission now
requires 640 MiB physical AND commit headroom: 512 MiB reserve plus 64 MiB cache
and 64 MiB staging allowances. This is admission, not a whole-process reservation.

### Incremental writer staging and full verification

Opening/resuming a volume and exposing a public snapshot still fully verify all
reachable object bytes and namespace references. The writer privately retains
the last acknowledged model and immutable record references. Each apply reads
and integrity-checks the namespace object, compares it exactly with the cached
model, then reads/verifies only old extents that the operation actually touches.
All newly staged extents pass nonzero/EOF-tail checks; the full graph and unique
ownership are admitted before publication. Namespace-only updates read no data
extents. This private staging view is unavailable to public callers.

This follows the backend's exclusive-writer/immutable-record contract; staging
is not a background media scrub. Corruption of an untouched extent after its
original admission may remain undetected until the next full verification or
read of that extent. Such a read fails before returning bytes; a staging read
failure terminally fences the writer. A full writer-snapshot verification
failure also fences it and reports a native filesystem error, preserving the
public API's error domain. Every public snapshot/open still rejects a corrupt
reachable record. No unchecked data is exposed.

RAM tests separately count actual backing read bytes for a 128-byte overwrite
among 200 raw extents, require bounded staging reads, and require a later public
snapshot to perform full verification. Corruption tests distinguish untouched
data from touched data and metadata; the latter two must fence before writes.
Production scaling still needs indexed metadata/allocation, reclamation, quotas,
concurrency and application qualification.

### Exact unchanged-extent reuse

A nonempty write compares its requested range with the verified staged bytes.
If no bytes change, it keeps the existing extent record instead of re-encoding
and appending it. The file size, modified sequence and namespace transaction
still update, and publication still uses all three replicated-checkpoint
barriers. This is payload write elision, not a weakened durability mode.

Multiple ordered operations can temporarily change, zero or remove a block and
then restore its original contents. For such possible reversions, staging reads
and verifies the original record again and compares all 65,536 decoded bytes.
It reuses that record only for the same original inode and logical block. This
happens before allocating pending object IDs, so another inode cannot take a
restored ID. Rename preserves that identity; delete/recreate does not. Exact
comparison introduces no hash-collision decision. Truncate/regrow still exposes
zeros unless subsequent writes explicitly restore the discarded bytes.

Single changed writes retain the existing one-source-read path. A possible
reversion may add one verified original-extent read per retained dirty block,
with one temporary 64 KiB comparison buffer at a time, released before output
mutation copies and encoding. The 63 dirty buffers and all input/live/physical
caps are unchanged. Dirty buffers plus this decoded scratch are at most 4 MiB;
that subtotal excludes namespace, graph, decoder/encoded storage and allocator
overheads. The existing 64 MiB staging admission allowance remains an admission
reserve, not a whole-process memory guarantee.

RAM checks inspect actual payload record generations and offsets, backing
write bytes, appended history and the three barriers. They cover unchanged
full/partial/cross-extent writes, repeated real edits, zero/restoration,
shrink/regrow, already-zero truncate tails, rename, object-ID allocation order,
delete/recreate, sparse zeros, retained snapshots and independent reopen.
A second-read corruption case must fence before writes. Every observed
write/flush of an elided rename transaction is faulted under all three RAM
persistence-failure modes in raw and automatic compression, followed by
independent recovery and further reuse. The hosted native file transaction also zeroes and restores a separate stable
file while changing other namespace/data. Independent recovery checks that
file's exact bytes, metadata and original payload generation, then continues
writing. Compaction includes the stable file too. The existing process-kill
matrix's 1024-case, 256-event and 3 MiB combined-fixture caps remain enforced;
this is distinct from physical power-loss qualification.
The Linux raw CI lane also runs the strengthened test against the exact prior
implementation and requires its specific identical-write failure before
accepting the current suite.

## Native metadata bytes

Object ID 1 stores the namespace; object IDs 2..256 store live extent records.
There may be no unrelated logical objects or duplicate extent-object ownership.
Extent object IDs can be reused transactionally; old snapshots retain their
old generation/offset/digest references in their own object-store snapshot.

All fields are unsigned little-endian, all reserved bytes zero. Namespace magic
is FLTXNS01. This format is separate from the existing .flfs image and .flcatalog
diagnostic view. There is no automatic import or migration.

### Namespace header: 64 bytes

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | ASCII FLTXNS01 |
| 8 | 4 | Internal format field 1 |
| 12 | 4 | Inode count, 1..256 |
| 16 | 8 | Namespace transaction sequence, positive and <=root generation |
| 24 | 8 | Next inode ID, greater than every live inode |
| 32 | 4 | Extent bytes = 65,536 |
| 36 | 4 | Total live nonzero extents, at most 255 |
| 40 | 16 | Object-store identity |
| 56 | 8 | Zero |

### Inode: 64-byte header, name bytes, then extent references

Inodes are serialized in strictly increasing ID order.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | Stable inode ID |
| 8 | 8 | Parent inode; root uses zero |
| 16 | 4 | Kind 1 directory, 2 regular file |
| 20 | 4 | Zero |
| 24 | 8 | Logical file size; directories zero |
| 32 | 8 | Created namespace sequence |
| 40 | 8 | Modified namespace sequence |
| 48 | 2 | Name byte count, <=100; root empty |
| 50 | 2 | Extent reference count, <=255 |
| 52 | 12 | Zero |

Name bytes follow without a terminator. Each extent reference is two u64 fields:
logical block index, then object ID. Indices strictly increase. The object must
decode to exactly 64 KiB, contain some nonzero byte and lie before file EOF;
any bytes after EOF in its final extent must be zero.

The parser rejects missing/file parents, cycles, disconnected nodes, duplicate
case-folded sibling names, duplicate extent ownership, invalid IDs/sequences,
trailing metadata, foreign objects and invalid extent padding. Store/namespace
identity and integrity must validate before snapshot exposure. Integrity remains
unkeyed; this is not authentication or malicious-rollback protection.

## Validation and interruption coverage

The RAM suite covers cross-extent writes, untouched snapshots, stable IDs,
sparse reads near 1 TiB, truncate/regrow, rename/replace, directory rules, limits,
no-write admission rollback and a separate byte-vector oracle. Capacity cases
combine 256 inodes, maximum-length names and all 255 live extents; verify bytes
in bounded reads; delete all extents atomically; and test 63-buffer admission
versus 64-buffer rejection in both raw and automatic compression. Metadata-negative
cases use valid object-store commits, recomputing enclosing integrity so graph
validation is exercised rather than only checksum rejection.

For both raw and automatic compression, every observed combined file-transaction
write/flush failure is injected with before-write, partial persistence, whole-unit
damage and post-flush outcomes. Recovery must expose the complete old or new
namespace/data; a terminal writer may not retry. Root-repair generation changes
are followed by a new native file commit and independent reopen.

The hosted process-interruption workflow now also exercises a native file
transaction: directory rename, a cross-extent partial write, truncate, replacing
an existing file, and directory removal. At each reported I/O boundary a process
is forcibly stopped. A new CPU process checks the acknowledged namespace/data,
resumes, creates/writes another file and independently reopens it.

All tests/builds run on disposable hosted VMs. Process-kill and RAM tearing are
distinct from physical power cuts. The exact commit's checks and retained reports
must be inspected; no result is inferred merely from this test description.

## Codec admission

Native store and .flfs Zstandard paths now require canonical modern frame magic
before calling optional library parsers, excluding legacy/skippable framing.
Writer-generated bytes remain unchanged. The reviewed public Zstandard header
defines ZSTD_MAGICNUMBER 0xFD2FB528 since v0.8.0. This closes an unnecessary
legacy parser path; it is not a claim of a completed decoder/security audit.


## Compacted new-volume copies

NativeFilesystemWriter::compact verifies a quiescent latest snapshot and copies
only live objects into a fresh volume identity. It rebinds namespace object 1
while preserving paths, inode IDs, sparse extents, bytes and logical sequence
metadata. Source writers must be destroyed first; historical snapshots cannot
silently replace the current acknowledged state. Old source snapshots remain
readable. Normal records retain their stored codec/payload; the namespace
candidate passes normal CPU round-trip checking.

The destination has two separately flushed complete recovery roots. A partial
unpublished destination may be unusable; retry uses a NEW target. No source
write/flush, path swap, source deletion, in-place reclamation or automatic
maintenance occurs. Total storage temporarily includes both volumes. See
COMPACTED_COPY.md for bounds, failure behavior, shared reader locks and hosted
source-digest/interruption qualification.

## Remaining product gates

The native file layer advances persistent filesystem semantics. Writable mounted
callbacks, user/application compatibility, Unicode/metadata/security semantics,
scalable allocation/reclamation, physical persistence qualification, broad
compression/throughput comparisons and actual AMD GPU integration remain unmet.

## Replicated acknowledgement protection

Native initialize/apply now use REPLICATED_CHECKPOINTS.md. Each file transaction
publishes two roots naming the SAME current namespace/data references, with
manifests in separate 64 KiB units and three durability barriers. Publication
generation advances twice; namespace sequence advances once. Qualified resume
repairs degraded checkpoint metadata before returning, and legacy writable
admission promotes publication without changing logical inode/content/sequence
bytes. No public existing-file or mounted write factory is added.

Tests now require latest contents after loss of EITHER current root or EITHER
manifest unit, then repair and a different continued commit. Write staging also
rechecks checkpoint health before publication; the added bounded metadata reads
are included in backing-read counts. Payload replication/reconstruction,
physical power-loss/device assumptions and automatic maintenance remain separate.
