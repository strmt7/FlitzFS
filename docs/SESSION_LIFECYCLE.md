# Read-only session lifecycle — development gate

## Plan and decisions (7 November 2026)

Continue from the native callback milestone. First make startup, teardown,
consent and unexpected dispatcher exits explicit and testable without kernel
operations. Do not equate injected lifecycle tests with a mounted filesystem.

Research round 1 inspected WinFsp 2.1's actual `src/dll/fs.c`, `fsctl.c` and
native headers, not only its tutorial. `FspFileSystemCreate` can start the driver
service through `FspFsctlCreateVolume`. `FspFileSystemStopDispatcher` waits for
its worker thread with an infinite wait. A timeout on the caller's polling loop
cannot honestly be described as a guaranteed kernel-shutdown deadline.

Research round 2 inspected `src/dll/mount.c` and Microsoft's DOS-device API.
Drive-letter assignment uses `DefineDosDeviceW`; a preflight check alone is not
an atomic reservation. The preview policy consequently rejects drive letters,
automatic letter selection, UNC, extended/device namespaces and traversal.
Only fully qualified local directory candidates up to 240 characters are
accepted. A future live fixture must use a fresh, exclusively created parent,
validate/hold parent handles and rely on atomic leaf creation, not a check-then-
overwrite scheme. Existing directories must not be adopted as mount targets.

Research round 3 separated resource ownership from the runtime provider. The
session starts the two-worker dispatcher before exposing a namespace. It stops
and joins that dispatcher before removing its owned mount or destroying its
filesystem object. The adapter and backing handles outlive that sequence.
An external process supervisor remains necessary before live experiments;
process limits are not a guarantee against a faulty kernel driver.

## Implemented contract

- `ReadOnlyMountPlan` is inert data. Consent defaults to false. Lifetime is
  1–30 seconds and is a caller-enforced experiment deadline, not a kernel limit.
- `WinFspReadOnlySession` requires an explicit complete API provider. No native
  provider is selected automatically and normal tests supply only fake calls.
- All container contents are verified under the existing aggregate 32 MiB cap
  before any provider call. Limits on indexes, handles and decodes are unchanged.
- Startup is single-use. Invalid plans make no provider calls; operational
  failure rolls back owned resources and makes the session non-restartable.
- A failed dispatcher-start attempt still receives an idempotent stop. A failed
  mount attempt does not authorize removal of someone else's mount.
- `DispatcherStopped` records the first normal/unexpected stop notification
  atomically. It never calls teardown from inside a dispatcher callback.
- Explicit stop and destruction are idempotent. One control thread owns start,
  stop, state inspection and destruction; these do not race one another.

## Tests and limits

Portable plan tests reject missing consent, invalid lifetimes, drive roots,
relative/UNC/device paths, ADS, NULs, wildcards, dot traversal and overlong paths.
Native tests use the real WinFsp ABI but fake lifecycle operations; they exercise
success, preflight/create/start/mount failures, partial provider output, invalid
success output, unexpected stop during startup, duplicate stop notifications,
idempotent cleanup, callback lifetime during stop, and corrupt-source rejection.
No test calls a real volume-create, mount or dispatcher function.

Local GCC+Zstd, raw-only GCC and Clang ASan/UBSan each passed all six portable
executables. Native lifecycle qualification requires the new Windows CI run.
No live mount, driver installation, GPU work, encryption or writable storage is
claimed by this milestone. No operations ran on the owner's Windows PC.

## Primary sources

- https://github.com/winfsp/winfsp/blob/v2.1/src/dll/fs.c
- https://github.com/winfsp/winfsp/blob/v2.1/src/dll/fsctl.c
- https://github.com/winfsp/winfsp/blob/v2.1/src/dll/mount.c
- https://github.com/winfsp/winfsp/blob/v2.1/inc/winfsp/winfsp.h
- https://github.com/winfsp/winfsp/blob/v2.1/inc/winfsp/fsctl.h
- https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-querydosdevicew
- https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects

The commit-bound source-evidence workflow retains only tracked source, the
commit SHA and archive SHA-256 for three days. An archive is provenance, not
proof that tests passed; follow the separate CPU/native workflow result.
