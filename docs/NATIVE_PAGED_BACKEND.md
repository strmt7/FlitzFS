# Native operations over paged object storage

Experimental CPU integration. The existing native filesystem and file-session
engine can explicitly consume a paged object store. This connects a backend;
paged namespace metadata and a Windows factory remain separate work. No driver,
mount, worker or GPU runtime is started by this code.

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
metadata and bounded residency remain next scale work.

Flat volume-head and staged recompression APIs reject paged input before target
writes. The explicit [native paged maintenance](NATIVE_PAGED_MAINTENANCE.md) path
now rebinds native identity and verifies compacted/recompressed new images.
Legacy layout migration, handoff/reclamation, incremental background rebuilding,
optional-codec breadth, Windows factory integration and reader retirement remain.

## Verification

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
