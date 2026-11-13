# Experimental Windows volume application

Implemented source slice, 13 November 2026. Qualification results must be tied to
the exact commit and workflow run; this document does not predeclare a pass.

The optional native Win32 application owns a real NativeFileSession on a
process-lifetime locked-memory provider. It creates a new volume, reserves its
separate optimization area, creates new text files, verifies saved bytes, lists
and reads them, runs explicit bounded CPU recompression, and retires the engine.
It never opens a storage path, initializes HIP, selects a driver or mounts a
filesystem. It is not yet an Explorer-accessible volume manager.

## Ownership and resource limits

One domain is admitted per process. A domain lease remains owned until the last
backing store is destroyed, including a store retained beyond its wrapper.
Working-set changes affect this process only and restore the prior settings
after retirement. Allocations use VirtualAlloc and VirtualLock with a verified
initial page-residency query. No pageable fallback is accepted. Failed cleanup
permanently refuses a new domain in the process; unconfirmed frees do not recycle
their accounted capacity.

The provider admits at most 40 MiB in one domain. This application uses a 12 MiB
experimental aggregate reservation, a 4 MiB image, a 128 KiB head and a separately
reserved 128 KiB advisory area. Candidate images share that same aggregate cap.
The physical and commit reserve is the greater of 768 MiB and one eighth of
detected physical memory. Missing/insufficient telemetry refuses admission.
These bounds are qualification limits, not final product sizing.

A memory flush is a serial ordering barrier: prior bytes remain available
while the process owns the allocation. It does not copy the entire image into
a second buffer. The declared persistence scope is the owning process lifetime,
not process-crash or power-loss survival. No disk durability is inferred.
The app reports file logical bytes, combined current image/head/advice extent,
and all allocated provider capacity separately; a payload ratio is not presented
as total-space savings.

## Controller and UI

One owned worker serializes all engine access. One command may be admitted;
oversized commands or a concurrent operation are refused. Results are bounded
owned copies and window notifications contain no heap pointers. Compression,
resource admission and metadata queries run outside the window procedure.

Native controls use the system font, UTF-8 conversion is checked and editor
inputs are capped at 16 KiB. Save creates a NEW name only. Creation and writing
are separate engine transactions: a failed write may leave an empty new file,
and the UI reports failure while retaining ownership. Existing files are never
overwritten by this editor.

Optimize uses explicit staged engine maintenance and independently verified
publication, with a maximum of 257 bounded engine advances. This is not yet
automatic background learning or useful GPU acceleration. An unchanged image
is suppressed by the engine policy. Advisory-state creation does not mean the
application has implemented an autonomous learner.

Close hides a live temporary volume when a tray icon exists; otherwise it keeps
the window visible. Discard is a separate explicitly labelled action. Exit is
available after discard. A normal stop reports completion only after engine
owners and allocations retire. The controller joins its worker; no detached
thread, service, scheduled task, registry edit or reboot persistence is used.
The tray establishes notification version 4 and is removed during exit.
Forced process termination necessarily loses this temporary memory volume.

## Build and qualification

Build with FLITZFS_BUILD_VOLUME_APP=ON on Windows x64/MSVC. It defaults OFF and
does not enable WinFsp. The hosted portable build uses the current pinned CPU
dependencies and embeds an asInvoker, PerMonitorV2 manifest.

The separate volume-app workflow builds the application and a test harness.
Model tests exercise lease lifetime, aggregate/alignment/overflow refusal, exact
I/O, empty-write behavior, duplicate-name preservation, real file readback,
recompression, controller stop permission and cleanup. GUI tests launch the
actual executable inside the existing creation-time job containment, operate
its native controls, compare readback, close without discarding an active volume,
then explicitly discard and exit. Child limits are 128 MiB, 20% CPU and 25 seconds.
The harness has its own bounded CPU/process/commit budget. These are synthetic
qualification inputs, not full GUI/accessibility or mounted-filesystem coverage.

Physical provisioning, mount lifecycle, complete file operations, protected
helper authorization, automatic low-priority learning and qualified useful AMD
routing remain required product work. The final application must manage both
memory and physical backends through the same engine; see
[the backend contract](WINDOWS_VOLUME_BACKENDS.md).

The initial 477e688 build compiled successfully, then its dependency audit stopped
before execution because the console-only profile rejects native GUI libraries.
[The retained failure and current source research](evidence/volume-app-477e688.json)
document the correction: a separate role-specific GUI audit, unchanged console
policy, ASLR/high-entropy/NX checks and no delay-loaded or non-OS imports. The
application now also restores its window when tray registration is unavailable.
Corrected-source execution remains pending.

The corrected 58fa7c3 build and both role-specific PE audits passed. Its model
reached the final handle-balance assertion and failed there; GUI execution was
skipped and no owner binary was launched. The magnitude and cause were not
recorded by that test version. The next tests retain the assertion and add
provider/engine/controller phase counts. See
[the retained failure](evidence/volume-app-58fa7c3.json).

The provider now keeps at most 64 store registrations and explicitly verifies
all current pages and the working-set reservation at application status/discard
boundaries. This complements initial allocation verification without copying
image contents or changing transaction flush behavior.
