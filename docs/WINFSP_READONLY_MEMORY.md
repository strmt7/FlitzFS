# Sealed memory volumes through WinFsp

The memory provider can now expose a completed managed, paged FlitzFS volume
through the existing read-only Windows adapter. It uses the same native volume
head, extent decoder and file metadata as the writer and independent CPU reader.

The control thread retires the native writer, then `MemoryVolumeBacking::open_read_only()`
checks that the head and image belong to the same locked domain. It opens the
selected native image through `NativeVolumeHeadView`, which verifies the identity,
capability, current checkpoint, namespace and referenced contents. The projection
uses the stable managed volume identity for its Windows serial.

Before returning the projection, the provider checks every owned store for a
live writer lease. A successful check seals the domain for reading. Writes, write
admission, flush, new stores and writer acquisition are then refused, including
through previously retained aliases. A failed check leaves the original state
available to its owner. Sealing is a control-thread operation after mutation and
writer retirement. Projected reads serialize each native reader's decoder.

A projection retains the image and domain lease. The original owner keeps its
backing when a session starts, stops or fails to mount. The writable memory
controller exposes this operation only after an explicit stop and join, even
when its view already reports retirement. The physical image format is unchanged.

## Windows provider and runtime selection

`linked_winfsp_session_api()` returns the actual SDK operations linked in the
process. The caller selects this table explicitly. Returning the table performs
no filesystem creation, driver control or mount. Test providers keep using the
same interface with injected lifecycle operations.

`prepare_winfsp_sdk.ps1 -Profile Stable` stages the checksum-pinned official 2.1
user-mode SDK in a private directory. The existing candidate profile remains the
default for the 2.2 beta. Both use read-only MSI extraction. The stable staging
path also works in Windows PowerShell with the system HTTP and SHA-256 APIs.

The staged stable DLL matched the installed signed 2.1 DLL byte for byte. The
installed AMD64 driver reported the same product version and a valid Microsoft
hardware signature. Its service was stopped. These are artifact and state
observations. Mounted compatibility on the target Windows 11 client still
requires an actual matched driver and runtime test.

## External-client fixture

The optional `flitzfs_mount_probe` mode `--run-disposable-memory` uses fixed
synthetic files and locked memory backing. The fixture admits a fresh directory
under the qualified disposable root and pins its ancestors. The child creates
and closes the normal native writer, seals the volume and starts the read-only
adapter with the linked provider. It accepts no caller-supplied image or mount path.

The separate client process checks case-insensitive reads, mapped access,
directory enumeration, the Windows read-only flag and mutation refusal. After
the client finishes, the child joins the dispatcher, checks that its mount
disappeared, and compares the backing bytes and independent CPU readback. The
child runs under the existing 128 MiB, 20% CPU and 30-second process envelope.
The surrounding disposable job gives this mode a 40-second controller deadline.

Local validation covers SDK callbacks, read-only startup rollback, retained
ownership, sealing and existing paged behavior. The owner system driver remains
outside this mounted test. The separately provisioned disposable VM workflow
runs the real mode with signed pinned WinFsp on an isolated system.

[Source-bound evidence](evidence/winfsp-readonly-memory-2026-11-17.json)
