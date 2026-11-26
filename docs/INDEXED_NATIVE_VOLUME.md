# Indexed native volumes

An indexed native image uses mirrored paged storage and reads inode, name and
extent records on demand. The native snapshot API provides lookup, directory
pages, metadata and file reads. The native writer can initialize a new mirrored
image and resume a fully verified indexed image. Its path transactions update
the checked index and changed records together. New volumes use capability 2,
which supports file lifetimes through `NativeFileSession`. Shared-base
publication selects capability 3 for aligned patches, 4 for anchor copies,
5 for packed XOR planes, 6 for modular deltas, 7 for sparse XOR planes,
8 for prefixed Zstandard or 9 for packed slices.
The managed head and writable WinFsp SDK adapter can select indexed images. The owned mounted
provider passed an earlier indexed external-client fixture on disposable
Windows Server 2025. Current Windows 11 client and mounted shared-base
qualification remain open.
[Managed indexed head](MANAGED_PAGED_VOLUMES.md#indexed-images) ·
[WinFsp indexed adapter](WINFSP_PAGED_VOLUMES.md#indexed-volumes)

## Root and keys

Object 1 is the little-endian native root. Capability 1 has 96 bytes.
Capability 2 has 120 bytes and appends three lifetime counts. Capability 3
has 136 bytes and adds the shared-base and member counts. Capabilities 4 through 9
keep the 136-byte root layout and permit additional group encodings. Readers
check the capability against its exact root length.

| Offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | `FLTXPV01` |
| 8 | 4 | Indexed native capability, 1 through 9 |
| 12 | 4 | Workload accounting flag, 0 or 1 |
| 16 | 8 | Native namespace sequence |
| 24, 32 | 8 each | Next inode ID and next extent ordinal |
| 40, 48, 56 | 8 each | Inode count, mapped extent count and charged extents |
| 64 | 8 | Committed input bytes |
| 72 | 16 | Exact paged store identity |
| 88 | 4 | Extent length 65,536 |
| 92 | 4 | Namespace features. Bit 0 selects Windows ordinal names. Bit 8 admits directory summaries. Bit 9 confirms a tracked root. Bit 10 permits inline short-file data in capability 2 and later. Other bits are reserved. |
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

### Inline short-file data

Fresh Windows memory volumes and freshly created dense or range-backed
volume files select `NativeIndexedDataLayout::inline_small_files`. The
checked root advertises the feature before a regular inode can contain inline
data. Reopening an existing volume retains its selected layout. An older
decoder with the previous namespace-feature mask refuses a root carrying
bit 10. Compaction and a group-capability update preserve the feature.

The canonical inode frame still has zero legacy extent entries. A
regular file of at most 512 bytes may store its data from offset zero
through the last nonzero byte after the name and descriptor. The
suffix length comes from the checked frame length and must fit within
the logical file size. The read path supplies any
remaining bytes up to EOF as zeros. Empty and all-zero files need no
inline suffix. Directory summary framing is unchanged.

Both copies of the inode protect its metadata and inline bytes.
The live extent count describes mapped external extents. An unreserved
file with an inline prefix still charges one logical extent. Explicit
allocation retains its existing charge. The inventory verifier rejects
an external extent owned by an inline inode.

A data change stages the first extent under the existing transaction
budget. If the file remains short, the transaction can move that
payload into the inode and remove the external record and key. Growth
beyond 512 bytes stages the preserved prefix as an external extent,
even when a sparse write starts past it. Resize, mapping edits and the
inode become visible through the same checked root. Pending delete and
detached handles retain the inode until its lifetime ends. Old
snapshots retain their previous representation.

An external Win32 client exercised the default on 374 held-out CPython
files, including growth, sparse reads, shrink, rename, enumeration and
remount. Independent CPU inspection verified both retired selections.
The dense and range volume-file creation paths also passed independent CPU
reopen across 372 modeled write and flush interruptions during growth and
shrink. Gradual migration of pre-existing extent-only roots is planned
separately.
[Named Windows qualification](evidence/windows-inline-named-and-adaptive-read-2026-11-25.json) ·
[Inline layout evidence](evidence/windows-inline-small-files-2026-11-25.json) ·
[Persistent creation and recovery model](evidence/windows-persistent-inline-reconciliation-2026-11-25.json) ·
[Release design](architecture/ADAPTIVE_STORAGE_EXECUTION.md#protected-short-files-and-complete-size)

### Packed inode pages

An explicit indexed layout, `packed_small_file_inodes`, adds packed inode
pages to the inline-file format. Namespace feature bit 11 advertises
the representation and requires bit 10. The current decoder can open
earlier roots. A decoder whose feature mask predates bit 11 refuses the
new root. Compaction keeps both features in the rebound root.

A linked regular file of at most 512 logical bytes is eligible if its
canonical inode frame fits in 1,024 bytes. Its stable inode ID selects
one of eight slots. The paged object ID is
`(6ULL << 61) | (inode / 8 + 1)`, and the slot is `inode % 8`.
The root inode, directories, larger frames and active delete lifetimes
use their standalone records. File lookups can fall back from an empty
slot to a standalone inode. Path traversal looks up a parent in standalone
storage first and still resolves a packed regular file in that position.

A page starts with the eight bytes `FLTXIP01`, followed by eight
little-endian 16-bit row lengths. Zero means absent. Nonempty rows follow
the table in slot order with no padding. Each row is an unchanged
canonical inode frame with its own decoded identity. No row may address
a reserved inode or an ID beyond the selected allocator. The maximum
decoded page size is 8,216 bytes. An empty page is removed.

The paged object store independently checks both mirrored records. Changes
to a page and any standalone representation share the existing transaction
with names, extents and lifetime markers. A split or merge therefore
selects exactly one representation for the inode. Inventory verification
checks all rows, rejects duplicate representations and counts namespace
and allocation ownership. It retains one checked page during the name walk
and still verifies the stored copies in the inventory pass.

Packed pages can save the per-record and index overhead of short inodes,
at the cost of reading or rewriting neighboring rows. Btrfs leaf items
illustrate dense metadata slots, and EROFS describes inline data in inode
layouts. FlitzFS uses its own checked object and publication boundaries.
[Btrfs on-disk format](https://btrfs.readthedocs.io/en/latest/dev/On-disk-format.html) ·
[EROFS on-disk format](https://erofs.docs.kernel.org/en/latest/ondisk/core_ondisk.html)

The 192-file short CPython cohort occupied 88 KiB including the selector,
down from 176 KiB for compact inline files with standalone inodes. The
live object count went from 199 to 32. The 374-file named tree went from
672 KiB to 464 KiB. Its full-file CPU reads fetched 1.9% more backing
bytes, and ingestion wrote 40 KiB more. The short-file range trace
fetched 57.0% fewer backing bytes. Both configurations kept the same
protection and independently verified file contents.

The grow and shrink tests recovered the prior or complete generation
in all 66 modeled interruptions. The Windows SDK callbacks also passed
on 4 KiB and 64 KiB modeled failure units. The layout remains explicit
while ordinary mounted I/O and gradual migration are qualified.
[Footprint, read costs and recovery](evidence/windows-packed-inodes-and-radeon-2026-11-25.json)

### Incremental packing of existing inodes

`advance_packed_inode_migration` accepts a cursor and an upper bound on
backing writes. Each call considers at most eight standalone records in
one stable inode-page range. It can skip sparse ID ranges through the
ordered object index. At least two eligible standalone rows are needed
to stage a conversion. Large records, directory records and active
lifetimes retain their direct encoding.

The publication uses the existing mirroring and readback checks. It
copies exact canonical inode frames into the page, removes the standalone
records and sets feature bit 11 in the same transaction. The namespace
sequence, inode IDs, names, attributes and content remain valid. Readers
already holding the older storage root retain its original records.

The paged writer prices the append span and both root writes before
permitting any writes or flushes. A denied grant or preflight capacity
refusal can retry the same cursor. A failed publication fences the
writer and modeled recovery selects a complete prior or new root.
After resumption, rediscover candidates against the selected checkpoint.

This entry point is explicit. The write sweep appends data. Occupied
bytes drop when the existing reclamation path settles the superseded
records. Bind future automatic offers to measured release, write
credit and verified compatibility.
[Windows migration and fault evidence](evidence/windows-packed-inode-migration-2026-11-25.json)

### Packing during staged reclaim

The explicit `pack_short_inodes` mode extends staged reclaim for a
mirrored, inline-capable image with standalone inodes. During source
verification it examines one inode-page range at a time and prices a
normal compacted image and the packed candidate with the same mirrored
geometry. The selector can adopt the packed layout only when its planned
allocation improves both the ordinary reclaim copy and the occupied
source. A tie or a sweep with no eligible groups uses ordinary reclaim.

The copy streams original records in key order, preserving the live
identity and generation of every retained record. It skips the priced
standalone rows and emits their exact canonical bytes in the generated
page range. The paged writer reconciles removed and added inventories
against the price and verifies the complete target, including its
mirrors, before and after private root publication. The selected root
activates bit 11 with the packed pages. Source readers hold their exact
prior view throughout. The original image receives no writes.

On selector recovery, a CPU verifier compares retained objects and
reconstructs the expected packed pages from the original inode frames.
It checks every expected target key, generation and canonical byte
stream and accounts for the complete inventory. A recovered selector
can therefore name either complete layout. The design follows the same
relocation principle of retaining logical ownership, and treats a
selected capability as a reader requirement.
[Btrfs relocation](https://btrfs.readthedocs.io/en/latest/btrfs-balance.html) ·
[OpenZFS feature activation](https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Pool%20Structure/Feature%20Flags.html)

The 374-file CPython control settled at 672 KiB and the packed reclaim
at 464 KiB, including the selector. Target writes fell by 212,992
bytes. Checked source-page reuse reduced the candidate's additional
reclaim read cost from 7,376,671 to 757,535 bytes relative to the
ordinary copy. A complete CPU read of the settled files fetched 1.9%
more backing bytes. The two layouts produced identical file contents.
[Copy, recovery and allocation evidence](evidence/windows-packed-reclaim-2026-11-25.json)

#### Packing an owned volume-file range

The file-owned reclaimer accepts this mode on an already reclaimable,
direct inline image. It uses the existing owned spare range, mirrored
selector, and reader-retirement checks. Capacity repair and sealed
images keep their existing paths. A packing attempt binds the exact
checkpoint. After a stale attempt, later copies for that selected image
in the session use foreground reconciliation. The next selected image
can receive a new packing offer. The caller opts in.

At 64 KiB modeled failure geometry, normal and packed CPython copies
settled at 1,024 KiB and 896 KiB including their selectors. The same
wide geometry in the owned-range fixture recovered both complete
layouts across 12 modeled selector cuts. A captured reader held the
previous range and a live handle continued serving the current bytes.
The stale attempt left its spare untouched and a later reconciled
handoff carried two newer foreground writes.

[Owned range and named corpus evidence](evidence/windows-owned-packed-reclaim-2026-11-25.json)

### Name profiles

Profile 0 keeps the earlier ASCII spelling and lowercase keys. The first
successful transaction that needs extended names or paths selects profile 1
in the same mirrored publication as the affected inode and index records.
Readers before profile 1 reject the new value. Current CPU readers support both,
and compaction carries the selected profile forward.

Profile 1 stores validated UTF-8 spelling separately from each canonical key.
Case aliases use the ordinal equivalence measured on Windows 11 build 26300.
ASCII keeps its original lowercase keys. The remaining equivalent BMP characters
share a pinned representative. Supplementary characters retain their code
points. Canonically equivalent Unicode sequences can be distinct Windows names.
Changing these equivalences requires a new profile and index migration.
[Windows ordinal comparison](https://learn.microsoft.com/en-us/windows/win32/api/stringapiset/nf-stringapiset-comparestringordinal)

Each component can occupy 255 UTF-16 code units and the relative path can
occupy 1,022. This leaves room for the leading separator and terminator in the
WinFsp request. The indexed namespace currently admits 16 components. It
rejects malformed Unicode, path traversal, reserved device names, forbidden
characters, and trailing spaces or periods.
[Microsoft naming rules](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)

The Windows SDK callback and CPU recovery tests exercise both profiles, display
spelling, alias collisions, the path boundary, compaction and interrupted
publication.
[Name profile validation](evidence/windows-indexed-name-profile-2026-11-20.json)

The inode body uses the canonical format-7 header, stored spelling and optional
security descriptor. Extent locations live in the index. Capability 1 requires
linked inodes. Capability 2 uses the canonical pending and detached inode flags
and a checked lifetime marker. Lookup checks the name key against the referenced inode. A data object
starts with `FLTXPE01`, the owner inode ID and the logical block number. The
remainder is a nonempty byte prefix of that extent with trailing zero bytes
omitted. Readers check the owner, range and frame before copying. Missing
extents and omitted tails read as zeros. Data objects retain the paged store's
independent CPU decoders and mirrored-payload verification.

### Directory summaries

New indexed directories carry exact maximum descendant path lengths and
component counts. Each axis is a histogram of the bounds contributed by
immediate child inodes. Counts preserve maxima when any number of children
share the longest or deepest path. Only changes to a directory's maximum have
to propagate to its parent.

Bit 8 of the namespace feature field admits an optional suffix on a directory
inode. The suffix follows the name and security descriptor. It begins with
two little-endian 16-bit bucket counts, followed by the length buckets and then
the component-count buckets. A bucket stores a little-endian 16-bit bound and
a 64-bit child count. Bounds appear in ascending order with positive counts.
Lengths count UTF-16 units including internal separators, starting immediately
below the directory. Empty tracked directories have two zero bucket counts.
The two axes have the same total number of children.

An indexed transaction publishes changes to the histograms with the name and
inode records. At move time, the writer adds the proposed prefix to the stored
maxima. Older directory records without a summary use a checked traversal.
Moving an older subtree into a tracked ancestor clears the affected summaries
in the same transaction. Older readers reject the feature bit. Existing images
with the bit clear remain readable.

Full verification visits the name tree from the root in pages. It checks
parent and name bindings, actual path bounds and each present histogram. The
number of reachable inodes must match the linked inventory. Independent CPU
reopen and compaction preserve the same verified namespace.
[Directory move and recovery evidence](evidence/indexed-directory-bounds-2026-11-20.json)

CephFS permits quota counters to lag actual writes. Btrfs documents a full
rescan when adding quota accounting to an existing subvolume. FlitzFS needs
exact counts at commit time because the path check determines whether a file
remains reachable after a move. [CephFS quotas](https://docs.ceph.com/en/latest/cephfs/quota/) ·
[Btrfs quota groups](https://btrfs.readthedocs.io/en/latest/Qgroups.html)

### Automatic backfill

The WinFsp owner fills missing summaries during low-priority opportunities.
Each scan step reads at most 64 immediate children from the ordered index.
When a child directory needs a summary, the scan finishes that child first.
The scanner checks the active directory's revision before resuming a scan or
publishing its summary. A relevant edit restarts that directory's scan.

A completed summary is an atomic progress checkpoint. Reopen can use it
directly while scanning the parent. Partial histograms and open page cursors
live in the writer and restart after a process exit. The transaction that
completes the root sets bit 9. Pending work then clears without a backing read.
Before admitting a marked root, the decoder requires the root inode summary.
Full verification still checks it against the reachable children.

A capacity refusal leaves the selected image writable and suspends backfill
for that namespace sequence. A new checkpoint or image owner can resume it.
Uncertain publication fences the writer and follows the CPU recovery path.
The scanner fits the existing host admission, maintenance pacing and group
ownership. [Controller contract](WINFSP_OWNED_CONTROLLER.md) ·
[Backfill validation](evidence/indexed-directory-backfill-2026-11-20.json)

Btrfs scrub uses a saved position to resume background verification. FlitzFS
uses completed directory records as progress checkpoints for this upgrade.
[Btrfs scrub](https://btrfs.readthedocs.io/en/latest/btrfs-scrub.html)

## Shared-base extents

Capabilities 3 through 9 keep the ordinary extent frame. A delta member has
the same 24-byte owner and logical-block header with magic `FLTXPD01`,
followed by the existing checked [shared-base delta](SHARED_BASE_GROUPS.md#exact-member-bytes).
A base object uses the data-object allocator and starts with `FLTXPA01`,
followed by 65,536 immutable bytes. Both records use mirrored storage.
The member binds the base object ID, generation and decoded digests.
The index must also contain that exact base/member pair.

An indexed read checks each member's index binding and the expected base
generation. It validates the base digest, decoded digest and file bounds
before returning the requested range. Independent snapshots can reuse a
verified base within one request. Serialized reads through an owned native
file session can retain several verified bases within the memory budget of
one native read. Reuse requires the same healthy, mirrored store identity
and checkpoint. Observed replica degradation or a checkpoint change clears
the retained bases before the next reuse.

Mutations and recovery verify the backing records independently. A complete
inventory audit can reuse decoded bases from its own bounded cache for
member verification. Entries bind the exact base object and generation.
The mirrored record walk reads both copies directly, including bases
already used by members. Each audit releases its cache at exit. Bases have
their own protected records, so removing the file used as the base leaves
other files readable.

The compacted two-group fixture interleaves 16 files. Each full audit read
467,550 backing bytes, down from 1,386,958. Fault checks reported a damaged
anchor peer, verified the surviving copy, and refused a write when both
copies failed.
[Grouped audit evidence](evidence/indexed-grouped-audit-2026-11-20.json)
[Public revision reads and mirror recovery](evidence/public-revision-reads-2026-11-19.json) ·
[Retained base read evidence](evidence/indexed-retained-anchor-reads-2026-11-20.json)

Indexed range reads through independent snapshots and the native file session
reuse verified paged lookup nodes and ordered-index records within each
request. The ordered index can keep four checked records of up to 4 KiB each
and validates each parent binding again when it parses a reused record.
The query releases this scratch on return or error. File and group member
checksums, first-access backing checks and verified-survivor reads follow the
same paths on each request. Scrub and recovery read backing independently.
This changes no stored bytes or required capabilities.
[Scoped read evidence](evidence/indexed-scoped-read-metadata-2026-11-20.json)

Explicit grouping prepares independently checked member frames one extent at
a time from a pinned checkpoint. Foreground edits can proceed between steps.
Publication rechecks selected indexed bindings and compares complete packed
layouts from the checked object totals and changed lookup paths. The bound
charges replacement data at its encoded size and index pages at raw size.
Publication requires a strict reduction. A declined candidate performs no storage writes. An existing
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
selected decoded extent and immutable base. It extends matching bytes
around each verified range and charges the resulting copy frame against
the size limit. The hint validation still covers ranges absorbed by an
earlier extension. Valid hints compete with the CPU encodings. The chosen
frame passes complete CPU decoding and the existing
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

### Assembled section references

A fully staged new candidate can retain its verified extents and try an
alternate base after preparing the ordinary encoding. An AMD search can
supply the same immutable input window. When a candidate admits a subset,
only the retained members contribute to section sampling, encoding and
pricing. A rejected member keeps its original independent record and
revision. The residual payload test uses the number of retained members
and the provider's failure unit.
It samples one extent per opportunity, finds exact shared sections at
content-dependent boundaries, and ranks their decoded bytes by reuse across
members. Retaining source order among the selected sections preserves
opportunities for longer copy runs. The base and its independently decoded
member frames use the existing capability 4 representation.

The writer seals both proposals against the same live root and verifies that
they affect the same objects. It chooses the smaller protected compacted layout,
using lower sealed payload bytes to resolve alignment ties. The chosen layout
must shrink the current image before any writes are made. An assembled base
has its own protected records and survives removal of any member while
other members still depend on it.
Successful publication supplies the extent of a linked member as the
representative for later discovery and reuse.
[Assembly validation](evidence/indexed-section-references-2026-11-20.json)

In the mixed fixture, eight related extents and one rejected extent
settled at 560 KiB including the selector. Refinement of the retained
members reduced the matched 752 KiB control by 192 KiB. The
compacted CPU reader verified every extent and sampled range. Grouped
and cold-rewrite sizes matched the controls on the four-release CPython
and AITDCC held-out corpora.
[Subset and holdout evidence](evidence/windows-subset-reference-refinement-2026-11-26.json) ·
[Related-data grouping at FAST 2026](https://www.usenix.org/conference/fast26/presentation/huang)

Later scans can extend the same group. The scanner uses the logical bytes of
a representative member to find related extents. It can nominate the
published representative immediately or use the indexed reverse references
after reopening and reclaiming a deleted member. Preparation then verifies
the current member, its indexed reference and its base generation. Encoding
uses the protected base bytes. Each new dependent still passes the existing
compacted-size comparison before publication.

In the 4 KiB failure-unit fixture, an extension after reopening reduced the
complete compacted image from 520 KiB to 392 KiB. All nine remaining files
reopened byte-for-byte through the independent CPU reader. The same native
build also verified live AMD results on the public source and documentation
revision corpora.
[Reuse and readback evidence](evidence/indexed-assembled-base-reuse-2026-11-20.json)

Recent work on resemblance grouping and shifted content informed the choice to
keep similarity sampling separate from stored byte layout.
[RubikFS at FAST ’26](https://www.usenix.org/conference/fast26/presentation/huang) ·
[ParaSync at FAST ’26](https://www.usenix.org/conference/fast26/presentation/zhang-zhihao-parasync)

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

#### Direct AMD sparse encoding

The sparse window entry with private ABI 7 counts the set bits in each
tile plane using 32-lane subgroups. The kernel prices all tiles before
writing within the admitted member slot. Prefix counts give each segment
separate positions in the ordered sparse lists. The worker sends only
the used bodies in the compact reply, with the request and group digests.
Private ABI 8 shares the tile encoder and places the output in aligned
ranges within the admitted slab. The worker validates the directory
before downloading the used payload. An authenticated module with the
ABI 7 entry supports the same operation through fixed-slot readback.
The Windows controller requires a dispatch grant, live adapter headroom
and compatible thread, shuffle and shared-memory limits. ABI 8 also
requires global 32-bit device atomics.
[HIP device language extensions](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_cpp_language_extensions.html)

The controller consumes a reply after confirmed worker and supervisor
retirement. The CPU checks the sparse grammar and reconstructs every byte
against the owned input. It rejects malformed candidates, honors the
exclusive size bound and keeps the dense form on equal encoded cost.
Missing or refused device results leave CPU encoding available.
[Direct AMD encoding evidence](evidence/amd-direct-sparse-xor-2026-11-19.json) ·
[Device readback evidence](evidence/amd-sparse-device-readback-2026-11-19.json)

The mixed AMD route evaluates sparse XOR and modular delta for each
staged extent in a shared dispatch. Advice can include both codecs in one
batch. CPU verification binds each winning candidate to the same owned
base and member bytes. A protected sparse result uses capability 7 and a
modular result uses its existing capability 6 representation.
[Mixed worker protocol](WORKER_CHANNEL.md#mixed-codec-batch)

### Prefixed Zstandard members

Capability 8 admits the `FLTXGM06` group member frame. It retains the 128-byte
member header, exact base object and generation, base digest and decoded digest.
The count field at offset 96 is zero. Its body is one checksummed Zstandard frame
for exactly 65,536 bytes. The shared base acts as the raw prefix. The decoder
requires the matching base, rejects trailing bytes and dictionary IDs, bounds
the declared window to 131,072 bytes and verifies the decoded member digest.
The CPU reader for these members requires the linked Zstandard codec.
[Zstandard prefix API](https://facebook.github.io/zstd/doc/api_manual_latest.html)

When an eligible complete group leaves enough residual data for a different
representation to matter, idle preparation can try prefix encoding one
member at a time. Candidates that reduce the member frame size are decoded
independently and compared with the original bytes. The complete transaction
then compares the ordinary reference, the assembled reference and the prefix
variant under the actual store codec. It orders them by the complete compacted
size bound, followed by stored payload bytes on ties. Publication uses the
existing membership, source-generation and mirrored commit checks.

The new member uses the existing shared dependency and reverse reference.
Removing a file, restoring a checkpoint or compacting the image keeps the
base available to every surviving member. Groups can still store older
member methods alongside prefixed members.
[Prefixed group validation](evidence/indexed-prefixed-zstd-2026-11-20.json)

An already staged AMD group can compare section assembly and prefixed
Zstandard using its verified window. The live scratch grant includes any
simultaneous packed-slice candidate and retained scan state. The ordinary
batch and discovery grants still cover the work while optional candidates
compete. If that additional grant contracts, maintenance drops the private
alternate and can publish the verified ordinary group. The complete layout
comparison includes backing pages and the existing mirrored publication.
[Staged alternative validation](evidence/indexed-staged-reference-choice-2026-11-20.json)

### Packed slices

Capability 9 adds the 80-byte `FLTXPD02` member. The base remains a protected
`FLTXPA01` object with a 65,536-byte decoded payload. All integers are
little-endian.

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 0 | 8 | Magic `FLTXPD02` |
| 8, 16 | 8 each | Owning inode and logical extent index |
| 24, 32 | 8 each | Base object ID and exact generation |
| 40, 44 | 4 each | Byte offset in the base payload and positive slice length |
| 48 | 32 | BLAKE3 of the exact decoded slice |

The CPU reader checks the owner, bounds, reverse reference, protected base
generation and slice digest before returning file bytes. The slice ends at
its last nonzero byte and fits within the logical file size. Existing sparse
read semantics supply any trailing zeroes. A random read of one member needs
at most one base. It can reuse that base under the existing verified cache
rules and verifies the member slice without reconstructing a padded extent.

Idle maintenance can try packing the retained members of a complete staged
new group if at least two canonical payloads fit in one base and live headroom
covers the additional candidate. A Zstandard-enabled build enables this
comparison. Rejected extents occupy no space in the base and keep their
current indexed bindings. Each preparation step copies one retained payload
or validates one proposed slice against the original.
The same candidate can compete in a CPU or AMD search window. The writer
prices the complete compacted layout against ordinary, assembled and prefix
variants before writing. Publication requires a strict size reduction from
the selected image and raises the root capability in the same mirrored COW
transaction as the base and members. The existing dependency lifetime and
reader-retirement rules apply.
[Mixed-group comparison](evidence/windows-packed-shared-subsets-2026-11-26.json)

In automatic compression mode, the indexed scanner also tries short
standalone payloads left in its current window after similarity and catalog
lookup. It starts in object order and looks ahead within the admitted window
for payloads that fit in the remaining space. A skipped extent can seed the
next group. Each selected extent belongs to at most one direct packing trial
in that window. The trial reads one checked source extent per idle opportunity
and retains the 64 KiB decode limit. Each slice uses its canonical length and
preserves the file's logical zero tail. Work reserved for an AMD batch keeps
its own source bindings. The controller settles the batch before closing that
scan window.

Persistent coherence keeps a preferred rolling key per admitted
representative. A query probes its preferred key and remaining distinct
rolling bands after the preceding key families. These are stable band
positions. A shift plus a local edit can preserve the older key in a
different position from the query's preferred choice. Empty catalogs
skip lookups. Each admitted advance examines at most one checked key or
alternate while the existing live extent, CPU and protected-size checks
govern publication. No additional posting or reader capability is needed.
[Restart discovery evidence](evidence/release-reconciliation-rolling-lookups-2026-11-25.json)

When a seed has no fitting partner in the current window, the scanner looks
up retained representatives by rolling fingerprint and checks whether their
payloads fit. It builds sorted postings once on demand for the admitted
window. Fingerprints come from the existing rolling scan and fold into the
same coarse features used by catalog discovery. An exact-key lookup examines
matching postings. The count of shared fingerprints ranks fitting candidates,
with larger payloads preferred on ties. A size lookup supplies the largest
fitting payload when there is no fingerprint match.

The packer checks the chosen object's live revision before preparing the
group, drops stale nominations and verifies the selected bytes before
publication. The postings use the existing representative budget and leave
scope with the scan window. No additional catalog bytes or decoding
dependencies are needed. [Retained similarity and read-cost comparison](evidence/indexed-retained-packing-similarity-2026-11-22.json)

Direct packing uses the existing compacted-size comparison and transaction.
It renews the memory grant during preparation and checks the selected
revisions before publication. A later scan can consider each packed slice
for a stronger similarity match, with the same size and revision checks.
[Direct packing evidence](evidence/indexed-direct-short-packing-2026-11-20.json) ·
[Uneven short-extent evidence](evidence/indexed-uneven-short-packing-2026-11-21.json) ·
[Cross-window packing evidence](evidence/indexed-cross-window-packing-2026-11-21.json)

The current reader supports capabilities 1 through 9. Readers capped at 8
reject a capability 9 root. An earlier image retains its capability when opened.
A published packed group can require that its base's paged codec
is available on CPU. The slice framing itself uses the normal CPU digest
and extent checks.
[Packed slice evidence](evidence/indexed-packed-small-extents-2026-11-20.json) ·
[EROFS shared fragments](https://docs.kernel.org/filesystems/erofs.html)

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

New masked-XOR groups can choose their immutable base using estimated
dense and sparse costs on sampled tile planes. CPU and AMD preparation
use the same nominated base. The writer independently reconstructs each
member and compares complete compacted size before publication.
[Reference selection](AUTOMATIC_GROUP_MAINTENANCE.md#discovery-and-reuse)

An admitted AMD batch can compare several staged references across every
tile in a complete new group. The chosen reference is an existing decoded
member of that group. The writer treats the original base as ordinary file
content during preparation and keeps the previously recorded source
identities for validation. The stored group uses the existing protected
base and sparse XOR formats. [Reference pricing protocol](WORKER_CHANNEL.md#full-tile-reference-pricing)

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

An immutable catalog view retains the checked root and reuses one branch
per level and four checked leaf pages for point and alternate lookups.
Publishing or reopening a new catalog creates a fresh view. Every positive
lookup still reads and verifies the posting's payload. The maintenance
controller still rechecks the live extent and protected size before
publishing a group. The lookup cache changes neither the catalog format
nor the permitted work per controller advance.
[Catalog lookup measurements](evidence/indexed-coherence-lookup-pages-2026-11-25.json)

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
deletion uses the retirement protocol below. Tracked directory moves validate
descendant bounds from their inode summaries. Older untracked subtrees can
still require a paged traversal.

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

### Checked file information on open handles

The writable indexed owner retains one bounded query cache for allocation
and file-info inspection. It holds checked lookup pages and small
namespace records. Reuse requires the same backing owner, tree and
checkpoint. A changed checkpoint or observed degradation discards the
cached proof. Operations still check the inode and its ancestry, and the
independent reopen and recovery paths verify backing directly.

On the 4,096-file Windows SDK fixture, 64 ordinary file-info callbacks
read 4,343,289 backing bytes and made 1,599 backing calls. The control used
7,717,324 bytes and 2,664 calls. Both checks used the same layout and
metadata. The updated fixture observed a changed timestamp immediately
and verified it after independent CPU reopen.
[Matched file-info evidence](evidence/windows-indexed-file-info-reuse-2026-11-25.json) ·
[WinFsp callback contract](https://winfsp.dev/doc/WinFsp-API-winfsp.h/)

### Checked metadata on ordinary reads

Indexed file reads use the same bounded query cache as allocation
queries. Its key includes the exact backing owner, root reference,
data and checkpoint generations, and geometry. Pending repair or
reported damage disables reuse. A prepared read with captured
metadata uses its own checked query scope. CPU data decoding and
independent recovery keep their existing byte checks.

For 32 adjacent 4 KiB callbacks over two grouped extents, CPU
backing reads decreased from 341,008 to 54,672 bytes. The prepared
control used 37,216 bytes. An adjacent warm CPU read fetched 1,163
backing bytes. After an acknowledged write, the next read fetched
and verified the changed bytes under the new checkpoint. These
are byte counts from Windows SDK callbacks with a memory provider.
[Read evidence](evidence/windows-foreground-query-reuse-2026-11-25.json) ·
[OpenZFS metadata caching](https://openzfs.github.io/openzfs-docs/Basic%20Concepts/Pool%20Structure/Caching.html) ·
[Btrfs tree generations](https://btrfs.readthedocs.io/en/latest/dev/On-disk-format.html)

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
Full scrub now streams the verified payloads through the native checks and
shares the bounded metadata cache for related lookups. It read 4,271,192
backing bytes on the matched fixture, down from 160,852,631. The low-level
walk independently checks both copies of each object. A regression check
bounds duplicate reads on this specific compact fixture.

A healthy paired writable reopen reads 23,036,690 backing bytes on the
five-MiB fixture, down from 42,381,786, with zero backing writes. The
pre-fence and post-fence audits each have their own verification state.
Writable reopen still traverses the full inventory, so this improvement
reduces I/O within each audit.
[Matched Windows evidence](evidence/indexed-reopen-single-pass-audit-2026-11-20.json)

### Stored replica reads after the fences

A healthy writable reopen reads both replicas again after the image and
selector fences. A checked leaf can supply adjacent frames from a paired
stream. The reader splits I/O at each bank boundary, then compares the
primary and peer bytes. It verifies each frame against its own checked
descriptor and stored digest. A mismatch or failed read marks the reopen
degraded even when separate reads can subsequently diagnose a valid copy.
A physical gap or the 128 KiB framing limit ends a coalesced run. Oversized
records use the individual replica path. The full decoded and namespace
audit still precedes the fences.
[Matched reopen measurements](evidence/indexed-fenced-read-runs-2026-11-25.json)

### Mirrored reads during the decoded audit

The first audit now uses the same checked mirror runs. The reader fetches
each bank, compares the complete frames between replicas and verifies every
frame against its descriptor and stored digest. Identical encoded frames
share one CPU decode. The native inventory receives each decoded object in
key order and still checks inode, namespace, extent and protection
relationships. If a run fails, each record follows the established survivor
path and the observation retains degraded status. A decode failure rejects
both identical copies. The fresh post-fence scan remains in force.

| Windows in-memory workload | Stored-only coalescing | Decoded and stored coalescing within banks |
| --- | ---: | ---: |
| 1,024 short files | 8,138 | 4,604 |
| 8,192 short files | 64,616 | 36,232 |
| 16,384 short files | 129,226 | 72,492 |
| 2,640 CPython files with automatic compression | 40,634 | 33,754 |

Both versions read the same number of bytes per audit, completed one image
flush and one selector flush, and issued no backing writes. Independent CPU
readback checked every file. The results count provider calls in the model.
A line fitted to the candidate's short-file endpoints predicted 36,285
calls for the held-out 8,192-file tier, within 0.15% of observation. The
evidence records a conditional larger projection with the observed layout
and locality. The full audit still grows with the object inventory.
[Decoded reopen measurements and verification](evidence/indexed-decoded-reopen-runs-2026-11-25.json)

Grouping across adjacent banks in both audits reduced backing calls
further from 72,492 to 60,452 on 16,384 short files, and from 33,754
to 21,564 on 2,640 compressed CPython files. The byte counts and flush
boundaries matched, and every file verified by CPU after reopen. The
earlier scaling fit describes its own layouts and revision.
[Bank-spanning verification evidence](evidence/windows-bank-spanning-verification-2026-11-25.json)

### Decoded name pages within an audit

Each native inventory audit keeps one decoded name-index branch per tree
level and four leaf slots. A hit checks the page ID, height, checked entry
count and parent key bounds. Child enumeration descends in height, so a
child cannot evict the branch in use by its parent. The independent object
walk still reads and verifies both replicas. The cache-only comparison
used separate caches for the complete native audits on either side of
the fences. The fault fixture exercises loss of a name page replica
at either fence and checks CPU readback after repair.
[Btrfs tree checks](https://btrfs.readthedocs.io/en/latest/Tree-checker.html) ·
[XFS live namespace checks](https://docs.kernel.org/filesystems/xfs/xfs-online-fsck-design.html#full-filesystem-scans)

| Workload | Control median | Cached median | Control backing reads | Cached backing reads |
| --- | ---: | ---: | ---: | ---: |
| 1,024 short files | 17.19 ms | 12.56 ms | 6,135,648 B | 6,090,808 B |
| 8,192 short files | 147.69 ms | 95.60 ms | 56,545,744 B | 51,878,488 B |
| 16,384 short files | 270.87 ms | 192.44 ms | 113,630,294 B | 104,139,778 B |
| 2,640 CPython standard-library files | 163.45 ms | 132.23 ms | 215,239,276 B | 214,349,092 B |

These runs use the indexed engine on Windows with a memory backing and
raw mirrored data. Each trial completed one image flush and one selector
flush, with zero startup writes. The CPU independently read every file
back. Sixteen additional single-file pairs had overlapping ranges. Their
medians were 547 µs for the control and 599 µs with the cache, with equal
read bytes.

The affine backing-read fit from 1,024, 4,096 and 8,192 short files is
approximately `6,386.12 × file_count − 400,758` bytes in the measured
range. It predicts the held-out 16,384-file result within 0.09%.
A payload term calibrated on 1,024 CPython files predicted the larger
nested CPython tree within 1.18%. The same short-file cost projected to
10 million and 30 million files is 63.9 GB and 191.6 GB of reads, subject
to the stated cost and layout assumptions. These are work projections.
Mounted timings require their own measurements.
[CPython 3.15.2 source](https://www.python.org/downloads/release/python-3152/) ·
[Paired results, model and manifest](evidence/indexed-audit-page-cache-2026-11-22.json)

The `flitzfs_native_indexed_volume_tests` executable accepts
`--profile-reopen-count N` and `--profile-cpython-reopen FIXTURE`.
A source-tree fixture begins with `FLSTREE1` and a 32-bit file count.
Each file has a 32-bit UTF-8 path length and a 32-bit logical byte count,
followed by the path and exact original bytes. Integer fields are
little-endian. Use the release archive and selection rules in the manifest
to regenerate the inputs. The timing surrounds writable reopen. The
independent CPU readback follows the timing.

### Count name pages in the verified object walk

The native audit validates each reachable name-index page against its parent
bounds. The same audit counts stored name pages as its full object walk
verifies both replicas. It checks each page ID against the index allocator
and requires every reachable page to be accounted for. The regression fixture
inserts an otherwise valid mirrored page outside the reachable tree and
requires the native audit to reject the volume.

On the Windows memory-backed short-file fixtures, the total backing reads
fell from 4,713,038 to 4,654,688 bytes at 1,024 files and from 76,462,421 to
76,085,219 bytes at 16,384 files. Both scales used one image flush and one
selector flush with zero reopen writes and exact independent CPU readback.
The paired timings were close. These samples establish fewer backing reads
for this audit. [Matched audit evidence](evidence/indexed-name-page-inventory-2026-11-23.json)

### Reuse checked extent owners during inventory

The namespace walk can retain the inode number, EOF and reservation state
needed to verify file extents. This scratch fits within the existing native
read budget and lasts for one audited snapshot. A missing owner record uses
the checked inode lookup, including for detached files. The decoded object
walk still reads both inode replicas. The stored mirror scan runs with fresh
reads after the image and selector flushes.

At 16,384 files, reads before the image flush fell from 51,509,020 to
39,081,516 backing bytes. Both runs verified 24,446,104 bytes after the
fences. The 4,096-file Windows SDK fixture independently verified a later
write and metadata update after reopening. Its pre-flush reads fell
from 14,361,437 to 11,047,773 bytes.
The 1,024-file and 8,192-file samples predict the held-out total at
16,384 files within 0.047% for this short-file layout.
[Matched audit and recovery evidence](evidence/windows-indexed-extent-owner-audit-2026-11-25.json) ·
[Microsoft file flush contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)

### Read checked sibling inodes together during reopen

The initial namespace audit can read neighboring primary inode frames in
one bounded span. The checked name walk advertises consecutive inode IDs
it will visit in order. The object store coalesces only physically adjacent
frames on one checked leaf within that range. It validates the stored
identity and digest of each frame before returning any inode from the
span. Each inode still goes through the ordinary CPU decoder and native
name, parent, lifetime and shape checks.

A failed span keeps the view degraded, then the ordinary single-record
reader can recover a surviving frame. The reader also uses that path if
it cannot allocate the optional scratch. The scratch ends at the namespace
pass. The decoded inventory still reads both copies before the fences. The reopened
checkpoint uses fresh stored reads on both copies after both fences.

| Matched Windows memory image | Baseline provider reads | Grouped provider reads |
| --- | ---: | ---: |
| 1,024 short files | 2,639 | 1,662 |
| 4,096 short files | 10,205 | 6,289 |
| 16,384 short files | 40,496 | 24,825 |
| 374 named CPython files | 1,291 | 1,057 |

The short-file tiers read the same bytes on both sides of the fences.
The CPython tree read 8,253 additional bytes before the first fence and
the same bytes afterward. An earlier attempt to read neighboring frames
without pending name demand added 700,815 bytes before the fence on that
tree. Both versions of the final paired comparisons performed one image
flush, one selector flush and zero writes. Independent CPU readback
matched every file. The endpoint slopes are 2.465 and 1.508 provider reads
per file in these layouts. The revised fit predicts the held-out
4,096-file count to within 0.09%. The audit still traverses the inventory.
[Paired counts and fault checks](evidence/windows-indexed-namespace-read-window-2026-11-25.json)

### Reuse complete inode replica runs within the decoded audit

The namespace walk can verify a complete mirrored inode run from one
checked object leaf. Every frame must fit the existing physical run and
be within the name walk's pending sibling range. Each decoded inode
passes the same name, parent, lifetime and native accounting checks.
The owner retains one paired window while nested directories use the
ordinary scratch. The subsequent object walk consumes the verified IDs
only at their full run boundary. It still traverses the checked tree
and checks every other object. Both replicas are read afresh after the
image and selector flushes.

The 16,384-file Windows memory image used 24,825 provider reads before
this change and 23,919 after it. Before the image flush it read
39,081,516 and 36,657,339 bytes. The raw 374-file CPython control recorded
1,578 provider reads and 2,289,163 pre-flush bytes in both arms. Each
pair had the same post-flush reads, one flush per backing and zero writes.
Independent CPU readback matched every file. Fault coverage includes a
damaged inode in either bank and a fresh peer failure after both flushes.
The first implementation allowed partial runs and increased calls on
the CPython tree. The current implementation requires a full run.
[Matched profiles and replica checks](evidence/windows-whole-run-inode-audit-2026-11-25.json)

Use read grouping to reduce the cost of the complete audit. XFS describes
readahead in its full metadata repair scans. Btrfs documents separate
validation of checked tree blocks. Those designs inform batching against
an already checked page. FlitzFS binds writable admission to its own
fresh replica checks, exclusive writer leases and exact checkpoint.
[XFS scan design](https://docs.kernel.org/filesystems/xfs/xfs-online-fsck-design.html#full-filesystem-scans) ·
[Btrfs tree checker](https://btrfs.readthedocs.io/en/latest/Tree-checker.html) ·
[Microsoft sharing modes](https://learn.microsoft.com/en-us/windows/desktop/FileIO/creating-and-opening-files)

### Stored mirror recheck on healthy writable reopen

The ordinary indexed reopen takes its initial decoded namespace audit from
a completed volume-head view. That audit checks the Windows metadata when
the attachment supplies an admission callback. The owner acquires both
leases and fences the image before the selector. It reopens the image and
compares the exact native root and paged checkpoint on the same backing.

Each object-tree reference binds the next page digest. Each leaf descriptor
binds the encoded bytes and the record header, including the codec and the
decoded digest. The second pass reads both copies of each reachable
object-tree page and live stored frame, then checks those bindings. The exact checkpoint
preserves the decoded results of the first audit. The pass rejects newly
degraded protection before the writer becomes available. The ordinary
recovery path handles damage found during the first audit. No verification
state is saved to the format.
[Microsoft file flush contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers) ·
[Microsoft file sharing](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) ·
[Digest tree validation](https://docs.kernel.org/filesystems/fsverity.html)

| Workload | Control median | Stored recheck median | Control image reads | Stored recheck image reads |
| --- | ---: | ---: | ---: | ---: |
| 16,384 short files | 197.21 ms | 139.27 ms | 104,139,778 B | 76,462,421 B |
| 2,640 CPython files, raw | 124.79 ms | 110.00 ms | 214,349,092 B | 206,129,738 B |
| Same CPython files, automatic compression | 187.28 ms | 113.77 ms | 67,489,504 B | 59,863,717 B |

The control included the decoded page cache from the preceding round. Three
alternating pairs per workload kept the same files, storage geometry and
pre-flush read counts. Both modes completed the two flushes with zero
startup writes. The CPU independently verified every file. The
`--profile-cpython-compressed-reopen` profile uses the same public input
as the raw profile and requires Zstandard support.

The new backing-read fit estimates `4,669.83 × file_count − 44,076` bytes
in the measured short-file range. It predicts the held-out 16,384-file
result within 0.006%. At 10 million and 30 million files it projects about
46.7 GB and 140.1 GB under comparable layout and per-file cost.
[Paired reopen, recovery and scaling evidence](evidence/indexed-fenced-stored-recheck-2026-11-22.json)

On the 16,384-file fixture the verified object tree has height three.
Lookups retain a branch at each admitted level and use four separate leaf
slots. Each cache belongs to its checked view or query. The mirror audit
still reads both copies directly. Full scrub read 64,978,691 backing bytes,
down from 98,443,011 for the same layout. Writable reopen read 130,327,966
backing bytes, down from 197,479,086, with zero backing writes. The test
recorded image reads on each side of its image flush and checked every
short file through the independent CPU reader.
[Scale and recovery evidence](evidence/indexed-scale-reopen-2026-11-20.json)

The inventory check now parses an inode directly from the payload already
verified by the mirrored object walk. The same inode lifetime check applies
to both this path and ordinary lookup. Consecutive extents owned by the
same inode can reuse one checked owner within this audit. The namespace walk
still checks reachability and the object walk still checks both stored copies.
On the 16,384-file fixture, full verification read 56,629,855 backing bytes
and the two-barrier writable reopen read 113,630,294 bytes with zero writes.
[Indexed audit evidence](evidence/indexed-audit-reuse-2026-11-20.json)

### Reuse packed inode pages during namespace verification

A namespace can visit packed inodes in a different order from their
creation. The audit retains checked pages by object ID during the name
walk. The cache sizes itself from the selected image's inode geometry
and shares the existing scratch allowance with the extent-owner list.
If allocation declines, the audit uses its single checked page.
The cache is released before the separate object walk, which still
verifies both replicas. Name bindings, inode inventory, extent owners
and the two writable fences remain required.

| Files | Name order | Prior reopen image reads | Candidate image reads |
| ---: | --- | ---: | ---: |
| 1,024 | Permuted | 2,335,733 B | 1,286,029 B |
| 4,096 | Creation order | 4,462,442 B | 4,462,442 B |
| 4,096 | Permuted | 41,493,848 B | 6,796,952 B |
| 8,192 | Permuted | 87,382,008 B | 20,311,320 B |
| 16,384 | Permuted | 177,986,000 B | 47,886,704 B |

Both arms verified every path, inode and payload through the CPU reader.
The post-fence read counts in bytes matched and reopen wrote zero bytes.
The 374-file CPython holdout also passed identical content and footprint
checks. These measurements cover the complete audit and expose the
effect of name order. A durable changed-page proof remains a separate
writable admission requirement.
[Paired writable reopen evidence](evidence/windows-packed-audit-locality-2026-11-26.json)
