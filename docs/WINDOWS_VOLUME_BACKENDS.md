# Windows volume backends and application control

Product contract, 13 November 2026. The Windows helper, driver integration and
taskbar tray must create and manage BOTH physical-disk filesystems and
memory-backed virtual disks using the same FlitzFS engine.

The development memory volume exercises the real filesystem engine: namespace,
file I/O, compression, GPU/CPU routing, learning metadata, recovery, repair and
volume lifecycle are shared product functionality. Development must not become
a separate reduced-feature filesystem. No percentage of product completion is
inferred from the storage backend or source-code count.

## Shared engine and backend boundaries

| Shared functionality | Backend-specific responsibility |
|---|---|
| File/directory semantics and stored format | Obtain and own the selected storage region |
| CPU codecs and qualified AMD work | Allocate/admit memory or physical capacity |
| Transactions, integrity and recovery | Fulfil the declared persistence/failure contract |
| Reserved optimization state | Reserve and account nonaliased internal areas |
| Compaction, retained handles and publication | Preserve mappings and source ownership |
| Helper/tray lifecycle and diagnostics | Provision, attach and retire the selected volume |

The engine's checked StoreByteIo interface is the existing integration seam.
Its contract is an explicit requirement on the provider, not a way to infer a
device's failure geometry. Real physical-device provisioning and durable mapping
remain unimplemented. Image-backed development fixtures do not silently replace
the requested physical-volume product capability.

The application must clearly distinguish temporary memory volumes from
persistent disk volumes. The backend choice must not change file contents,
namespace semantics or independent CPU recovery.

## Required tray and helper behavior

- Discover eligible capabilities, dependencies and available resources; accept
  explicit configuration without hardcoded paths, drive letters or host models.
- Create a new volume with an explicitly selected backend, identity, capacity
  and fresh mount destination. Validate the full target and capacity before
  any irreversible provisioning step.
- Attach/reopen, show capacity and complete physical storage accounting, display
  activity/health/compression, and stop/detach with verified owner retirement.
- Keep helper/control authorization separate from ordinary file access. Learning
  updates use the controlled volume owner; arbitrary UI/raw offsets grant no
  mutation authority.
- Expose automatic bounded optimization and useful CPU/GPU fallback, with no
  routine manual codec or maintenance selection required.
- Install/update/rollback/uninstall through verified application-owned resources,
  preserving unrelated applications, current Windows security and stored data.
  Missing optional GPU setup must not block CPU-readable storage recovery.
- Do not leave stale workers, scheduled tasks, services, handles or allocations.

The current implementation is an experimental engine and SDK adapter, not this
finished application. A tray icon or a synthetic dashboard alone cannot satisfy
these lifecycle requirements.

## Development and release qualification

Continue development using the memory volume and the established resource and
driver qualification gates. Do not repeat the backend restriction in routine
progress messages. Physical-backend validation belongs to the final pre-release
qualification stage; product design is not authorization to provision a disk
on the owner's PC.

Use the same file-operation and application workloads for both providers, with
backend-specific lifetime/persistence fault models. Verify backend-independent
decoded bytes and metadata, capacity/accounting, restart, cancellation, safe
detach, upgrades and rollback. Actual Windows 11 client and full helper/tray
qualification remain required. The owner controls release.
