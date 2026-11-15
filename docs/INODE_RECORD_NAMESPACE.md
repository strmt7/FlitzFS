# Incremental inode-record namespace (experimental)

The explicit `NativeFilesystemWriter::initialize_inode_records` factory creates
a new namespace on a new empty paged store. It publishes a fixed 64-byte root
and individual inode objects through the existing native mutation/session engine.
Only changed inode frames are rewritten; deletion/retirement removes their index
entries in the same transaction as the namespace/data changes. Old snapshots
retain immutable prior references. No mount, worker or driver is started.

This is a persistence step. The frontend still loads and validates a complete
bounded Model, performs global lookups and keeps per-inode extent maps in RAM.
The 256-node/255-extent/64-KiB aggregate metadata limits remain. Indexed names,
paged extent maps, bounded caches, incremental verification, migration/rebuild
and Windows factory integration are still required before claiming scale.

## Encoding and CPU admission

The root is object 1. Inode objects use `(1ULL << 63) | inode`; inode zero and
identities using the reserved bit are invalid. Inode identities are never reused. Data IDs
remain the existing native 2..256 range. Store records bind object identity,
generation, decoded/stored lengths and digests. Inode bodies must additionally
match their inode index key and exact independently validated frame boundary.

All integers below use the existing little-endian encoding.

| Root offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 8 | `FLTXPN01` |
| 8 | 4 | Inode-record layout capability 1 |
| 12 | 4 | Inode count |
| 16, 24 | 8 each | Native sequence and next inode |
| 32, 36 | 4 each | Extent size 65,536 and extent-reference count |
| 40 | 16 | Store identity |
| 56 | 8 | Committed input bytes |

Each inode body is exactly the existing format-7 104-byte node header followed
by its name, optional framed security descriptor and ordered 16-byte extent
references. Counts, lengths, flags, key/inode identity, parent/name graph, tail
zeros and all reachable data are checked before files are exposed. Records may
not borrow bytes from adjacent inode objects. Unknown root capabilities, missing
or foreign records, invalid allocation and identity/sequence mismatches refuse
the image. Reading never writes it or initializes a GPU.

The normalized native semantics are format 7; workload accounting starts enabled
at zero and exact shared references are supported. This avoids a later feature
upgrade forcing unrelated inode header rewrites. Existing flat namespace formats
1..7 and ordinary paged initialization retain their bytes/behavior. New readers
accept both layouts; old readers explicitly reject this new magic. There is no
in-place conversion or claim that an old reader understands the new layout.

## Atomic publication and bounds

Fixed root, changed/removed inode records and data mutations publish through one
existing three-barrier paged transaction. Node metadata does not take away the
existing data extent allowance. The internal native adapter admits at most 512
combined data/metadata operations, with the same 4-MiB input bound. Public paged
commits retain 256 operations. Page staging is bounded from the actual changed
key count and tree-height allowance. Native operation, dirty-extent and metadata
bounds remain; this is not a larger-volume scalability claim.

A successful native Flush preserves its pre/post namespace verification and
actual backend barrier. Read/metadata failure or indeterminate publication fences
the writer. No automatic replay follows. Explicit closed-context recovery retains
the prior pending/unlinked lifecycle semantics.

Paged index/root metadata has its existing replica protection. Inode object
payloads, like the former namespace payload, are integrity checked but not
independently replicated. Losing required inode/data bytes cannot be repaired
by a checksum. Native-aware protected payloads and repair remain R01 work.

## Executed scope and accounting

The dedicated target runs the shared native operation/session/fault suite against
this layout. Additional cases check unchanged inode generations, maximum combined
inventory, a 255-extent truncate requiring 257 atomic operations, and forged
record identities/counts/boundaries. The larger truncate additionally covers all
22 observed write/barrier cuts in three failure modes. An independent CPU snapshot verifies bytes
and continued writes after modeled failures. These are synthetic memory models,
not mounted, hardware power-cut or production driver qualification.

The broader 96 selected native/storage suites passed in raw-only Windows Release
and AddressSanitizer builds. After adding the large-truncate failure campaign,
both affected targets passed again in each configuration: 7,930 inode-record
checks with 333 injected failures, plus the existing paged-backend checks. This
does not claim full CTest, optional-codec or mounted Windows qualification.

In the 128-empty-file diagnostic, one write changed one inode record: 186 bytes
of namespace input versus 13,882 bytes for the whole logical namespace. Actual
backend writes were 110,592 bytes including data, index/padding and root work.
This is a deterministic write-accounting fixture, not a speed benchmark or a
complete-size compression win. Per-record headers and extra index entries cost
space; all such overhead must remain charged in future compression comparisons.
[Source-bound validation](evidence/inode-record-namespace-2026-11-15.json).

The reviewed 12 November 2026
[SQLite source](https://github.com/sqlite/sqlite/blob/b4b5ba2afb750999741ebd658d926af2cca3fe64/src/btreeInt.h)
separates ordered page indexing from record payload/framing. It informs this
incremental decomposition, not its correctness or performance. No SQLite code,
file format or runtime dependency was introduced.
