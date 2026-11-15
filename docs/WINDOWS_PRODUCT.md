# Windows-only product contract

Owner clarification: 9 November 2026. Product work targets the latest stable
supported Windows 11 release on AMD64, with a Windows helper app and filesystem
driver integration. Old Windows compatibility, Linux and macOS products are
outside the current phase. "Latest" is resolved from current official release,
SDK/WDK and driver-policy sources, not a remembered release number or Insider
preview.

## Required product boundaries

Normal Windows Explorer access through a mounted volume is required. The helper
and tray manage that volume; they do not replace Explorer with a private browser.
WinFsp provides a kernel driver/user-mode bridge and Windows shell integration.
The existing FlitzFS writable adapter already owns the native session. Its
managed paged factory and cooperative group API are now implemented in the
[paged callback bridge](WINFSP_PAGED_VOLUMES.md). Mounted low-priority scheduling,
lifecycle and broad Windows-client qualification remain work to integrate with the
[current helper engine](WINDOWS_BACKGROUND_MAINTENANCE.md). No current driver
installation is qualified by an unmounted helper test.

The helper app owns user interaction, safe new-volume setup, explicit mount/
unmount lifecycle, status, recovery visibility and dependency/update orchestration.
The filesystem engine owns native namespace/data transactions, independent CPU
compression/recovery, integrity, durable acknowledgement and bounded maintenance.
An isolated optional HIP worker supplies acceleration only after trusted runtime,
adapter, budgets and output validation. It cannot control durable publication.
The driver integration presents normal Windows filesystem operations and must be
qualified against current Windows kernel, caching, security and application
semantics.

These are required boundaries, not claims that the app, writable mounted engine
or live GPU worker exists. Existing WinFsp callbacks/read-only hosted mounts are
an implementation foundation. Current research must determine the production
driver approach and its deployment obligations. FlitzFS remains its own
filesystem format/engine, with no archive application affiliation.

## Architecture research questions

Compare current WinFsp native API/driver/service design, Dokan, an actual native
FSD such as WinBtrfs, Microsoft ProjFS and Cloud Files. Identify which mechanisms
provide a real filesystem boundary and which provide projection/synchronization
inside an existing filesystem. Evaluate memory mapping/cache coherency,
FlushFileBuffers and acknowledged persistence, sharing/oplocks/delete/rename,
ACLs, streams/reparse/hard-link support, directory changes, process death,
request cancellation and driver/service lifecycle.

Review current Windows 11 driver signing, Code Integrity, Secure Boot and HVCI
acceptance. A downloaded signed binary or a short-lived Windows Server VM is
insufficient evidence for a latest Windows 11 installation. Do not adopt
workarounds that disable security protections or alter owner boot/policy state.

Use current official maintainer documentation and actual release/source identity.
Retain native Crawl4AI successes AND failures, provider/source hashes and dates.
A blocked page is not evidence and must not be bypassed. Current stable release
metadata can be checked through native repository APIs. Current published docs
may describe long-established interfaces; verify them against the selected
current driver/API sources before important implementation changes.

## Qualification

Existing non-Windows CPU and sanitizer jobs are implementation analysis only.
They do not create or promise a second OS product. Existing Windows Server CI
provides native build/API/lifecycle evidence only; actual latest Windows 11 client
VM, driver signing/security, application behavior and AMD hardware qualification
remain required. Local development, builds, GUI/GPU and safe tests use the host
under AGENTS.md and HOST_RAM_TESTING.md. Preserve existing data and driver/system
state, discover fresh resources and retire owned work. Unqualified driver
provisioning and destabilizing fault campaigns require isolation. The owner's
later 15 November permission permits installing a stable, qualified FlitzFS-
related driver after the exact artifact passes the host guide/AGENTS.md gates.
GPU and unrelated drivers remain untouched. There is no blanket hosted-only or
VM-first development rule and no currently qualified driver-install claim.

An app/driver package, working writable Windows volume, automatic resource-bounded
learning/repair, qualified AMD execution and independently verified recovery are
all outstanding product deliverables. Keep these requirements intact until
their actual implementation and end-to-end Windows evidence prove completion.


Current maintainer/source comparison and the resulting app/engine/driver
boundaries are recorded in WINDOWS_ARCHITECTURE.md, including the WinFsp
prerelease security-fix candidate, current Windows driver-policy gate, explicit
research failures and outstanding latest Windows 11 qualification.
