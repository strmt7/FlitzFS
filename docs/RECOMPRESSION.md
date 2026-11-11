# Verified native post-compression (experimental)

The explicit CPU recompress() primitive decodes and re-encodes every live
object into a NEW image under the supplied bounded compression policy. It does
not run in callbacks, launch a worker, schedule itself, reclaim the source,
execute HIP or implement adaptive/idle maintenance. Existing compact() keeps
its payload-copy behavior and all existing default routes and encodings.

A fast/raw landing representation can therefore be converted later without
rewriting the acknowledged source. This is a prerequisite for maintenance,
not an implemented autonomous learning system.

## Ownership and publication

Require the latest fully CPU-verified source, actual writer quiescence, a
distinct image identity, a NEW empty target and qualified nonaliasing/exclusive
backend ownership. The same per-backend source/target leases and terminal
failure discipline apply as in compaction. A lease does not authenticate a
global provider or exclude a second facade over the same storage.

Before target writes, admit a conservative raw-sized destination plus metadata
and require the existing physical/logical/codec budgets. This may reject a
target that would fit compressed bytes alone; never trade the reserve for
unbounded compression debt. Encoding streams one live object at a time using
existing bounded single-threaded CPU codecs, without a whole-volume payload
buffer or repeated encoder pass.

Preserve object IDs and generations. Rebind image identity and record digests;
native namespace identity alone changes, preserving inode IDs, sparse layout,
times, security descriptors, reservations and sequences. Native unresolved
file-context state rejects before copying.

Decode target records with the independent CPU reader and compare their exact
bytes against the source (or the explicitly rebound native namespace) BEFORE
publishing any root. Revalidate the unchanged latest source, then publish the
existing replicated metadata protocol and independently verify readback.

Failure leaves the source authoritative and the partial NEW target retained.
No automatic retry, name replacement, deletion or old-image fallback follows.
The explicit NativeVolumeHeadWriter publication must separately compare and
flush the complete candidate before changing selection; existing reader views
continue to own the preserved source.

## Qualification boundaries

Three additional CPU test groups exercise native and generic re-encoding,
pattern compression in raw-only builds, raw restoration, preserved generations,
native sparse/security metadata, live-writer/identity/nonempty-target rejection,
candidate readback corruption and every observed destination write/flush under
all three existing RAM fault modes with 4096/65536-byte contracts. Configuration
is not execution; retain exact-commit results before claiming a pass.

The portable locked-RAM probe adds explicit raw-to-automatic post-compression
on real and synthetic inputs, byte-exact CPU selection and separately reported
target payload. It retains the SAME 40-MiB locked/128-MiB child/20%-CPU/15-second
limits. Qualify the exact changed binary in the disposable Windows VM before
another owner-PC run. No background, mounted or GPU claim follows.

The first owner-PC CPU result BEFORE this primitive is retained in
HOST_RAM_VALIDATION.json. It demonstrated safe bounded unmounted RAM execution
with six public real files and honest full-image overhead accounting, not
post-compression, speed superiority or physical durability.

## Incremental candidate copy

A managed NativeFileSession can call begin_recompression, then advance the
candidate one live object at a time between foreground operations. It uses the
same encoder, raw-size reservation, record framing and publication checks as
the explicit synchronous path. The session holds one candidate and the source
writer remains live. The caller must serialize all calls through the same
control thread. No additional process, worker thread, queue or timer is created.

Each advance copies at most one record. The target has no published roots
during these steps. A committed source change invalidates the candidate before
the next step or publication, without reading or writing either backing on the
stale path. Cancel releases the candidate lease without touching storage. The
provider retains partial output and may reclaim it only under its own qualified
ownership rules. Pending file-context changes are rechecked before handoff.

Publication seals and verifies the independent target while the source writer
still owns the active image. Only then does the session retire that writer and
enter the existing volume-head publication and recovery path. Pre-handoff
target failures leave foreground operations available. The final verification
and publication still run synchronously and can scan the complete bounded
volume. This interface gives a future workload-aware scheduler a unit of copy
work and a stale-input check. Scheduling, priority, power and throughput need
separate qualification.

The volume-session invariant tests cover both 4 KiB and 64 KiB metadata
contracts. They check foreground access between copy steps, one-record writes,
stale detection after an acknowledged foreground mutation, deferred handoff,
cancellation, failures before writer retirement and byte-exact selected-image
readback using the original handles.

## Previous real-file host check

The exact 6da519db3dfc5da21bc8d19d9aa0233288f040b4 locked-RAM CPU artifact
processed 230,055 source bytes across six public files. Raw and recompressed
compacted image sizes were 851,968 and 393,216 bytes, respectively. The raw and
recompressed stored record payloads were 524,958 and 122,517 bytes. All source
hashes and readbacks matched. The unmounted run peaked at 33,816,576 locked
bytes, released all locks and confirmed process exit. This measurement used
the earlier synchronous path. It does not measure idle behavior or throughput.

The same commit passed the hosted synthetic locked-RAM handoff. Its two
mounted crash profiles initially failed to confirm worker exit in the
post-termination observation window. Both passed a same-source retry with
unchanged limits and independent CPU verification. The initial failures remain
unexplained and do not qualify crash retirement on an owner host.
