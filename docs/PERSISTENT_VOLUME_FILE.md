# Indexed volume files

[Managed paged volumes](MANAGED_PAGED_VOLUMES.md) · [Windows memory volumes](WINDOWS_MEMORY_VOLUME_FACTORY.md)

## File layout

A volume file carries an indexed filesystem and its durable selector and
funded advice on one exclusively owned stream. The regions are aligned to the
existing 64 KiB failure-unit ceiling.

| Offset | Region | Extent |
| --- | --- | --- |
| 0 | Paired native volume head | 128 KiB |
| 128 KiB | Paired funded optimization records | 128 KiB |
| 256 KiB | Selected indexed data image | Current file extent minus 256 KiB |

All record encodings retain their existing versions. The head declares the
volume and image identities and the indexed capacity. The file provider uses
epoch 1 and one append stream. Each bounded metadata region and the growing
image have disjoint write limits. File creation commits only the bytes used
by initialization. Creation publishes the image before the paired selector.
Missing or damaged advice leaves the data image usable.

## Create, inspect and resume

The Windows provider opens an explicit local path. It pins the parent,
checks the normalized paths of the opened objects, and verifies a
single-link regular file. Creation uses exclusive creation. Reopen requires
an existing file and an exclusive OS handle. A pinned read-only inspection
excludes write sharing. The file's declared capacity supports paged storage
beyond the older flat-store cap. Actual growth requires a separate live
free-space check before each admitted mutation.

Store admission compares the live logical and physical sector information
and the filesystem's effective atomicity size with the independently qualified
failure-unit contract. The code uses the existing file flush for barriers.
Live geometry and successful API calls are inputs to the provider boundary.
Physical failure-unit isolation, device cache behavior and the durability
of the new directory entry require qualification for the actual storage chain.
[Windows creation and sharing](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) ·
[Windows storage geometry](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_storage_info) ·
[Windows flush contract](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)

Open verifies both head peers and binds the selected data at the fixed image
offset. A supplied expected volume identity must match. Without that identity,
the checked peers must belong to the same volume. The independent CPU reader
validates the entire image and can inspect the durable state of detached
lifetimes. A writable reopen uses the same indexed session-recovery transaction
after the previous provider's confirmed retirement. The WinFsp bridge applies
the existing Windows root ACL and timestamp profile.

## Validation and remaining integration

The in-memory suite recreates the backing from durable bytes, checks old and
new file contents, and injects failures across the measured write and barrier
boundaries. The Windows SDK callback suite also exercises stop and reopen
with injected lifecycle operations. The optional real file-handle test uses
a newly created disposable fixture and belongs to a separately qualified
Windows test environment. [Validation record](evidence/windows-volume-file-recovery-2026-11-19.json)

The helper still needs persistent-volume selection and ownership. This
provider also needs bounded generation replacement, persistent catalog
compaction and full storage-failure qualification before release.
