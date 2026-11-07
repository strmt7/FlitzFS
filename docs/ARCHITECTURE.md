# Architecture / deliberately staged development

## Today: bounded CPU-only chunk core

```text
ordinary source file (read only)
    -> bounded 4KiB..1MiB chunks
    -> CPU Zstandard or raw fallback
    -> CRC32 of each logical chunk
    -> NEW .flfs file (never overwrite)

read -> validate container index -> locate touched chunks
     -> bounded CPU decompress -> verify chunk CRC32 -> tiny verified-chunk cache
     -> return requested range
```

The container reader deliberately has **no** filesystem mount calls, user-PC API calls, AMD GPU device initialization, encryption, file deletion, volume creation, shell commands, background tasks, or service installation.

## Stage 1.5: bounded in-memory read-only directory catalog (implemented)

`ReadOnlyCatalog` maps up to 16 existing independently indexed `.flfs` files onto
conservative, relative ASCII virtual paths. It builds parent directories, handles
ASCII case-insensitive lookups, rejects traversal/reserved Windows names and
collisions, and delegates byte-range reads to pinned read-only `Reader` objects.
A 4 MiB aggregate index-data budget limits the catalog. It neither persists
metadata nor mounts a Windows drive; there is no mutation API.

**Important:** Catalog construction validates metadata, not every chunk payload;
reads verify the touched chunks. In M1.6 the catalog serializes reads and full
verification **per backing file**, protecting the mutable cache and Windows file
pointer without unnecessarily serializing independent files. Direct `Reader`
usage remains single-threaded. The catalog supports an explicit `verify_all`
operation with a default 32 MiB total decoded-byte limit, at most 256 MiB.
This verifies payloads without writing; it never turns CRC32 into authentication.
WinFsp callbacks, catalog lifetime under concurrent requests, Unicode names,
Windows ACLs, timestamps, security descriptors, cache eviction, and
persistently authenticated manifests remain future gates.

## Stage 1.7: in-process virtual Windows path projection (implemented, no mount)

`ReadOnlyProjection` accepts only root-relative `\`-delimited ASCII virtual paths,
including `\` for the root; it translates to the catalog's canonical `/` paths
without accessing real OS paths. Drive-qualified, UNC, traversal, control,
embedded NUL, and Unicode names are rejected. Read requests admit at most 4 MiB
and return a **short read** when they cross EOF, or zero bytes when at/past EOF.
These semantics are independently unit-tested across CPU codecs and are intended
for a future WinFsp callback translation. Error-code mapping, Windows metadata,
callback lifetime/cancellation, actual mounting, and live WinFsp tests remain
**unimplemented**.

## Stage 2: read-only Windows mount, only after tests

WinFsp is a reasonable user-mode filesystem bridge for Windows. The future adapter will translate open, read, metadata, directory enumeration and memory-mapped reads into bounded requests against a new immutable chunk/object store. Start in a disposable test mount folder with NO persistence of the mount across reboot. Never mount over an existing directory containing data. Retain a read-only CPU export path that does not require the mounted filesystem.

File and directory metadata, per-block checksums, availability of independent decoding, and a secure path normalization model must be defined before exposing untrusted data through WinFsp. The current v1 single-file container is a backend experiment, not that metadata model.

## Stage 3: bounded adaptive HIP backend

HIP is planned as an *optional* strategy, not a requirement to read the data. Before enabling any AMD GPU work on a Windows computer, first measure equivalent CPU-only performance and inspect the current driver/ROCm qualification status. Any initial GPU experiment must:

- Be opt-in, run without a mount, and be bounded to a fixed small test buffer copied from disposable test data.
- Bound the total GPU allocation (proposed first cap <= 128 MiB), queue depth (1), and job duration; avoid long-running compute kernels that might trigger Windows TDR resets.
- Never operate on sole-copy user data; compare decoded bytes to CPU reference and abort on a mismatch.
- Allow a clean CPU fallback for GPU busy, driver restart, loss of allocation, or codec failure.
- Time all H2D transfers, kernels, D2H transfers, synchronization, and filesystem handoff, not only device-kernel duration.
- Run with low scheduling priority where available and refuse tests when free RAM/VRAM headroom is inadequate.

A GPU device *alone* cannot make a transparent Windows read faster. Small requests and GPU-contended workloads may strongly favor CPU decoding. Codec selection belongs in a measured deadline-aware policy.

## Stage 4: encryption and transactional writes

A writable encrypted store will require authenticated per-chunk records, persistent versioned metadata, an independently recoverable commit protocol, nonce uniqueness despite crashes/restore, and an authenticated metadata/extent map. CRC32 is suitable only as an accidental-corruption detector. The proposal is compression-before-authenticated-encryption, initially using a vetted CPU AEAD implementation (e.g., XChaCha20-Poly1305) and a deliberately designed key hierarchy.

Crash/kill/power-loss tests must establish which operations were acknowledged as durable, and recovery must be possible without an AMD GPU. Only then would a writable WinFsp mount be enabled for disposable test data.
