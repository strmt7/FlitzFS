# Native paged compaction and recompression (experimental)

`NativeFilesystemWriter::compact` and `recompress` now accept the native paged
backend, including the original namespace, inode-record capability 1 and indexed-
directory capability 2. They stream live records into a new empty backend and
rebind its namespace identity. Layout, inode IDs, object generations, namespace
sequence, data, sparse regions, timestamps, security, reservations, sharing and
workload provenance are preserved. No source bytes are written or flushed.

This removes the backend's previous maintenance refusal. It does not select a
new active volume, remove the source, launch background work, run a GPU or
provide the planned collective codec. Flat volume-head handoff and staged
recompression still reject paged sources.

## Admission and ownership

The source must be the current quiescent snapshot, with all file contexts retired
and durable delete intents resolved. Acquire its exclusive backend lease and
check current roots. A live writer, stale root, same/zero new identity, aliasing
backend, nonempty target, invalid contract, unavailable codec or invalid native
graph refuses. Custom providers must qualify nonaliasing and exclusive backing;
per-instance leases cannot exclude a different facade over the same storage.

Before target writes, independently validate all native references and source
payloads. The private root replacement must match the admitted root byte for
byte except its 16-byte identity at offset 40. It must retain the same length
and object ID. This extension never exposes arbitrary replacement to public
paged rebuild callers.

The target uses the source snapshot's configured `PagedStoreLimits`, available
through its new `limits()` accessor. This is a caller-admitted capacity bound,
not a discovered amount of free RAM or a guarantee that another provider can
allocate it. Backing admission is refreshed before growth; a capacity/resource
failure retains the source and incomplete new target. Paged metadata isolation
derives from the target backend contract. Flat-only `compact_metadata=true`
rejects before target writes instead of silently changing its meaning.

## Copy and publication

Compaction preserves each non-root object's codec and exact stored payload. A
future-write compression policy does not cause those objects to be re-encoded.
The rebound root is encoded privately. Recompression instead CPU-decodes and
re-encodes every live record with the requested policy; codec streams may change,
but exact recovered source bytes must agree.

Both paths retain the paged streaming builder: one unfinished leaf and bounded
references per tree level, without a volume-sized payload inventory. The native
frontend still retains its complete bounded Model; this is not scalable native
residency. Old history is omitted. New identity-bound record digests and page
references are rebuilt, while individual object generations are preserved.

Every target record is independently decoded and compared before inclusion.
Finish and flush dependencies, verify the complete new tree, reject degraded
new metadata, and recheck the unchanged latest source and its content. Only then
publish the two roots with their existing separate barriers. Reparse and scrub
the published target before returning a writer. Any failed comparison or
indeterminate publication throws; no retry, active-volume swap or fallback
selection is performed. A failed target may be unreadable or contain one complete
verified new image, never an acknowledged partial namespace. Retain it for explicit
independent recovery. Historical source snapshots remain readable.

## Validation and limits

Shared tests exercise all three layouts, live/stale/pending/identity/option/alias
refusal, metadata and sparse-byte parity, encoded compaction under a raw future
policy, raw restoration, large combined inventory, source/target corruption and
all observed destination write/barrier cuts under three failure modes at 4-KiB
and 64-KiB contracts. Independent CPU opening checks any exposed candidate and
continues writes after recovery. Source bytes and source write/flush counters
must remain unchanged.

Complete image sizes are reported for one controlled repeated-byte fixture.
Those counts establish that post-compression is connected, not real-data
superiority, GPU benefit or a compression record. The broader validation uses
raw-only Windows Release and AddressSanitizer configurations; optional codecs,
mounts, drivers, physical power cuts, Linux and automatic maintenance remain
unqualified by this round. Payload redundancy/repair, paged native residency,
reader-safe selection/reclamation and automatic low-priority scheduling remain
required. [Execution evidence](evidence/native-paged-maintenance-2026-11-15.json).

Current [SQLite VACUUM source](https://github.com/sqlite/sqlite/blob/master/src/vacuum.c)
was reviewed through its native repository API on 15 November 2026, file blob
`70e62e1ef1b71693a193e915a50dd19e31fc648c`. Its separate-output and nonempty-output
checks inform the copy boundary. FlitzFS uses its own leases, CPU validation and
publication protocol; no SQLite code, format or dependency was introduced.
