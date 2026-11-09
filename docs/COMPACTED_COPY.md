# Verified compaction into a new volume

Experimental native storage primitive. It creates a NEW destination, never
rewrites the source, never truncates an incomplete destination, and never
renames/deletes a file. No CLI, mount, automatic background worker or GPU runtime
is enabled. All execution/qualification remains on disposable hosted VMs.

## Ownership and identities

ObjectStoreWriter::compact takes the latest recovered ObjectStoreSnapshot,
an empty qualified StoreByteIo target, a fresh nonzero StoreIdentity different
from the source, and the compression policy for later writes. The source writer
must have ended, including a poisoned writer's lifetime. The copy holds both
source and destination backend leases. A stale historical snapshot is rejected;
compaction cannot silently substitute an older acknowledged namespace.

Per-instance leases do not exclude different facades over the same storage.
Custom backends must qualify exclusive source stability and destination
ownership. Ordinary store readers now take a nonblocking shared POSIX flock on
the exact opened handle; cooperating exclusive file writers are excluded.
Windows read handles already exclude write/delete sharing. POSIX locks remain
advisory, and this is not hostile-process/directory isolation. Size admission is
refreshed after acquiring the shared reader lock.

There is no public path-based compaction/reopening factory. Hosted tests alone
supply freshly owned ordinary destination files through the internal adapter.
Creation remains CREATE_NEW/O_EXCL; a partial new output is retained on failure.

The destination is a new volume identity. This avoids two independently writable
branches claiming the same identity/generation after a copy. Source inode IDs,
namespace sequence and created/modified sequences remain unchanged in the native
file client. The old volume must remain read-only during handoff; cross-process
or global clone/fork prevention is not implemented.

## Copy and publication protocol

For a positive source root generation G:

1. Acquire the source lease, load the actual latest root/manifest and CPU-verify
   all reachable objects. Require exact agreement with the supplied snapshot.
2. Require a distinct empty destination, qualified durability contract, two
   available generation numbers and admitted memory/physical growth.
3. Lay out current live records contiguously from byte 131,072. Keep at most one
   record payload in copy scratch. Preserve codec IDs and stored payload bytes;
   update identity-bound headers, record digests and manifest references.
4. CPU-decode/verify all copied destination records before any root publication.
   Recheck the source's extent, latest metadata and full reachable content.
5. Write two manifests naming identical copied objects, at generations G+1 and
   G+2. Both roots name the same sealed 64 KiB-aligned physical end. Flush all
   dependencies before writing either root.
6. Publish the G+1 root in its parity slot and flush. Publish G+2 in the peer
   failure unit and flush. Reparse both roots and CPU-verify before returning
   the destination writer.

Initial empty generation-zero stores use ordinary new-store creation instead.
Positive empty stores retain the two-generation protocol. Generation exhaustion,
unknown headroom/contracts, unavailable source codecs, stale snapshots and
invalid identities fail; no source write or flush occurs.

Both catalogs are durable before the first root. Interruption before a valid
root may leave an unusable partial NEW destination; the source is still intact.
After the first root's successful flush, the complete baseline survives damage
confined to the peer root's admitted failure unit. Recovery may expose G+1 or
G+2 with identical contents, repair a missing root, and continue writing.
Success follows the final flush and independent readback.

The existing root/record encoding is unchanged. A generic object copy preserves
all logical object bytes and object generations. NativeFilesystemWriter::compact
must be used for native namespaces: it verifies the graph/data and explicitly
re-encodes object 1 with the new volume identity. All other native objects keep
their codec/payload/object generation. This metadata candidate passes the normal
private CPU compression round trip. Calling the generic object API does not
magically rebind client metadata.

## Space, resources and remaining handoff work

The existing 256-object, 16 MiB decoded-live, 1 MiB per-object and 64 MiB image
ceilings remain. The two catalogs occupy at most 32,896 bytes in total.
Ordinary-file admission retains 640 MiB physical AND commit headroom, a 1 GiB
free-space reserve and repeated admission during the record copy. This is a
bounded synchronous library operation, not a qualified low-priority background
executor or whole-system resource reservation.

The destination excludes obsolete records, orphan tails and old snapshots.
The source retains all of them, and source snapshots remain readable.
Destination size is counted including both catalogs, record headers and sealed
padding. Total storage temporarily includes BOTH volumes. No old source bytes
are physically reclaimed until separately qualified durable handoff and reader
retirement permit it; automatic filename replacement/deletion is absent.

The copy preserves existing encodings rather than promising a better ratio.
Future learning-driven recompression, placement and defragmentation need
transactionally checked candidates, resumable small work units, low scheduling
priority, resource/foreground yielding, persistent advisory learning state,
cancellation and lifecycle/reclamation proofs. See AUTONOMOUS_MAINTENANCE.md.

## Qualification

RAM tests cover raw/automatic modes, exact data and stored-payload parity,
source immutability, old snapshots, latest-snapshot admission, leases during
copy, low headroom/contracts/identity/count/generation limits, empty stores,
full 256-object manifests, continuation, source-change detection and every
observed destination write/flush failure under three abstract fault modes.
Native tests preserve inode/time-sequence/namespace semantics, damage either
identical-content compaction root, and then continue writing. After a later
different transaction, they damage only the obsolete root and require ALL
acknowledged bytes to survive. Silent target-data damage must be detected before
publishing a root.

Arbitrary media loss of the latest root after an ordinary, single-publication
transaction is outside this protocol's write-failure envelope: an older
surviving root may describe older contents. That must not be counted as verified
repair of all acknowledged history. A replicated current-commit/acknowledgement
design is still required for that stronger media-failure guarantee.

Hosted Linux/Windows file tests cover shared reader/exclusive writer exclusion,
new-file-only targets, compacted independent reopen and unchanged source bytes.
The process-interruption workflow adds native compaction at every reported I/O
boundary. A BLAKE3 digest captured at the acknowledged source baseline is passed
to a new recovery process. It must preserve exact source bytes, accept only a
complete published destination or retry into a distinct NEW path/identity, then
commit another file and reopen independently.

The controller admits at most 640 cases, 128 events per process, 256 bytes per
event, eight queued events, 15 seconds per process and 180 seconds total. The
combined owned image files stay within 2 MiB. Existing process memory/CPU limits
remain. These are process-termination and RAM fault experiments, not physical
power-cut, Windows filename-durability, writable mounted, background-priority or
AMD hardware qualification. Inspect exact-commit executed results.

## Replicated metadata extension

Positive compacted copies now publish root format field 2 as defined in
REPLICATED_CHECKPOINTS.md. The two catalogs occupy separate 64 KiB units rather
than adjacent bytes; a root binds its peer's exact same-state content. The
original protocol description above explains the earlier adjacent-catalog
checkpoint. Generation-zero empty copies retain ordinary creation. No source
write/delete or automatic handoff follows from the extension. All isolated
metadata units/padding count in destination size and process-fixture bounds.
