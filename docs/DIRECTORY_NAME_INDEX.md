# Ordered directory names and snapshot cursors (experimental)

Native path resolution now uses a maintained ordered `(parent inode, canonical
name)` map rather than scanning every inode. Enumeration walks that directory's
key range without sorting a global scan. This applies to all native layouts.
The map is rebuilt and checked against the complete inode graph on admission;
it is still volume-sized within the existing 256-node, 255-data-extent and
64-KiB normalized metadata limits.

The explicit `NativeFilesystemWriter::initialize_indexed_names` factory adds a
persisted index for each directory on a new empty paged store. Snapshot path
lookup reads the directory object and uses binary search. Inode information and
extent maps still use the complete admitted Model. Each directory index is a
whole object, not a split name B+tree or bounded page cache. No million-file,
performance, compression or release claim follows.

## Encoding and compatibility

This is capability **2** of the 64-byte `FLTXPN01` root described in
[inode-record persistence](INODE_RECORD_NAMESPACE.md). Other root offsets and
format-7 inode bodies remain unchanged. Capability 1, ordinary initialization
and namespace formats 1..7 retain their encoding. Old readers reject capability
2; there is no implicit conversion or implemented multi-release updater.

Object 1 is the root. Inode objects remain `(1ULL << 63) | inode`. A directory
index is `(1ULL << 62) | directory_inode`; indexed-layout inode IDs are positive
and below `1ULL << 62`, which is allowed only as the exhausted next-ID sentinel.
Data IDs remain 2..256. Every directory, including an empty or detached directory,
has exactly one index object until its inode is retired.

All integers are little-endian:

| Offset | Bytes | Field |
| --- | ---: | --- |
| 0 | 8 | `FLTXDN01` |
| 8 | 4 | Directory-body capability 1 |
| 12 | 4 | Entry count |
| 16 | 8 | Owning directory inode |
| 24 onward | Variable | Entries in strictly increasing canonical-name order |

Each entry has an 8-byte child inode, 2-byte key length, 2 zero bytes, then the
canonical lowercase ASCII key. Existing conservative Windows component rules
apply; this does not introduce full Unicode or NTFS namespace semantics.
Linked delete-pending children stay indexed until unlink. Detached children
have no entry. On open, exact canonical bytes generated from the independently
validated graph must match each index. Lengths, counts, references, ownership,
reserved bytes, ordering, boundaries and exact inventory must agree. Snapshot
index reads recheck that match. Reads never modify or repair an image or start HIP.

## Atomic updates and resource bounds

Fixed root, changed inode frames, directory indices and file data use one COW
paged transaction with the existing three barriers. Cross-parent rename updates
both parent indices together. Moving a folder preserves descendant identities
and its unchanged child index. Case-only rename changes the inode's display
name without rewriting identical canonical names. Data-only writes leave name
indices untouched; retirement removes obsolete index objects atomically.

Only the private native indexed-layout adapter admits 768 combined operations
and 4 MiB plus 64 KiB input. Capability 1 retains 512 operations / 4 MiB; public
paged commits retain 256 operations / 4 MiB. The extra allowance covers at most
256 directory headers and 255 bounded entries. User-operation, data, logical and
physical limits remain. Closed-context recovery can retire 255 distinct extents,
255 file inodes, the changed root inode, root directory index and fixed root
in one 513-operation publication.

Index/root pages retain paged metadata replication. Directory and inode object
payloads are integrity checked but **not independently replicated**. Digests
cannot reconstruct missing bytes. Protected payloads, native repair/migration,
incremental verification, paged inode/extent access, bounded residency and
automatic Windows factory selection remain separate work.

## Directory cursor API

`list_inode_page(directory, cursor, maximum)` accepts 1..1024 as the requested
page limit and returns canonical-name order with an optional opaque continuation.
An empty directory has no continuation. A cursor belongs to the exact immutable
snapshot instance and directory; moving the snapshot preserves it. Reopening
even the same root creates a new owner. Expired, foreign-snapshot and cross-
directory cursors reject before backing I/O. The weak owner does not keep a
retired snapshot or mutable writer alive. Writer mutations cannot change a
retained snapshot's enumeration. WinFsp pagination wiring remains unfinished.

## Validation scope

The shared operation/session/fault tests cover legacy paged, inode-record and
indexed-name layouts. Cases include canonical order, pagination, snapshot
lifetimes, rename/replacement, stable descendants, 14 malformed/foreign index
variants and maximum closed-session recovery. The indexed layout passed 27,795
checks with 429 injected write/barrier cases. The maximum recovery covers 28
cuts in three modeled failure modes. These memory tests do not qualify mounted
semantics, physical power loss, driver stability, optional codecs or GPU benefit.
[Source-bound execution evidence](evidence/directory-name-index-2026-11-15.json).

Current [Microsoft STL ordered-tree source](https://github.com/microsoft/STL/blob/main/stl/inc/xtree) was reviewed through its native
repository API (file blob `776279441110457c6be4cc7c2c57130e69597528`, fetched
15 November 2026). Its comparator-directed lower-bound traversal informs the
in-memory key range design, without establishing filesystem speed. No new
library, copied implementation or runtime dependency was introduced.
