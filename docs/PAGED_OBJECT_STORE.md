# Experimental paged object storage

The explicit [mirrored-payload extension](MIRRORED_PAYLOADS.md) adds capability 2
with bank-separated payload copies and surviving-copy CPU reads. The ordinary
capability-1 encoding described below remains unchanged and single-copy.

Native [shared-base groups](SHARED_BASE_GROUPS.md) require capability-2
backing. Both paged capabilities use separate packed payload and index
regions during compaction. The checked root summaries and changed object
paths are enough to compare the full compacted sizes before a group commits.
[Packed layout and accounting](#packed-rebuild-accounting)

`include/flitzfs/paged_store.hpp` provides an explicitly selected storage path
over the existing `StoreByteIo` backend/ownership contract. It implements actual
compressed-object writes, lookup, deletion, stable keyset pagination, retained
snapshots, recovery and streaming rebuilding. Native filesystem snapshots,
writers and file sessions can now explicitly use this backend through their
existing operation engine. An explicit [inode-record namespace](INODE_RECORD_NAMESPACE.md)
also supports incremental metadata publication. The explicit
[directory-name index](DIRECTORY_NAME_INDEX.md) publishes parent indices in the
same transaction and adds native snapshot cursors. The
[indexed WinFsp composition](WINFSP_OWNED_CONTROLLER.md) connects the paged
store to writable SDK callbacks and background opportunities.

The distinction matters: a million populated object records are now tested;
a million named Windows files and scalable automatic compression grouping are
still integration work. Group membership can span one, two or thousands of
files; group discovery and independently indexed decode units remain separate
from this object catalog. See [the execution plan](DEVELOPMENT_PLAN.md).

The owned indexed memory provider selects the isolation unit from the Windows
page size. This uses the existing paged wire fields and aligns with the live
provider's bounds checked byte copies. Other backends retain their own qualified
write-failure unit. The two 64-KiB root zones and independent CPU decoding
semantics remain stable. Obsolete-record accounting is a cached scheduling
signal, with no new wire fields. [Provider contract](WINDOWS_MEMORY_VOLUME_FACTORY.md)

## Memory and capacity

There is no whole-inventory vector or volume-sized in-memory key map. A lookup
visits a root-to-leaf path; pagination prunes key ranges and returns at most
4,096 entries. Full CPU verification streams one object at a time. Tree height
is bounded at 16 for malformed-input/depth admission. Indexed range reads can
reuse up to four verified lookup nodes during a single request. The ordered
index also reuses checked page bytes within that scope.
[Indexed read verification](INDEXED_NATIVE_VOLUME.md#shared-base-extents)

The original native paged frontend uses the bounded namespace/extent model
with 256 nodes and 255 extents. The [indexed native frontend](INDEXED_NATIVE_VOLUME.md)
stores names, inodes and extent mappings in pages and bypasses that model.

The caller must supply an explicit physical capacity and a backend that admits
growth using current resources. No host capacity, drive, GPU or RAM size is
hardcoded. The old 256-object/16-MiB live-inventory/64-MiB history limits are
unchanged for legacy stores. Paged transactions retain their independent
256-operation/4-MiB-new-input/1-MiB-per-object work limits. At most 8,721 new
metadata pages can be staged per transaction, independently of inventory size.

An optional write-byte allowance charges the full planned append span and
both root writes before admitting backing growth. A refusal leaves the
writer usable and performs no writes or flushes. This lets advisory callers
account for copy-on-write index pages and padding alongside their payloads.
[Coherence catalog](OPTIMIZATION_STATE.md#paged-coherence-catalog)

The tree has 62 entries per leaf and 35 references per branch. Mutations rewrite
affected paths and split pages; empty subtrees disappear. Partially empty pages
are not merged automatically. History remains append-only while readers retain
their roots. Rebuilding copies only live objects and creates densely filled
pages with one unfinished leaf and at most 35 references per tree level in RAM.
It preserves object IDs/generations, rebinds the image identity and checks
decoded CPU bytes independently before publication.

Rebuild is an explicit synchronous API, not automatic background maintenance.
The separate [staged API](STAGED_PAGED_REBUILD.md) now splits copying and complete
CPU verification into one-object opportunities without retiring the source writer.
It does not automatically schedule work or select/reclaim a volume.
It requires a new empty target and a current quiescent source. It never changes
the source or selects/replaces a user's volume. Partial targets survive failure;
they have no valid root until the entire output has been checked. An interrupted
final publication may leave a complete readable output, never a partial catalog.

## Packed rebuild accounting

A rebuild writes one contiguous payload stream, then packed index pages.
Capability 2 stores each payload bank alongside its independently checkable
peer. The index always has separate mirrored banks. Padding seals the end of
each region. Both payload copies still precede the index, and every child
index page precedes its parent in the respective copy. The existing object,
index and checkpoint frames carry all the required offsets and digests.
[Packed staged rebuild](STAGED_PAGED_REBUILD.md#work-phases)

At the same isolation unit `U`, let `N` be the checked object count and
`S` the sum of stored bytes in one copy. Payload space is the aligned
size of `S + 128*N`, doubled for mirrored payloads. The number of index
pages comes from the leaf capacity and each branch level rounded up to the
branch capacity. The index occupies twice the aligned sum of its 4,096-byte
pages. The two fixed root zones complete the occupied size.

`rebuilt_size` accepts a hypothetical sorted inventory and performs no
backing I/O. `rebuilt_size_after` reads changed paths and starts from the
checked root's count and stored-byte total. It preserves untouched encodings,
subtracts replaced or removed records, and charges additions at their
raw lengths. An identity-rebound object contributes zero encoded bytes to
the lower bound and up to its decoded length to the upper bound. These
bounds apply to preserving compaction at the same failure unit. Recompression
with a different codec policy requires measurement.

Separating even/odd mirror pairs from the breadth of the packed index removes
leaf-boundary padding as a dependency between unrelated records. The
application of separate data and metadata regions is local to FlitzFS's
copy-on-write rebuild and failure-unit geometry. Current format references
also place metadata after leading data, and the 2026 CloudTS work explores
separate metadata management in a different workload.
[Apache Parquet format](https://parquet.apache.org/docs/file-format/) ·
[RocksDB table format](https://github.com/facebook/rocksdb/wiki/Rocksdb-BlockBasedTable-Format) ·
[USENIX FAST 2026 CloudTS](https://www.usenix.org/conference/fast26/presentation/zhang-kai)

## Separate on-image capability

This is a new experimental storage encoding, not a product version bump.
Existing `.flfs`, object-root formats 1–3 and native namespace formats 1–7 remain
unchanged. Use `PagedObjectStoreSnapshot::open` explicitly; legacy readers do
not interpret these bytes as their old manifests.

All integers are little-endian; reserved bytes are zero. Integrity uses unkeyed
BLAKE3, not authentication, encryption or rollback protection.

Two 4-KiB roots occupy separate 64-KiB zones at offsets 0 and 65,536. Immutable
records begin at 131,072. Root magic is `FLTXPG01`, capability field 1:

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | Magic |
| 8, 12 | 4 each | Capability, parity slot |
| 16, 24, 32 | 8 each | Publication generation, checkpoint generation, sealed end |
| 40 | 16 | Image identity |
| 56, 60 | 4 each | Metadata isolation unit, index page size |
| 64 | 112 | Root-page reference |
| 176 | 3,888 | Zero |
| 4,064 | 32 | Digest of preceding root bytes |

A reference stores first/last key, object count, decoded/stored-byte sums,
primary/peer offsets and page generation at relative offsets 0..63 (eight
uint64 fields); height at 64 (uint32), zero at 68..79 and digest at 80..111.
Parent/child height, allocation order, generation, key intervals, aggregate
sums and checked address arithmetic are validated before traversal.

Pages use `FLTXPGN1`, capability 1, height, generation, image identity and entry
count in a 64-byte header. Their final 32 bytes contain the digest. Leaves have
64-byte descriptors: object ID, generation and offset (uint64), logical/stored
lengths (uint32), stored-record digest (32 bytes). Branches contain references.
Unused bytes are zero. Object records use the existing 128-byte `FLTXDAT1`
header and independent CPU raw/zero/pattern/optional Zstd/LZ4 codec contracts.

### Compact coherence catalog pages

New coherence catalogs have 1,024-byte index pages under capability 1.
Existing 4,096-byte catalogs keep their geometry when reopened, appended or
rebuilt. The standalone store and file-data images create 4,096-byte index
pages. Both catalog geometries use 4,096-byte commit records, with offset 60
identifying the index page size. Both intact commit records must agree on it.

The compact tree uses the same page header, leaf descriptors and full digest.
Each 88-byte branch reference contains first key, last key, object count,
decoded bytes, stored bytes, primary offset and generation, in that order, as
seven uint64 values. Bytes 56–87 hold the full page digest. The reader derives
the peer offset from the primary offset plus the isolation unit and the child
height from the parent height minus one. The root-page reference in the
commit record retains its 112-byte layout. The reader verifies the resulting
references and the complete metadata checksums.

During a compact-tree update, consecutive siblings that already need
replacement contribute their entries or child references to one run. The
writer splits the run into pages before assigning its addresses. Unaffected
siblings retain their original checked references. Work is bounded by the
admitted transaction and one sibling run at each depth. A commit seals its
new banks before replacing the root pair, which keeps old snapshots readable.
[Measured catalog costs](evidence/indexed-coherence-compact-pages-2026-11-21.json)

## Publication and recovery

The qualified backend failure unit must be a supported power of two in
512..65,536 bytes with meaningful successful flushes. The persisted metadata
isolation unit is the larger of 4,096 and that declared unit. This is an explicit
backend assumption, not device attestation. Read [the failure model](PERSISTENCE_MODEL.md).

Metadata copies occupy different isolation units. For a unit larger than a page,
the allocator packs pages into a primary unit and their peers into the following
unit. A transaction seals both units; later writes never reuse acknowledged
padding. This avoids spending two entire large failure units per page during
ordinary path updates. Streaming rebuild packs consecutive pages into the same
paired banks and seals the suffix when its target tree is complete.
Page writes initialize only alignment gaps and the unused suffix of the final
replica banks with zeros. Complete page buffers initialize their own ranges.
This avoids duplicate backing writes without changing page placement, the on-image
format, occupied bytes or the flush and root publication protocol.
In the focused mirrored 4 KiB-unit fixture, backing writes fell from 32 KiB to
24 KiB for the same 16 KiB append and two 4 KiB root updates. Tests also covered
64 KiB units, multi-bank updates, exact recovery and modeled interruption.
[Source-bound checks](evidence/paged-write-accounting-2026-11-17.json).

Publication flushes appended dependencies, publishes root G+1 and flushes,
then publishes root G+2 in the other root zone and flushes. Both roots name the
same object set. A failed operation permanently fences its writer; recovery may
select complete old or new state. It never silently substitutes an older root
when a selected intact root names damaged descendants. Old snapshots retain
immutable references. The writer lease is shared with the flat store API.

Opening checks roots and the root page, then validates descendants/payloads on
access; unlike the legacy eager reader, it does not scan the whole inventory.
`verify_all()` performs an explicit full CPU scrub. Single metadata-copy loss
can recover through its bound peer and is reported as degradation. Root-only
repair follows a fully verified, identity/generation-leased resume; orphan tails
are fenced and retained. Damaged descendant metadata refuses in-place writable
resume. A streaming rebuild can recover its surviving peers into a new image.
Mirrored payloads in capability 2 can supply verified bytes for an append-only
repair. Healthy source and snapshot references stay usable through the existing
checkpoint protocol. [Mirrored payload repair](MIRRORED_PAYLOADS.md#automatic-repair-after-a-verified-read)

Rebuild now refuses newly written degraded metadata before publication and
independently reparses/scrubs the completed root pair before returning success.
The [native maintenance regression](NATIVE_PAGED_MAINTENANCE.md) exercises both
root-slot and metadata-primary readback corruption. This is a verification fix,
not new payload redundancy or physical durability evidence.

## Executed scope and next integration

[Source-bound local evidence](evidence/local-paged-store-2026-11-15.json) records
raw-only Windows Release and AddressSanitizer checks, deterministic oracle,
malformed-input rejection, modeled write/barrier failures, metadata-unit loss,
streaming rebuild and a one-million-object experiment. These are memory-backed
storage tests, not mounted Windows qualification, real power cuts, a general
compression ratio or a filesystem/GPU speed claim. Tiny records intentionally
make metadata/history overhead visible.

The native backend bridge uses the existing namespace formats 1..7 and the same
Stage, mutation, metadata, sharing, session and CPU validation code. A NEW empty
paged store starts at publication generation 2; native initialization publishes
the root inode normally. Resume requires a recovered exclusive paged writer;
pending/unlinked state still requires explicit retired-context recovery. A real
`flush_checkpoint()` checks roots, backend contract and unchanged extent before
and after the barrier without publishing or rewriting records. Native Flush
retains its own pre/post namespace verification.

Flat volume-head and staged recompression APIs explicitly reject paged sources
before target writes. Generic paged rebuilding changes only store identity.
The separate [native maintenance path](NATIVE_PAGED_MAINTENANCE.md) now rebinds
the native root as well and retains layout/object generations during verified
compaction or recompression. It does not change any existing wire encoding or
implement active-volume handoff, legacy migration or an automatic scheduler.

The explicit inode-record layout is the first metadata persistence step. Finish
ordered name/extent indexes, bounded native residency and atomic multi-index
publication while preserving namespace/security/open-handle semantics;
add explicit old-format migration, resumable scan cursors and bounded caches.
Automatic reclaim/rebuild scheduling, concurrent reader retirement and data
redundancy remain required. Do not relax the flat namespace limits and call
that integration complete.

Current primary-source review used SQLite's
[12 November 2026 source revision](https://github.com/sqlite/sqlite/blob/b4b5ba2afb750999741ebd658d926af2cca3fe64/src/btreeInt.h)
for paged ordered lookup and finite cursor depth, and Linux Btrfs's
[9 November 2026 tree-checker revision](https://github.com/torvalds/linux/blob/a1250669825f96ecc7045ab1b10cdc3d8832339d/fs/btrfs/tree-checker.c)
for strict structural rejection. These informed engineering checks; they do not
prove this independent implementation correct or establish novel compression.
Exact native API/source captures and digests are retained with the evidence.
