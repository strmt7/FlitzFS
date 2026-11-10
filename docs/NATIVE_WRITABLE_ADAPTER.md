# Native writable Windows adapter candidate (experimental)

This candidate combines the native transaction engine and actual WinFsp SDK
callbacks. It is built only with the opt-in Windows SDK target. Its factory
requires a disposable hosted environment and creates a NEW ordinary image with
OS-exclusive creation/writer ownership. It has no CLI, installer, service, live
provider selection or mounted-write executable. Default product paths and the
existing mounted probe remain read-only. No owner-PC operation follows.

## Current primary evidence

The selected WinFsp release remains v2.2B5 (13 October 2026), immutable source
`14d6e0280ae15e2550c44d6dbc476be51649727f`. Its native release API and source
bytes were reviewed on 10 November 2026. This change uses those actual bytes,
rather than historical API assumptions:

- [Kernel disposition](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/fileinfo.c):
  successful cancellation clears the file node's global pending state. An
  aggregate of per-open explicit intents would incorrectly retain deletion.
- [Cleanup](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/sys/file.c):
  an unprocessed initial per-FD delete-on-close option can arm pending state at
  its later Cleanup. Cancelling current state does not erase that option.
- [SDK dispatch](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/src/dll/fsop.c):
  Open receives the full CreateOptions, including the high disposition byte;
  Overwrite can close a failed prepared open without Cleanup. The final access
  mask can exclude already-checked implicit overwrite/supersede rights.
- [Callback contract](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/inc/winfsp/winfsp.h)
  and [memory reference](https://github.com/winfsp/winfsp/blob/14d6e0280ae15e2550c44d6dbc476be51649727f/tst/memfs/memfs.cpp):
  basic attributes INVALID_FILE_ATTRIBUTES and times zero mean unchanged.
  Reference no-op Flush and detached slow-I/O patterns are not adopted.

The source-routing receipt for the base commit `7de69ce` used six real hosted
queries, job 129734480112, source SHA-256
`9c23568c77c3538d2c2fe43375c5ad8116a78c75158fa34b6658a363a89b21fa`.
Candidate paths and all changes were verified against native immutable source.
This is a base-source navigation receipt, not an index receipt for new drafts.

## Ownership, callbacks and offlining

One mutex owns every callback interaction with the single-controller engine.
The adapter has 256 bounded monotonic opaque context tokens; no raw pointer
context can rebind after Close. Capacity is checked before creation. An owned
metadata/security lookup admits no new context/grant. The root receives actual
UTC metadata and a protected inheritable descriptor for the process owner in
one transaction. Regular creation sets the source-qualified archive attribute. SDK/FSD access
checks and inherited descriptors remain required;
the adapter does not invent access authority.

Create, Open, Overwrite, private verified Read, synchronous Write, size,
GetFileInfo, real Flush, basic metadata, rename, deletion, security read,
directory enumeration and volume quota are implemented in this candidate.
Queries support provisional contexts. The first trusted data/metadata/Cleanup
callback explicitly adopts a prepared context. This transition is NOT kernel
adoption attestation. A premature active Close without Cleanup fences rather
than silently discarding ownership. Actual cancellation/cache/early-paging
delivery still requires bounded real-kernel qualification before promotion.

Windows global deletion is an explicit NativeDispositionPolicy; the historical
aggregate policy remains the default. A successful global cancel clears all
current explicit requests/latches only after durable publication, retaining
unprocessed initial per-FD options. Failed writes retain exact runtime ownership.
No stored encoding or historical byte/format/API behavior is changed by policy
selection.

Storage, corruption, unknown or VOID-callback failures fence and monotonically
signal offlining; later callbacks do not retry storage or release contexts.
The controller must observe the signal and stop outside callbacks. No callback
joins/stops a dispatcher. The injected session joins first, removes/deletes only
owned filesystem objects, then captures diagnostics and destroys the writer.
Surviving contexts or fenced diagnostics make stop a failed outcome. Stop's
native ABI can block; a future live helper still needs an external bounded
supervisor and actual process/kernel retirement proof.

## Explicit capacity and durability limits

AllocationSize and volume total/free report the existing native live quota,
not host clusters or per-file compressed size: 255 extent slots, 64 KiB each.
The 4096-byte aligned reservation/cache value is distinct from this shared
extent charge. The 256-inode, 16 MiB aggregate decoded, 63-dirty, 4 MiB transaction
and 64 MiB append-history ceilings are unchanged. This is a small prototype.

The ordinary backing file retains per-transaction projected-growth and memory/
free-space admission, exclusive new-file creation, backend identity checks and
real FlushFileBuffers. Native reservations DO NOT physically preallocate host
storage or guarantee future writes against history exhaustion, allocation
pressure, media failure or other host writers. Thin virtual quota is an
experimental presentation requiring real-kernel free-space/cache qualification;
it must never be sold as a physical-space guarantee.

Every successful engine mutation keeps its three publication barriers and
captured success output. Kernel cached WriteFile acknowledgement is a distinct
contract: future mounted tests must explicitly FlushFileBuffers before asserting
durable file data. CPU barriers and modeled failures cannot prove physical
power-loss durability or immunity to future software/driver bugs.

The profile currently admits ASCII paths, 100-character components, bounded
native path length, at most 1 MiB per write and 4 MiB per read callback.
SetSecurity and volume label changes return explicit unsupported status.
POSIX unlink/rename is disabled; held-target replacement is conservative.
Streams/EAs/reparse, full sharing/locks/cache/suppression, broader names, Windows
11 client/app/installer, reclamation/scale, live AMD, autonomous learning and
payload redundancy/self-repair remain separate unfinished product gates.
The candidate never initializes a GPU.

## Configured qualification (inspect exact executed jobs)

- Pure CPU tests preserve both disposition policies, initial-option cancellation/
  re-arm, explicit/initial latch provenance, nonempty directories and lookup
  ownership. Separate arm/cancel raw/automatic fixed/compact traces inject every
  observed write/flush in all three existing fault modes. Independent CPU
  recovery/continuation must preserve complete old/new data/security/reservation/
  intent and unrelated acknowledged bytes.
- The hosted Windows candidate exercises both compression modes, real SDK
  Write/Read/Cleanup/Close dispatch, direct creation/open/overwrite and other
  callbacks, exact persisted readback after exclusive writer destruction,
  capacity failure before creation, stale tokens, two joined concurrent readers,
  global cancellation, initial deletion and terminal VOID fencing. Handle
  counts compare after identical initialization/thread warmup; they do not
  establish universal heap/kernel/VRAM leak freedom.
- A separate fake-provider lifecycle group covers invalid/missing opt-ins and
  every partial startup failure, callbacks alive during join, rollback order,
  one-shot state, external stop, controller-led offlining after a VOID failure
  with retained failed-context diagnostics, and independent reopening after stop. No real
  filesystem/dispatcher/mount/driver function is invoked.
- All old groups, 45-second per-group limits and mount/process/oracle caps remain.
  Configured tests are not passing evidence until their exact jobs are inspected.


### First hosted build result retained

Commit ce09720 compiled the adapter and pure core, but native test qualification
failed before execution: MSVC rejected value-initialization/assignment of the
SDK's zero-length trailing-array request/response structs, and the test used a
nonexistent NativeFileInfo.basic member. The harness now uses the same explicit
ZeroMemory pattern as the existing SDK tests and the actual snapshot metadata
accessor. No cap, test or oracle was removed. Inspect the follow-up commit's
executed results; this first build is not passing Windows evidence.
