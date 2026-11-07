# Read-only persistent catalog manifest v1

## Purpose and scope

A `.flcatalog` text file reopens a named collection of sibling `.flfs` containers.
The parser, loader and catalog CLI operations are implemented in native C++.
This is a **plaintext, unauthenticated configuration file**, not an encrypted
store, a transactional directory tree, or a secure reference to immutable content.
The `.flfs` v1 compressed-container format is unchanged.

## Format

The first line is `FLITZFS-CATALOG-1`, a literal TAB, then a decimal record count.
Each subsequent line contains a virtual relative path, one TAB, and a backing
container filename. Every line must end in LF or CRLF, including the last line.
Canonical serialization emits LF and preserves record order. No BOM, comments,
escapes, blank lines, extra fields or trailing records are accepted.

```text
FLITZFS-CATALOG-1	2
Documents/report.txt	report.flfs
Images/frame.bin	frame.flfs
```

The separators shown above are real TAB characters. The manifest and both
containers reside in the same physical directory. Virtual directories are
implicit; empty directories and original timestamps/ACLs are not represented.

## Bounds and path rules

- Maximum serialized file size: **8 KiB**, checked before the text buffer is
  allocated. Maximum records: **16**, checked before reserving entries.
- Virtual paths reuse the existing conservative ASCII namespace: 240 characters,
  100-character components, depth 16, case-insensitive collision checking.
- Container references must be one 6–100-character `.flfs` filename (extension
  comparison is case-insensitive). No slash, backslash, drive prefix, UNC,
  device namespace, stream syntax, traversal, NUL, wildcard or reserved name.
- Validate the entire namespace, including file/directory conflicts, before
  opening any referenced container. The validation helper performs no I/O.
- Bind filenames to the manifest's absolute parent, capturing the working
  directory once. The regular-file reader rejects final symlinks/reparse points.
  Parent directories must still be trusted: this is not a hardlink/parent-link
  sandbox or an authorization boundary for attacker-controlled local paths.
- Repeated backing filenames are permitted as explicit aliases. Each record
  consumes the usual reader/index budget, and verification counts logical bytes
  for every reference. No deduplication or alias-free-space benefit is claimed.

The existing 4 MiB aggregate serialized-index budget, 4 MiB read-request limit
and 32 MiB default whole-catalog verification budget still apply. These limits
are not a cap on every allocation in the process. The live probe additionally
uses its OS-enforced Job Object limits.

## API and commands

`parse_catalog_manifest` and `serialize_catalog_manifest` are bounded, in-memory
operations. `read_catalog_manifest_sources` returns validated absolute bindings;
`load_catalog_manifest` opens the existing CPU catalog. There is deliberately
no manifest-writing or replacement API in this milestone.

```text
flitzfs catalog-list collection.flcatalog
flitzfs catalog-list collection.flcatalog Documents
flitzfs catalog-verify collection.flcatalog
flitzfs catalog-probe collection.flcatalog Documents/report.txt 0 64
```

All three commands are read-only. Listing validates metadata but not every
payload byte. Verification decodes/checks the whole admitted collection. Probe
uses strict range bounds; it does not silently truncate an out-of-range request.
CRC32 is accidental-corruption detection, **not authentication**. Editing a valid
manifest can redirect a virtual name to another sibling container or replay an
older valid collection. A later authenticated store requires a distinct design.

## Research and validation rounds — 7 November 2026

1. Reviewed the existing catalog and no-follow file-reader contracts. Extracted
   namespace-only validation rather than duplicating its collision rules.
2. Reviewed Microsoft Windows namespace, device-name, stream and file-open
   documentation. Selected sibling-only references; rejected general URI,
   absolute-path or directory-traversal resolution. No new runtime dependency
   was added for this deliberately narrow, versioned configuration grammar.
3. Verified `from_chars` bounded decimal parsing. Required a count and terminal
   newlines so truncation/extra records fail rather than silently dropping files.
4. Negative tests exposed reserved-name normalization before an extension
   (`NUL .flfs`) and COM0-style names. Reproduced the failure, then aligned the
   canonical virtual-name validator with the stricter physical-path policy.
5. Added exact round trips, CRLF, every truncation of the valid fixture, bounded
   deterministic mutations, count/length limits, aliases, collisions, and real
   container read parity. Linux also tests final manifest/backing symlinks.
   Cross-platform CTest invokes the real CLI for empty-list/verify and rejection
   cases. The hosted live mount and crash-recovery probe now load the persisted
   catalog and verify it is unchanged across forced termination.

Primary sources:
- https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file
- https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew
- https://learn.microsoft.com/en-us/cpp/standard-library/charconv-functions

Success of local Linux tests is not a Windows/mounted-result claim. Use the
matching commit's CI results for the newly integrated persistent-catalog path.
