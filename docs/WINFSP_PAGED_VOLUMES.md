# Paged volumes through the WinFsp writable adapter

[Windows product](WINDOWS_PRODUCT.md) · [Shared managed engine](MANAGED_PAGED_VOLUMES.md)

## Implemented connection

`WinFspNativeWritable::create_managed_paged` now creates a new mirrored paged
volume through the same `NativeFileSession` used by the writable SDK callbacks.
It selects indexed names/shared-base capability, the Windows-global disposition
policy, a real owner ACL and UTC root metadata. Accepted-input provenance is
enabled before exposure. A supplied distinct qualified advisory backend is
bound through the existing funded-journal protocol.

Create/Open, Write, Read, metadata, directory enumeration, security, Flush,
Cleanup and Close retain the existing callback table. No reduced-feature second
filesystem is introduced. Legacy flat creation remains available and unchanged.
This bridge is required groundwork for normal Explorer/Win32 access; it is not
a mounted-product qualification or an installer.

## Resume and compatibility

`resume_managed_retired` requires exact volume identity/head epoch, a qualified
image resolver and actual retirement of the previous dispatcher/controller.
The resolver is pinned once during read-only preflight and reused for resume.
The complete Windows FileInfo/security profile and original root creation time
must be valid before repair is allowed. The same profile validator is shared
with the historical file-backed restart path.

Supported flat and paged representations retain their decoder/layout selection,
root ACL, metadata and volume serial/time. Earlier layouts are not silently
promoted to shared-base storage. Optional advisory resume requires its exact
sequence and preserves funding; grouping never mints accepted input. Provider
authentication, globally nonaliased backing and previous-owner retirement remain
caller responsibilities, not properties proved by a per-instance lease.

## Cooperative controller opportunities

The adapter owns the core `NativeGroupMaintenance`; no callback starts a
maintenance thread or performs device discovery. A trusted controller calls
`advance_group_maintenance` with fresh observations on its low-priority context.
`WinFspNativeSession` forwards this only while running.

Each opportunity tries the callback control lock and refuses queued/active
foreground calls, native pending writes, outstanding responses and worker
retirement. It does not drain a response or wait for GPU teardown. Unknown
retirement, a stopped dispatcher or a failed owner makes optional work unavailable
and releases private buffers. CPU file access retains its existing independent
path. A healthy GPU service that is unavailable but confirmed quiescent does
not prevent the CPU group encoder from operating.

The cached `namespace_sequence` query supplies change detection without backing
I/O or storage authority. It also observes commits performed by the deferred
service. Completed unchanged volumes return without source I/O. Selected edits
invalidate prepared work; unrelated current metadata is preserved at publication.
Explicit cancellation releases the private controller. Destruction remains
serialized with the owning lifecycle; the dispatcher and deferred completion
owner retire before the adapter/native writer are destroyed.

The [owned WinFsp controller](WINFSP_OWNED_CONTROLLER.md) now supplies callback
events, real OS observations, resource priority, pacing and joined ownership.
Its lifecycle has been tested through injected providers, alongside the
unmounted helper's collector in [Windows background maintenance](WINDOWS_BACKGROUND_MAINTENANCE.md).
This is not yet qualified on a real running Explorer volume. A quantum
and final commit remain synchronous while the control lock is held; mounted
interference and practical foreground stalls must be measured before promotion.

## SDK dependency and validation

The latest development SDK candidate was checked against the current upstream
release API: v2.2B5, published 13 October 2026. The existing exact MSI byte/hash
pin is retained. This is a prerelease development dependency, not a production
driver qualification. Staging now reads embedded MSI cabinet streams in read-only
mode and extracts only the four selected headers/library/user-mode DLL payloads
using Windows' built-in cabinet tool. It needs no installed 7-Zip, runs no MSI
installation/custom action and selects no driver/service payload.

[Exact source and executed checks](evidence/paged-winfsp-2026-11-15.json) separate
real SDK-DLL callback execution from injected lifecycle functions. The new
memory-backed fixture exercises group creation, active file-context readback,
selected foreground mutation, busy callbacks/responses, canceled/stopped owners,
flat/paged restart and independent CPU recovery at all observed modeled
publication cuts. Telemetry in these callback tests is explicitly modeled.

The injected lifecycle passes inert path strings only to test functions; it
never calls the native filesystem-create/dispatcher/mount functions. No test
installs or loads a kernel driver, formats a disk, runs HIP or writes a backing
image to physical storage. This is not a live mount, physical power-loss test,
recognized benchmark, useful GPU result or release readiness claim.

Next connect the owned controller to a shared memory-volume factory and explicit
native provider, then qualify it through a disposable
memory-backed mount. Scalable persistent namespace/coherence, full Windows
client semantics, useful ordinary AMD operation and deployment remain required.
