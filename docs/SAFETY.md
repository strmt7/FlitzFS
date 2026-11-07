# FlitzFS safety gates and threat model

The user's primary constraints are: **no formatting, no lost data, no drive modification, no system freezes, no resource exhaustion**. These are treated as engineering requirements and release blockers. No software can honestly guarantee immunity from all software, firmware, hardware, or power-loss failures; therefore new code must remain isolated from original data until it has evidence of correct behavior.

## Present implementation: allowed effects

- Read the explicitly supplied input file/container path using a validated regular-file handle, with final-component no-follow and POSIX nonblocking open.
- Create ONE explicitly supplied **new** ordinary output file; never overwrite or delete an existing one.
- Create small, uniquely named test fixtures in a newly created temporary directory when running the unit tests.
- Compile on a developer's own build host or GitHub runner, without privileged operations.

No mounts, raw devices, disk partition APIs, volume formatting, boot modifications, Windows service registration, scheduled tasks, registry updates, filesystem minifilters, GPU compute, or encryption are implemented. This milestone does not need administrator privileges. The in-memory catalog has no writes or filesystem mounts. Tests never invoke `remove_all`, format utilities, or shell commands to alter drives.

## Resource limits enforced in the code

| Resource / behavior | Default or bound |
|---|---|
| Maximum file to pack | 256 MiB by default; up to 4 GiB only with explicit budget |
| Maximum file to extract or verify via CLI | 256 MiB by default; up to 4 GiB only with explicit budget |
| Chunk bytes | 4 KiB–1 MiB, power of two; default 128 KiB |
| Metadata entries | At most 1,048,576, each 32 bytes |
| Single random read | At most 4 MiB |
| Stored compressed chunk | At most 2 MiB |
| Decoded read cache | One 1 MiB-or-smaller chunk per Reader |
| In-memory virtual catalog | At most 16 read handles and 4 MiB aggregate index metadata; no persistent mount |
| Catalog full verification | Default 32 MiB total decoded bytes; upper limit 256 MiB, preflighted before decoding |
| Concurrent catalog reads | Per-file mutex protecting mutable decoder cache / shared Windows file pointer; no background thread pool |
| Zstd decoded output | At most 1 MiB, checked against single-frame metadata; single-shot decoder window is not independently capped |
| CPU compression level | 1–5 only; one CPU stream, no parallel workers |
| Disk-space preflight | Refuse projected output unless 1 GiB additional space remains |
| Output path | Exclusive new-file creation; existing paths are never replaced |
| GPU memory and scheduling | Not used at all |

These checks reduce risk but do not protect against concurrent disk filling, I/O driver bugs, quota changes, hardware faults, unexpectedly high OS allocation overhead, memory fragmentation, or a malicious concurrent process changing backing files. The check is performed once before writing; the OS can still return ENOSPC later. Interrupted writes can leave a partial new file. Output correctness must always be verified before discarding the original.

## Threat-model limitations

- CRC32 is **not authentication**. Anyone who can edit the container can recompute it. This version does NOT protect confidentiality or tampering.
- Input files or container files can change during processing; v1 does not snapshot or lock them. Handles are pinned to an opened object, but other processes can still mutate underlying content (particularly on POSIX). Use immutable copies for testing.
- The format parser bounds lengths and checks arithmetic, but has not been through a professional security audit or an exhaustive external fuzzing campaign.
- Zstd decompression requires a trusted and patched Zstd library; an intentionally malformed stream is handled as an error, not as trusted data.
- There is no read-only WinFsp mount yet, so no claim is made about full filesystem semantics, Windows crash recovery, ACLs, mmap, paging, or WinFsp callback safety. M1.6 covers simultaneous read-only `ReadOnlyCatalog::read` calls against a live, nonmoving catalog; object lifetime and every other callback remain separate gates.

## Explicit prohibitions for later work

1. **Never** run `format`, `diskpart`, repartition, change drive letters, format a volume, use raw disk/PhysicalDrive writes, or mount a test filesystem over an existing nonempty directory.
2. **Never** install a driver, launch stress tests, change AMD overclock/voltage settings, or create scheduled tests automatically on the user's machine.
3. **Never** put the storage backing directory, service binaries, journals, or recovery utility inside the virtual drive they operate.
4. **Never** enable write access on important personal data until the isolated crash/recovery test suite passes and independent backups exist.
5. Do not treat a GPU result as valid until the CPU reference decoder confirms its bytes and a safe fallback exists.

## Environmental scope of this milestone

All compilation and tests included in the handoff were performed in an isolated Linux container, **not on the user's Windows PC**. No AMD GPU was accessible. Passing those tests does not establish Windows, HIP, WinFsp, power-loss, or performance qualification.
