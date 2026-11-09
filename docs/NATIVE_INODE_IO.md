# Native inode I/O for the Windows writable engine

Experimental CPU engine APIs; no mounted-write permission or complete Windows
open-handle lifetime follows. The Windows product target remains latest stable
Windows 11 AMD64, with a separate helper application and driver-backed engine.
All builds and experiments remain on disposable hosted VMs.

## Why inode selection is required

A pathname can change during an open file's lifetime and can later identify
another file. A future Windows callback must not resolve its saved original
pathname and accidentally write a replacement. The existing native namespace
already persists monotonically allocated inode IDs. The new entry points select
that stable live ID within the volume's current acknowledged model.

NativeFilesystemSnapshot::stat_inode, list_inode and read_inode operate on the
snapshot's own namespace and immutable object references. They preserve the
same directory/type checks, sparse zero handling, bounded reads and private
integrity verification as their path-based counterparts. A missing ID,
including zero, is not_found. Directory enumeration remains case-key ordered.

An older snapshot can still read an inode deleted from a later namespace.
The current writer rejects a missing/retired inode; it does not resurrect it,
find an older pathname or redirect to a new same-name file. IDs are scoped to
the containing volume/snapshot. They are not globally authenticated handles.

## Atomic writes and accepted counts

NativeFilesystemWriter::write_inode accepts a live inode, offset, byte span and
NativeInodeWriteOptions. Its single-control-thread call selects the inode's
current state directly in the existing transaction staging and
replicated publication. No caller callback or external namespace operation
intervenes between selection and commit. This is reuse of the native engine,
not a second transaction or codec implementation.

- Ordinary writes select the supplied offset and preserve the existing size,
  sparse, extent-ownership, input and staging admission rules.
- Append selects the current acknowledged EOF inside the call and ignores the
  supplied offset. Consecutive calls use the EOF produced by the preceding
  successful commit. UINT64_MAX may therefore be supplied as the Windows append
  sentinel without performing overflowing offset arithmetic.
- Constrained writes never extend logical EOF. Input crossing EOF is clipped;
  a starting offset at/beyond EOF accepts zero bytes. Append plus constrained
  therefore accepts zero bytes. An arbitrary beyond-EOF offset cannot create
  a sparse gap in this mode.
- The FULL supplied span must be at most 1 MiB, even when most/all would be
  clipped. The wrapper owns at most one additional 1 MiB accepted-input copy.
  Existing 4 MiB transaction, 63 dirty buffers, 16 MiB decoded-live and 64 MiB
  physical-history caps remain unchanged.
- NativeInodeWriteCommit returns the selected inode, offset, transferred count
  and ordinary NativeFsCommit. The count is returned only after successful
  replicated publication. An exception returns no successful result; backing
  failure retains terminal/indeterminate writer semantics.
- Zero accepted bytes use the existing metadata-checked no-op path: no size,
  namespace sequence, payload or root publication changes. Nonempty identical
  writes still advance modified sequence and perform all three barriers.
- Result ownership/moves after publication are statically required not to
  allocate/throw. No post-commit pathname/snapshot lookup is needed to form
  the returned result.

truncate_inode selects the same current live identity and delegates to ordinary
atomic truncate staging. Shrink/regrow preserves zeroed tails. Directories and
the root cannot be written/truncated as regular files. Missing IDs and ordinary
admission errors preserve the healthy writer and perform no backing writes.

The writer and shared backend still require one controlling thread. These APIs
do not make concurrent writer calls safe. The future Windows engine must
serialize each full inode/append/commit operation; locking only EOF lookup would
not satisfy this contract. Caller byte spans must remain stable for the call.

## Current primary contract and deliberate limits

The selected WinFsp v2.2B5 API source at
14d6e0280ae15e2550c44d6dbc476be51649727f was fetched again and compared with the
previously reviewed exact bytes before this implementation:
https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h

Its Write contract requires current-EOF append and says constrained I/O must
not extend the file. Its Flush contract distinguishes per-file and whole-volume
flush after FSD cache writes. Cleanup is distinct from Close, cannot return a
failure status and may be followed by more I/O even for a deleting file.
CanDelete/SetDelete must not directly delete; delete-on-close bypasses those
callbacks. These are binding integration requirements, not properties proved
by this new engine API.

The later lifecycle capability supplies persistent intent/cancellation and
detached data retention as CPU primitives. The owning Windows engine still must
track actual open contexts, establish final Close, aggregate dispositions and
persist Windows ACLs/times; share/byte-range locking, truthful allocation and
writable callbacks remain unfinished. Mounted writes remain absent. See
NATIVE_FILE_LIFETIME.md for the explicit recovery/retirement preconditions.

The original inode I/O addition changed no encoding or historical mutation/commit
layout. The later explicit lifecycle capability is specified separately in
NATIVE_FILE_LIFETIME.md; ordinary format-1 bytes remain supported. Existing path mutation and snapshot behavior
remains regression coverage. The immutable write-reuse baseline test remains
unchanged and uses the original source-compatible types.

## Configured verification

The dedicated native inode suite runs raw and automatic compression. It checks
directory/file identity through ancestor and file rename, path reuse, replacement
and removal; retained snapshots; independent CPU reopen; consecutive appends
across extent boundaries; constrained clipping, no-op sentinel arithmetic,
sparse writes at the 1 TiB logical bound; unchanged-write barriers; input/ID/
type/moved-object rejection and zeroed truncate tails.

It records every write/flush of a cross-extent inode append, injects each of the
three existing persistence-failure modes at every observed boundary, requires a
terminal writer and retained acknowledged bytes, then independently recovers the
complete old or new append and continues at the recovered EOF. It never
acknowledges a partial append or retries a poisoned writer.

Existing native graph, 976-case hosted process campaign, metadata replica
recovery, CPU/reference/sanitizer, relocated binary and read-only mount lanes
remain required. New checks are configured until exact-commit CI is inspected.
RAM/process interruption is distinct from physical power-cut and writable
Windows application qualification. No owner-host execution is authorized.


Persistent intent and detached-inode state now have CPU transaction primitives
in NATIVE_FILE_LIFETIME.md. A detached inode remains live/addressable until
explicit retirement and can receive inode writes without a surrogate path.
An actually retired ID still fails rather than selecting a replacement.
These primitives do not supply the future Windows handle ownership/count proof.
