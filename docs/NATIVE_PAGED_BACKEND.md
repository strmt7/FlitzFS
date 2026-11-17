# Native operations over paged object storage

Experimental CPU integration. The native filesystem and file-session engine
can explicitly consume a paged object store. The
[writable Windows adapter](WINFSP_PAGED_VOLUMES.md) uses this backend for
managed volumes. Demand-paged native namespace metadata remains part of E01.

## Shared engine and compatibility

Native snapshot `open` and writer `initialize`, `resume`, `resume_closed_session`
accept flat or paged storage types. Internal dispatch selects storage. Native
validation, private Stage processing, mutations, workload provenance, basic/
security metadata, allocation, sharing and session lifetimes stay in one engine.

Namespace formats 1..7 and existing object/page bytes are unchanged. Backend
selection is explicit; old bytes are never silently reinterpreted. Initialize
requires a generation-zero empty flat store or the initial generation-two empty
paged store, then publishes the native root through a replicated transaction.
Publication generation and namespace sequence remain distinct.

Native paged open fully verifies the bounded namespace, inventory, references
and payload bytes before exposing files. Generic paged open's lazy contract
does not replace native verification. Pending/unlinked state requires explicit
retired-context recovery. Reading needs no GPU or worker.

## Checked live queries

For inode-record volumes, live reads and metadata queries compare the committed
root and verify the inode records they use. A path query also verifies its
ancestors and any persisted directory records it crosses. A range read verifies
its selected extents and any decode dependencies before returning bytes. A
bounded cache can reuse four verified object-index pages within that query.
The next query opens a new check scope. Corruption in a selected record fences
the writer and leaves the acknowledged checkpoint untouched.

Mutation, flush and recovery continue to check the full required namespace.
Legacy monolithic namespaces use their original verification path. In the
132-inode directory-index fixture, a live name lookup read 17,655 backing
bytes and complete namespace validation read 2,262,944 bytes.
[Checked read evidence](evidence/native-scoped-read-2026-11-17.json)

## Flush and ownership

Paged commits retain their three barriers and shared backend writer lease.
`flush_checkpoint()` checks current roots, replica health, backend contract,
failure unit and unchanged retained extent before and after the actual barrier.
It publishes no root, advances no generation and writes no record. Native Flush
also retains pre/post namespace verification. No payload scrub or physical
device-flush attestation is inferred.

Preflight allocation pressure leaves the writer usable. A metadata/contract
mismatch, changing extent or failure after barrier entry fences it. Never
replay an indeterminate commit; retire its writer before independent recovery.

## Remaining integration

Ordinary initialization still uses one namespace object. The explicit
[inode-record layout](INODE_RECORD_NAMESPACE.md) publishes a fixed root and
changed inode records instead. Both frontends still load a complete Model;
their 256-node, 255-extent, path, transaction and allocation bounds remain.
The million-object index test is not a million-file filesystem qualification.
The explicit [directory index](DIRECTORY_NAME_INDEX.md) adds ordered persisted
names and snapshot-bound pagination. Split name pages, demand-loaded inode/extent
metadata and bounded residency are being developed for the scalable namespace.

The explicit [managed paged-volume factory](MANAGED_PAGED_VOLUMES.md) now
connects typed head selection, writable resume, context-preserving synchronous
handoff, shared-base control and funded advisory provenance. The legacy flat
staged-recompression API still rejects paged input; its idle form defers before
target I/O. The explicit [native paged maintenance](NATIVE_PAGED_MAINTENANCE.md) path
now rebinds native identity and verifies compacted/recompressed new images.
Legacy layout migration, automatic reclamation, wider codec support, product
installation and full restart/retirement qualification remain open.

## Ordered name pages for the scalable namespace

The internal name index uses 4 KiB slotted pages keyed by
`(parent inode ID, canonical name bytes)`. A leaf binds a key to a stable
inode ID. A branch binds each child to its greatest key and subtree item
count. Lookup follows one path. Enumeration resumes strictly after the given
name in the same immutable snapshot and stops at the requested parent
boundary. The caller applies Windows name policy before using the index and
preserves the snapshot identity with each cursor.

The 40-byte `FLTXNT01` record holds the version, height, root page ID, next
page ordinal and item count. Its object ID is `0x6000000000000000`. Name
page IDs add a nonzero ordinal from the same reserved object domain. The
reader checks ordinals against the root allocator and the writer never reuses
them. A `FLTXNP01` page starts with a 40-byte header containing the version,
height, row count, reserved field, page ID and subtree item count. Each
little-endian 16-bit slot points to a row with a parent ID and an inode or
child page ID. A branch row also stores the child's count. The remaining bytes
of the row are the nonempty canonical name. The decoder checks the slot
boundaries, strict ordering, page identity, height, counts and parent key bounds.

A name edit supplies its expected current binding and replacement. The editor
splits by encoded page size, merges fitting neighbors and retires empty pages.
It stages only touched pages and publishes them with any accompanying inode or
data records in one protected paged-store commit. A known degraded checkpoint
requires recovery before publication. Retained snapshots still read the previous
pages. Scrub checks the tree and streams the reserved object domain to detect
unreachable records. The index requires mirrored payloads and participates in the
existing checked object-store rebuild.

This is the object-backed layout under evaluation for E01. Its reserved object
domain needs explicit capability negotiation before native volumes use it. The
existing reader, writer, mount format and limits still apply to current volumes.
Demand-loaded inode and extent lookup, coordinated allocation, migration and the
comparison with typed pages remain part of that integration.

## Verification

Standard WinFsp lifecycle tests use sealed managed memory and an injected
provider. The file-backed lifecycle test remains in the integration fixtures
and requires its qualified fixture parent before creating files.
[Directory and session results](evidence/native-directory-verification-2026-11-17.json)

The memory test compares bytes, IDs, sequences, metadata, reservations and
workload counts against the flat engine through mixed native operations. It
covers snapshots, shared private updates, real session rename/delete/close,
restart, pending-context and foreign-object rejection, and incompatible
compaction refusal before writes.

Transaction failures cover all observed write/barrier cuts, declared 4-KiB and
64-KiB failure units, torn writes and unit loss. Independent CPU recovery must
select complete old/new state and continue correctly. Flush tests include real
barriers, post-barrier metadata/extent changes, changed contracts and allocation
failure before/during/after entry. These are modeled memory failures, not
physical power cuts or mounted Windows qualification.
[Executed source-bound evidence](evidence/native-paged-backend-2026-11-15.json).
