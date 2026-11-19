# Indexed native volumes

An indexed native image uses mirrored paged storage and reads inode, name and
extent records on demand. The native snapshot API provides lookup, directory
pages, metadata and file reads. The native writer can initialize a new mirrored
image and resume a fully verified indexed image. Its path transactions update
the checked index and changed records together. New volumes use capability 2,
which supports file lifetimes through `NativeFileSession`. Shared-base
publication selects capability 3 for aligned patches, 4 for anchor copies,
5 for packed XOR planes, 6 for modular deltas or 7 for sparse XOR planes.
The managed head and writable WinFsp SDK adapter can select indexed images. The owned mounted
provider passed an earlier indexed external-client fixture on disposable
Windows Server 2025. Current Windows 11 client and mounted shared-base
qualification remain open.
[Managed indexed head](MANAGED_PAGED_VOLUMES.md#indexed-images) ·
[WinFsp indexed adapter](WINFSP_PAGED_VOLUMES.md#indexed-volumes)

## Root and keys

Object 1 is the little-endian native root. Capability 1 has 96 bytes.
Capability 2 has 120 bytes and appends three lifetime counts. Capability 3
has 136 bytes and adds the shared-base and member counts. Capabilities 4 through 7
keep the 136-byte root layout and permit additional group encodings. Readers
accept all seven capabilities and check each against its exact length.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | `FLTXPV01` |
| 8 | 4 | Indexed native capability, 1 through 7 |
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

Capabilities 3 through 6 keep the ordinary extent frame. A grouped member has
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

Indexed preparation also accepts borrowed base-copy suggestions for its next
member. It checks bounds, order and each proposed byte range against the
selected decoded extent and immutable base. Valid hints compete with the CPU
encodings. The chosen frame passes complete CPU decoding and the existing
packed-size and live-generation checks. A selected copy frame requires at
least capability 4 and can be read independently on CPU.

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

### Packed XOR members

Capability 5 admits `FLTXGM03` after the ordinary grouped extent header. Its
128-byte member header uses version 3 and retains the base identity, generation,
decoded size and both digests. Offset 96 holds the fixed tile count of 256.
Offset 100 holds the exact body length. Offsets 104–127 are zero.

The body covers the extent in consecutive 256-byte tiles. Each tile starts with
an eight-bit mask of the XOR planes that contain changes. A zero mask uses one
byte for an unchanged tile. For each set mask bit, in increasing bit order, the
body contains 32 bytes from that plane. In each plane byte, the least significant
bit belongs to the first of the eight covered positions. The last tile ends at
the exact body boundary. Each advertised plane must contain a change. At least
one tile in the member must change.

The CPU decoder starts with the verified immutable base and applies the XOR
planes to reconstruct one extent. It checks both digests. A member must be
smaller than the current competing private frame and the published group must
pass the existing strict complete-image savings check. The COW transaction
raises the capability only when it publishes a selected XOR member. Capability
5 also reads aligned and anchor-copy members. Earlier capabilities reject
packed XOR members.

#### Batched AMD advice

A qualified controller can ask the owned AMD worker for packed XOR bodies using
the same immutable base and member windows as anchor search. The worker checks
the current adapter, warp and launch geometry and the admitted memory envelope.
It packs 256-byte tiles into each bounded reply slot and returns an empty slot
when the candidate exceeds that transport capacity. The selected layout can
supply 16 KiB or 64 KiB slots. The wider layout fits a two-plane change across
the full extent and every packed body that can beat the raw extent size. The
writer still prices each result under the usual stored-image rule.
[Wider reply validation](evidence/amd-wide-packed-xor-2026-11-19.json)

The reply binds the batch and each group to their original identities and
source digests. The controller collects it after successful worker and
supervisor retirement. For each exact staged member, the writer applies the
canonical body to the base and compares every byte with the original extent.
It constructs a frame and attaches the authoritative digests only when the
body beats the competing size. Valid bodies at or above the limit need no
CPU repack because the tile encoding is canonical. Missing or invalid bodies
invoke the CPU encoder. The full-image savings and live-generation checks
still govern publication. The controller verifies the batch only after
all supplied bodies pass the byte comparison.
[Packed XOR verification and recovery](evidence/amd-packed-xor-verdicts-2026-11-19.json)

### Modular-delta members

Capability 6 admits `FLTXGM04` with member version 4. The shared header
retains the verified base identity, generation and both digests. The declared
decoded extent and its base remain 65,536 bytes.

| Header offset | Bytes | Value |
| --- | ---: | --- |
| 96 | 4 | Tile count. Zero for a bias-only member, otherwise 256. |
| 100 | 4 | Exact body length |
| 104 | 1 | Little-endian word width in bytes. One of 1, 2, 4 or 8. |
| 105 | 7 | Zero |
| 112 | 8 | Unsigned additive bias within the selected word width |
| 120 | 8 | Zero |

The predictor adds the bias to each base word modulo its width. The
encoder subtracts the predictor from the original word using the same
modulus and applies ZigZag to the signed interpretation of that residue.
An exact nonzero bias needs no body. Other members use consecutive
256-byte tiles. Each tile has a one-byte residual bit width followed by
its least-to-most-significant bit planes. A plane takes 32 bytes divided
by the word width. Its first word occupies the low bit of the first byte.
The highest advertised plane must have a set bit. A nonempty body must
contain a residual and end at the exact tile boundary.

At each word width both encoders consider the first difference, zero and
the quartiles of sixteen staggered samples. The shared selector orders the
differences in the signed modular interval around the first difference and
removes duplicate biases. The CPU prices the first and zero biases before
sampling. Additional biases require a residual body, so it can skip them
when the competing frame is already smaller than the minimum size with a
body. Once a residual establishes that the candidate needs a body, scoring
stops if its tile cost reaches the exclusive size bound. The encoder emits
only a candidate that beats that bound.
The writer independently reconstructs every byte and applies the existing
strict complete-image size check before COW publication. Capability 6
readers support earlier members. Earlier capabilities refuse modular-delta
frames. The protected member contains every parameter needed for CPU recovery.
[Parquet modular arithmetic and bit packing](https://parquet.apache.org/docs/file-format/data-pages/encodings/) ·
[Blosc2 filter composition](https://blosc.org/c-blosc2/c-blosc2.html)

The automatic scanner samples sixteen 64-byte regions and tries each
word width. It hashes their modular differences from the first word and
uses those signatures alongside existing similarity features. Funded
maintenance can persist them in the paged coherence catalog. A subsequent
scan can find a reference outside its bounded memory window, then recheck
the live revision and decoded bytes during preparation.
[Modular coherence evidence](evidence/modular-coherence-catalog-2026-11-19.json)

#### Batched AMD modular-delta advice

The configured Windows owner can send several groups to one supervised
AMD worker. The private v6 kernel prices all distinct biases across the
residual tiles and packs the smallest admitted result. Its block-wide
barriers and checked shared-memory requirement
apply independently of wave size. Launch size follows the qualified dispatch
grant, device geometry and live headroom.
[HIP programming model](https://rocm.docs.amd.com/projects/HIP/en/latest/understand/programming_model.html)

The worker checks the device slots and sends a compact reply. Each group
has a length prefix and each member carries a payload length or an empty
slot. The batch and group headers bind the reply to its immutable request.
Admission still charges the complete worst-case mapping and device slabs.
The owner waits for exact worker retirement before consuming the reply.

The indexed writer reconstructs the exact member on CPU, checks its bytes
against the staged original and compares the resulting frame size with
the competing representations. The CPU encoder handles rejected, missing
or uncompetitive candidates. A payload mismatch also fails the recovery
ticket. Protected publication uses the same capability 6 member format.
[AMD execution and CPU readback evidence](evidence/amd-modular-delta-2026-11-19.json)
[Sampled-bias comparison](evidence/amd-sampled-bias-2026-11-19.json)
[Equivalent CPU and AMD search](evidence/cpu-amd-modular-parity-2026-11-19.json)

### Sparse XOR members

Capability 7 admits `FLTXGM05` with member version 5. It uses the same
128-byte header as packed XOR, including a tile count of 256 at offset 96
and zero reserved bytes at 104 through 127. It references one protected
65,536-byte base and never requires decoding a second member.

Each 256-byte tile begins with its nonzero plane mask. A zero mask ends that
tile. A nonzero mask is followed by a sparse plane mask, which is a subset
of the first mask. Subsequent plane data follows increasing bit order.

| Plane type | Payload |
| --- | --- |
| Dense | A 32-byte bitmap using the packed XOR bit order. The smaller of the set and clear bit counts is at least 31. |
| Sparse set | A descriptor from 1 to 30, followed by that many strictly increasing one-byte positions to flip. |
| Sparse clear | A descriptor from 128 to 158. Flip the entire plane except the strictly increasing positions listed in the low seven bits of the descriptor. |

An empty change, extra bytes, incomplete tile or noncanonical plane fails
validation. The decoder checks the protected base and reconstructed member
hashes. CPU and GPU-assisted candidates compete on exact frame size, with
ties favoring the existing representation. The writer can convert a dense
AMD reply after CPU reconstruction matches the staged input. It decodes
the selected sparse frame separately before one COW transaction publishes
the member and raises the capability. The CPU encoder can form sparse
members when packed advice is unavailable or an existing dense slot cannot
carry a useful body. Capability 7 readers accept earlier group versions.
Readers limited to capability 6 reject a capability 7 image.
[Sparse XOR format and recovery evidence](evidence/indexed-sparse-xor-2026-11-19.json)

### Automatic candidate discovery

The scanner seeks the next allocated extent in the ordered index. It advances
past sparse address ranges and unlinked or pending inodes without a full
directory or object inventory. Each sampling opportunity reads at most one
extent and its dependency. The controller keeps fingerprints for one scan
window and a bounded set of representatives from older windows. Eight aligned
bands, up to four offset-independent features and four masked signatures
nominate candidates. Offset-independent features pair minima from distinct
fingerprint buckets over 16-byte windows, skipping trailing padding and zero
window fingerprints. The masked signatures each omit a different pair of
adjacent bit positions across sixteen distributed 64-byte samples. This can
find packed XOR candidates after changes throughout an extent. Constant
projections provide no candidate key. Live revisions, byte-exact readback
and the full packed-image bound govern publication.

New masked-XOR groups can choose their immutable base using the estimated
packed cost of differing planes across sampled tiles. CPU and AMD preparation
use the same nominated base. The indexed format and independent CPU reader
are unchanged. [Reference selection](AUTOMATIC_GROUP_MAINTENANCE.md#discovery-and-reuse)

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
fresh resource observations, foreground readiness and measured duty. A funded
advisory pair retains the scan cursor and representatives across owner
restart. The extended checkpoint fits up to 373 representatives within the
existing pair, subject to admitted memory, with a sparse tail for masked
signatures. The scanner revalidates same-image representatives after an
intervening edit and resamples changed extents. The paged coherence catalog
can find masked and modular matches outside checkpoint history using the same
live checks. Each compact posting costs 96 decoded bytes per stored signature.
Measured collective AMD benefit across workloads remains open.
[Restart validation](evidence/persistent-indexed-discovery-2026-11-18.json) ·
[Automatic indexed discovery](evidence/automatic-indexed-discovery-2026-11-18.json) ·
[Masked XOR discovery and restart](evidence/projected-xor-discovery-2026-11-19.json) ·
[Paged masked discovery](evidence/projected-coherence-catalog-2026-11-19.json)

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
