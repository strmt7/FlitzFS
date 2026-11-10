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
