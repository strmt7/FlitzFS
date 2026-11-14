# Next ordinary Windows write bridge

This is a researched implementation boundary, not an asynchronous adapter.
The current controlled write coordinator remains explicit, and normal callbacks
remain CPU-only. Its reused dispatch passed source-specific CPU/SDK qualification at 3b7a6c9;
actual GPU execution through this coordinator remains unqualified.

Current official WinFsp source was captured at
[b0508fad](https://github.com/winfsp/winfsp/tree/b0508fad28306b71d7eba340915ed64798229607),
dated 12 November 2026. Native API captures retain exact Git blobs, byte counts and
SHA256 digests in [the research receipt](evidence/native-async-write-research-2026-11-14.json).
No webpage extraction, SDK update, driver operation or owner mount is claimed.

The [public header](https://github.com/winfsp/winfsp/blob/b0508fad28306b71d7eba340915ed64798229607/inc/winfsp/winfsp.h)
documents deferred responses for read, write and directory-read operations.
The current operation context is thread-local and may be obtained only while
servicing a callback. Own the input bytes and required opaque request identity
before returning; callback-local pointers must not become long-lived ownership.

The [response implementation](https://github.com/winfsp/winfsp/blob/b0508fad28306b71d7eba340915ed64798229607/src/dll/fs.c)
submits the response to the driver and records/stops on delivery failure.
Submission returns no status to the caller. Storage commit and response delivery
must therefore be separate states: never repeat a mutation because its response
could not be delivered.

The [upstream asynchronous sample](https://github.com/winfsp/winfsp/blob/b0508fad28306b71d7eba340915ed64798229607/tst/memfs/memfs.cpp)
shows the response identity and committed byte-count/FileInfo fields. It does not
qualify a scheduler for FlitzFS or authorize unbounded threads/delays.

## Implementation requirements

- Retain immutable CPU input, exact file-context ownership, request identity and
  one terminal completion. GPU failure must preserve independent CPU writes/reads;
  CPU encoding need not match GPU format or size.
- Separate worker supervision/CPU verification from native publication. Respect
  the existing controller's serialized ownership; do not race poll, fallback,
  retirement or session destruction across callback threads.
- Specify append, overlap, truncate, rename/delete, flush, cleanup/close and stop
  ordering before activating deferred writes. Do not assume flush or metadata
  operations support pending status.
- Keep bounded queues, resource admission and a joined lifecycle. Current native
  commit facts already provide byte count and FileInfo; do not requery after
  publication or send a stale/duplicate completion.
- Gate automatic GPU routing on qualified configuration/shape/resource evidence.
  Work batching must improve useful complete-operation costs; a pending API or
  successful CPU fixture alone is not acceleration.
- Qualify the exact pinned SDK/driver ABI and full latest-Windows-client behavior
  in disposable environments. Then test cancellation and stop at every lifecycle
  boundary, with no lost acknowledged bytes or orphan work.

The next code slice should implement owned pending-write/completion state and
join it to the actual callback path. Finite cross-operation coalescing, automatic
cost acquisition, scalable storage and collective compression remain required.
Do not replace those exits with another capability-only probe.
