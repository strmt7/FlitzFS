# Indexed native volumes

An indexed native image uses mirrored paged storage and reads inode, name and
extent records on demand. The native snapshot API provides lookup, directory
pages, metadata and file reads. The native writer can initialize a new mirrored
image and resume a fully verified indexed image. Its path transactions update
the checked index and changed records together. New volumes use capability 2,
which supports file lifetimes through `NativeFileSession`. Useful shared-base
publication selects capability 3 for aligned patches or capability 4 when a
member copies ranges from its base. The managed head and
writable WinFsp SDK adapter can select indexed images. The owned mounted
provider passed an earlier indexed external-client fixture on disposable
Windows Server 2025. Current Windows 11 client and mounted shared-base
qualification remain open.
[Managed indexed head](MANAGED_PAGED_VOLUMES.md#indexed-images) ·
[WinFsp indexed adapter](WINFSP_PAGED_VOLUMES.md#indexed-volumes)

## Root and keys

Object 1 is the little-endian native root. Capability 1 has 96 bytes.
Capability 2 has 120 bytes and appends three lifetime counts. Capability 3
has 136 bytes and adds the shared-base and member counts. Capability 4 preserves
the 136-byte root layout and permits anchor-copy members. Readers accept all four
capabilities and check each against its exact length.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | `FLTXPV01` |
| 8 | 4 | Indexed native capability, 1 through 4 |
| 12 | 4 | Workload accounting flag, 0 or 1 |
| 16 | 8 | Native namespace sequence |
| 24, 32 | 8 each | Next inode ID and next extent ordinal |
| 40, 48, 56 | 8 each | Inode count, mapped extent count and charged extents |
| 64 | 8 | Committed input bytes |
| 72 | 16 | Exact paged store identity |
| 88, 92 | 4 each | Extent length 65,536 and zero reserved field |
| 96, 104, 112 | 8 each | Capability 2 and later counts for active lifetime records, detached inodes and retired inodes |
| 120, 128 | 8 each | Capability 3 and later shared-base and member counts |

The native namespace sequence preserves file provenance through a copy.
It is independent of the new object's publication generation. A reader
requires the explicit indexed capability for `open_image`.

Inode objects use `(1ULL << 63) | inode`. Data objects use
`(1ULL << 61) | ordinal`. Both sequences have a positive usable 61-bit ordinal.
The ordered index uses its existing reserved object domain at `3ULL << 61`.
The index's kind 2 covers names and extent locations. Kind 3 adds
shared-base backreferences in the same protected tree.

| Key | Value |
| --- | --- |
| Parent inode, canonical name bytes | Child inode |
| `(1ULL << 61) \| inode`, 8-byte big-endian block number | Data object ID |
| `(1ULL << 61) + base ID`, 8-byte big-endian member object ID | Member object ID |

The inode body uses the canonical format-7 header, stored spelling and optional
security descriptor. Extent locations live in the index. Capability 1 requires
linked inodes. Capability 2 uses the canonical pending and detached inode flags
and a checked lifetime marker. Lookup checks the name key against the referenced inode. A data object
starts with `FLTXPE01`, the owner inode ID and the logical block number. The
remainder is a nonempty byte prefix of that extent with trailing zero bytes
omitted. Readers check the owner, range and frame before copying. Missing
extents and omitted tails read as zeros. Data objects retain the paged store's
independent CPU decoders and mirrored-payload verification.

## Shared-base extents

Capabilities 3 and 4 keep the ordinary extent frame. A grouped member has
the same 24-byte owner and logical-block header with magic `FLTXPD01`,
followed by the existing checked [shared-base delta](SHARED_BASE_GROUPS.md#exact-member-bytes).
A base object uses the data-object allocator and starts with `FLTXPA01`,
followed by 65,536 immutable bytes. Both records use mirrored storage.
The member binds the base object ID, generation and decoded digests.
The index must also contain that exact base/member pair.

CPU reads decode at most one base per member extent and validate the
result against the member digest and file bounds before slicing the
requested range. The full scrub checks every member, the referenced
anchors and the index and volume totals. Bases are separate from visible
files, so removing the file used as the base leaves other files readable.

Explicit grouping prepares independently checked member frames one extent at
a time from a pinned checkpoint. Foreground edits can proceed between steps.
Publication rechecks selected indexed bindings and compares complete packed
layouts from the checked object totals and changed lookup paths. The bound
charges replacement data and index pages at raw size and requires a strict
reduction. A declined candidate performs no storage writes. An existing
immutable anchor can accept more members in bounded transactions without
rewriting the group.

### Anchor-copy members

Capability 4 supports `FLTXGM02` after the ordinary 24-byte grouped extent
header. The 128-byte member header uses version 2 and the same decoded length,
base ID, generation and digest fields as version 1. Offset 96 holds the
instruction count. Offset 100 holds the exact instruction payload length.
Offsets 104–127 are zero.

Instructions append bytes in output order. Each starts with a little-endian
32-bit field. The low 31 bits give the positive byte count. A clear high bit
is followed by that many literal bytes. A set high bit is followed by a
little-endian 32-bit offset into the immutable base. A copy must fit wholly
within the base extent. All instructions together produce exactly 65,536 bytes
and consume the entire payload. The CPU reader checks both decoded digests.

The optimizer compares the copy representation with aligned patches and uses
the smaller private frame to bound the complete packed image. It builds one
bounded index for the selected base and compares candidate matches against its
actual bytes. The root capability is raised in the same transaction as the new
member frames and remains raised for subsequent writes and copies. Capability 4
volumes may contain version 1 members. Earlier capabilities reject version 2
members, and readers limited to capability 3 refuse a capability 4 root.

An edit or removal detaches its old reference in the same COW transaction
as the file change. Ordered index ranks determine whether an anchor has
surviving references without scanning its members. The last removal
retires the base from the current root while old snapshots keep their
previous records. Actual occupied space is reclaimed by compaction.
The packed layout removes the inventory walk from proposal sizing. The owned
maintenance controller now discovers indexed candidates and advances group
preparation in admitted opportunities. A grouped volume passed six paired
SDK overwrites and exact CPU recovery with the other three member files intact.
The AMD worker compressed the incoming overwrite.
[Shared-base evidence](evidence/indexed-shared-base-2026-11-18.json) ·
[Packed layout evidence](evidence/packed-rebuild-accounting-2026-11-18.json) ·
[Cooperative preparation evidence](evidence/cooperative-indexed-shared-base-2026-11-18.json)

### Automatic candidate discovery

The scanner seeks the next allocated extent in the ordered index. It advances
past sparse address ranges and unlinked or pending inodes without a full
directory or object inventory. Each sampling opportunity reads at most one
extent and its dependency. The controller keeps fingerprints for one scan
window and a bounded set of representatives from older windows. Eight aligned
bands and up to four offset-independent features nominate candidates.
Offset-independent features pair minima from distinct fingerprint buckets
over 16-byte windows. The feature scan excludes trailing zero padding and
zero window fingerprints. Candidate features guide selection. Exact readback
and the full packed-image bound govern publication.

A candidate carries the sampled object identity and generation into group
preparation. An intervening selected change invalidates the candidate before
its payload is decoded. Unrelated edits preserve the cursor and the prepared
work. The cursor reaches the end of its current pass before revisiting earlier
positions affected by foreground writes. Rename and other metadata-only
updates leave an idle scanner idle. Reusable anchors let later windows extend
an existing group through independently verified commits.

Detached-inode reclamation and managed compaction share the controller's
serialized opportunities. An active group releases its source before the
controller begins a compaction handoff. The controller admits work using
fresh resource observations, foreground readiness and measured duty. Current
representatives and scan progress live in memory. Durable advisory pages,
cross-restart continuation and measured collective AMD benefit remain open.
[Automatic indexed discovery evidence](evidence/automatic-indexed-discovery-2026-11-18.json)

## Writable transactions

`NativeFilesystemWriter::initialize_indexed` creates the empty root, root
inode and index in one commit. `resume` checks the reachable inode and extent
inventory and both copies before admitting writes. The namespace sequence and
allocators advance only through a selected transaction. Existing readers keep
their immutable checkpoint.

`apply` and `apply_with_metadata` support creation, sparse writes, truncation,
rename with optional replacement, removal, and basic and security metadata.
An edit reads the names, inodes and extent ranges it needs. Missing blocks read
as zeros. Shrinking a file clears deleted mappings and the unused tail, so
re-extension cannot expose previous bytes. Directory moves check cycles and
validate descendant paths when the new prefix grows. The root, affected inode
records, data frames and index edits share one mirrored copy-on-write
publication. A resource or capacity refusal during preparation leaves the
checkpoint and writer usable. An ambiguous publication fences the writer until
independent recovery.

The indexed layout uses 61-bit inode and extent IDs and admits path batches
against the paged transaction's working budget for owned input and affected
records. Shrinking a live file can still exceed one transaction. Large file
deletion uses the retirement protocol below. Expanding
directory moves currently scan the subtree in pages. Per-directory summary
metadata or revised path representation must remove that foreground cost before
large production mounts.

Stable-inode writes and truncation also use this transaction. Append chooses
the current EOF under the writer's control. A constrained write clips its
count to that EOF and preserves metadata when it writes zero bytes. The
transaction returns the selected offset, committed count, allocation, EOF
and metadata. File identity survives a rename and reuse of the old name.
[Executed indexed inode I/O evidence](evidence/native-indexed-inode-io-2026-11-18.json)

An owned write can also supply LZ4 advice for an extent. The writer verifies
the candidate against the final stored body, including canonical sparse
trimming. It adds the existing 24-byte inode and block header as leading LZ4
literals and verifies the resulting frame before publication. The data codec,
extent format and recovery reader are unchanged. Invalid, stale, ambiguous or
unavailable advice leaves the ordinary CPU encoding path available.
[Indexed advice and deferred writes](NATIVE_WINDOWS_LZ4_ADVICE.md)

The raw fixture created, paged and byte-checked 1,024 nonempty files. The
latest checkpoint occupied 3,792,896 bytes. The selected 17-byte replacement
read 202,372 backing bytes and full scrub read 291,765,432. The three failure
modes at 77 write or barrier positions recovered 223 previous and eight fully
committed checkpoints. The same fixture verified retained snapshots, resumed
writing, a sparse offset near the 64-bit boundary, capacity rejection and
continued writes. These are measurements of the development fixtures.
[Executed writer evidence](evidence/native-indexed-writer-2026-11-18.json)

## Indexed file lifetimes

A delete-pending file retains its name and payload. The owning session refuses
a new open and can cancel intent under its existing disposition policy.
Cleanup removes the name while existing file contexts can still read and
write by stable inode. A new file at the old name gets its own inode and data.
The session authorizes retirement only after the final active context closes.
[WinFsp interface contract](https://winfsp.dev/doc/WinFsp-API-winfsp.h/) ·
[Windows disposition flags](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/2e860264-018a-47b3-8555-565a13b35a45)

Capability 2 stores the following one-byte markers, keyed by owner inode.

| Record ID | Byte | Meaning |
| --- | --- | --- |
| `(2ULL << 61) \| inode` | 1 | Pending intent on a linked inode |
| `(2ULL << 61) \| inode` | 2 | Detached inode accessible to existing contexts |
| `(5ULL << 61) \| inode` | 3 | Retired inode awaiting reference removal |

A detached or retired inode carries no parent or name. The single checkpoint
publishes its marker, inode fields, names and counters together. Full CPU scrub
checks marker ownership and the extent owner's role, then reconciles the root
counts. Current snapshots and writes refuse retired inodes. Earlier snapshots
keep their own mappings and bytes.

For a large closed file, retirement first publishes the retired marker.
`reclaim_retired_inode` removes one bounded prefix of the remaining extent
mappings and their data object references per call. The remaining keys record
progress without an in-memory cursor. The final batch removes the inode and
releases its remaining charge. Normal reopen accepts a retired backlog.
Reopening pending or detached inodes requires the separate closed-session API
and proven retirement of the previous contexts. That recovery finishes one old
context owner per commit and can enqueue its large-file work for later steps.

This removes live references while the append-only backing retains history.
Physical reuse, scheduling this work on qualified idle opportunities, and
incremental truncation of live files remain separate implementation steps.
The Windows session fixture retired a closed file with 273 mapped extents in
five reclamation calls, with an intervening reopen. The closed-session and
first-reclamation fault sweeps covered 180 simulated storage interruptions.
Each recovered a complete checkpoint and preserved the unrelated file.
[Indexed lifetime and retirement evidence](evidence/native-indexed-lifetimes-2026-11-18.json)

The owned Windows controller processes the retirement queue using fresh resource
observations and one transaction at a time. Post-join CPU inspection can access
checked detached bytes before sealing resolves the old lifetime records.
[Owned Windows path](WINDOWS_MEMORY_VOLUME_FACTORY.md)

## Export and validation

`create_indexed_copy` takes a legacy snapshot with resolved file lifetimes and
a new empty backing with a new identity. It stages records in bounded
transactions. Each transaction publishes its owned records and corresponding
index edits together. The native root is absent during staging. The final
transaction publishes it after independent checked readback of the inventory,
parent graph, typed extent references and both payload copies. Failures can
leave a partial candidate to inspect or discard. The source retains its bytes
and selection throughout.

Opening the completed image checks the root, index root and root inode.
Lookups and range reads verify the pages and records they need. Full scrub
also checks both mirrors, exact reachable objects, mappings, ancestry and
charged extents without building an inventory-sized map. The public
`list_inode_page` cursor belongs to the exact immutable snapshot.

Legacy shared-base members are decoded through the existing CPU path and
re-encoded as individual extent records. The export preserves decoded bytes,
file IDs, timestamps, descriptors and reservations in the exercised fixtures.
Automatic adoption needs the indexed mount/provider lifecycle and preservation
of collective representations across a qualified migration. The handoff must bind the
source and destination checkpoints independently
of the preserved namespace sequence. It also needs independently verified
layout equivalence and enough retained source state for immediate rollback.

The raw 1,024-file fixture measured 3,588,096 occupied bytes including history
and redundancy. Open read 90,736 backing bytes and a path lookup read 179,292.
Full scrub read 292,261,348 bytes. These counts expose the current metadata
overhead and guide page-layout and scrub work. [Executed evidence](evidence/native-indexed-volume-2026-11-17.json)
