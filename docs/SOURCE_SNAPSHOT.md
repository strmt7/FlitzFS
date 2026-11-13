# Bounded commit source snapshots (experimental)

The source-evidence workflow retains only the exact tracked Git snapshot and its
commit/archive hash. It neither ships SDK/binaries nor qualifies runtime safety.

At 94ec882 the 514 tracked files totaled 6,891,233 bytes; the largest was 223,618
bytes. The job refused its obsolete 512-file preflight despite fitting the byte
budgets. [Failure](evidence/source-package-count-94ec882.json).
The SDK and all 13 ordinary integration jobs at that source passed separately;
the failed source-snapshot job is not called successful.

The inventory now accounts for tracked files AND their unique parent-directory
entries against the existing 768-entry ZIP limit. Shared directories count once;
nested parents are funded. The independent 512-file rule is replaced by that
combined bound. All archive/inventory byte and actual ZIP-entry ceilings remain:
256 KiB of native inventory, 2 MiB per file, 8 MiB total expanded, 4 MiB compressed
and 768 actual entries. Oversized, linked/submodule, duplicate, unsafe or malformed
inputs still reject before archive creation.

The actual-ZIP check retains every old byte/entry cap and additionally requires
the exact file/directory inventory, no duplicates, exact lengths and every
canonical Git blob identity. Missing or substituted export content rejects.
Exact HEAD binding and archive SHA-256 remain. New pure CPU tests cover more than 512 files,
flat and directory-bearing 768/769 boundaries, shared/nested directory accounting,
file/directory collision, original byte limits, malformed/unsafe inputs and
actual in-memory ZIP omissions, duplicates and byte corruption.
Publishing-source inventory tests and actual source ZIP execution are pending.
No GPU allocation, process, queue, driver, owner staging or filesystem data limit
changes, and historical failure evidence remains tracked.
