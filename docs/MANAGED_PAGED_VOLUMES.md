# Managed paged volumes

Experimental opt-in integration of the paged/native engine with the existing
volume head, file-session ownership and funded advisory journal. It removes the
flat-store-only selection/resume barrier. It does not install a driver, create
a mount, change the application's default volume factory, start a background
scheduler or qualify useful GPU operation.

## Creation, reading and ordinary files

`NativeFileSession::create_paged_volume` requires new empty qualified head/image
backends, distinct stable identities and explicit `PagedStoreLimits`. It creates
mirrored paged payloads with indexed names and native shared-base capability 3.
The capacity is an admitted backend bound, not a host RAM reservation or assumed
device capacity. Root metadata/security use the same checked native transaction.
The existing `create_volume` remains flat and keeps its previous wire encoding.

`NativeImageStorage` explicitly identifies flat versus paged storage and the
paged capacity bound. `NativeFilesystemSnapshot::open_image` dispatches exactly
that reader. A rejected/corrupt image never triggers another-format fallback.
Native paths, inodes, data/metadata, open contexts and independent CPU decoding
continue through the existing shared engine. Native namespace limits and complete
Model residency remain; this is not million-file qualification.

## Typed head encoding

The separate `FLTVH001` head still occupies two disjoint 65,536-byte slots.
Flat capability-1 records remain byte-for-byte unchanged. Capability 2 selects
paged images, retaining all identity/epoch/checkpoint/sequence fields and adding:

| Offset | Bytes | Meaning |
| ---: | ---: | --- |
| 8 | 4 | Head encoding capability 2 |
| 96 | 4 | Image kind 1: paged |
| 100 | 4 | Zero reserved field |
| 104 | 8 | Positive maximum physical image bytes |
| 112 | 65,392 | Reserved zero bytes, before digest at 65,504 |

The existing BLAKE3 covers these fields. Unknown kinds/capabilities, zero bounds,
intact unsupported/reserved fields, equal-epoch descriptor disagreement and
adjacent-epoch descriptor changes reject selection. A head is not authentication
or an anti-rollback device. Providers still qualify global ownership, durable
nonaliasing identity mappings and distinct failure domains.

New readers accept capability 1 and 2. Earlier head readers reject capability 2.
No in-place format conversion, automatic migration, capacity expansion or
product version bump is implemented by this extension. A future migration must
preserve supported readers and publish explicit recoverable state.

## Handoff, surviving-copy repair and recovery

Managed `compact_volume`/`recompress_volume` now preserve paged kind, capacity,
namespace layout, payload-protection policy, object IDs/generations, file state
and advisory ownership while selecting a new image identity. Contexts remain in
the same session table. The canonical comparison admits the correct bounded
native metadata/dependency inventory instead of a flat 256-object assumption.
Changed layouts/protection/capacity are not implicit compaction migrations.

Both target replicas are scrubbed before health comparison and again after its
flush, before head publication. Initial head creation checks health after scrub;
a newly discovered bad peer cannot pass using earlier cached flags. New head
publication requires a fully verified target. Source images remain retained.

A capability-2 head owner may resume over fully CPU-readable degraded source
data **for explicit selection of a repaired new image**. This does not grant a
writable lease on that data. Paged writer/session resume still refuses damaged
payload replicas; loss of both copies rejects reads. Legacy flat source-admission
rules are unchanged. The existing owned-transition recovery can select only the
original or the verified candidate and preserves the same file contexts. Head
damage never causes an unrelated/older-image fallback.

These operations are explicit synchronous control operations. The legacy flat
staged-recompression API rejects a paged source; its idle form defers before
target I/O. Paged cooperative rebuilding and reader-safe automatic handoff still
need a managed scheduler integration. Physical backend flush/alias/atomicity
contracts and actual power-cut behavior remain separate qualification gates.

## Collective work, provenance and advice

The session now forwards owned `prepare_shared_base`, `advance_shared_base` and
`finish_shared_base` control calls to the same native writer. They preserve the
[cooperative work contract](SHARED_BASE_GROUPS.md#cooperative-preparation-and-foreground-reconciliation),
including owner invalidation on handoff and selected-data guards. No raw writer
accessor or second reduced-feature filesystem is introduced.

The existing input-funded journal works with managed paged snapshots. Actual
accepted input funds advisory changes; grouping, image maintenance and reopening
do not mint input or reissue spent settlement. Journal state/ownership survives
image selection. The journal pays for its own updates: **it does not authorize
or pay for data-image rewrites**. Resource/foreground admission, learned grouping
and a separate appropriate data-work/write-amplification policy remain required.

The new [automatic group controller](AUTOMATIC_GROUP_MAINTENANCE.md) supplies
aligned content discovery and resource admission from trusted supplied
observations through this same session. Live Windows observations/scheduling,
persistent learning and a data-work/write-amplification policy remain unfinished.

## Evidence and source scope

[Source-bound validation](evidence/managed-paged-volume-2026-11-15.json) records
ordinary session/context operations, journal funding/resume, malformed descriptors,
over-256-object native inventory, surviving-base repair and every observed modeled
handoff mutation/barrier cut. Fixtures use synthetic in-memory backends. They do
not qualify a driver, mount, physical power loss, real workloads or performance.

The current [FAST 2026 integrity abstract](https://www.usenix.org/conference/fast26/technical-sessions)
supports evaluating integrity across the whole I/O stack; this is an abstract
review, not replication of the paper or Windows platform evidence. Current
[FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
and [file-caching documentation](https://learn.microsoft.com/en-us/windows/win32/fileio/file-caching)
were recaptured with the admitted isolated Crawl4AI provider. The caching page
has an old update date and is retained only as background, not recent evidence
for new durability semantics. Native captures and the refused stale environment
are retained. No new Win32 API, physical provider or durability assumption was
introduced; model results depend on the explicitly qualified byte-I/O contract.
