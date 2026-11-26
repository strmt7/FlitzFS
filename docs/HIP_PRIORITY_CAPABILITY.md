# HIP worker stream capability

ROCm Core SDK 10.2.0 ships HIP 7.17.0 and documents that priority streams
are disabled. The worker requires `hipStreamCreateWithFlags`. The
priority-range query and priority stream creation are optional capabilities.
[AMD release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html#hip-7-17-0) ·
[Stream management](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/reference/hip_runtime_api/modules/stream_management.html)

After matching and selecting the adapter, the worker corroborates the
range with at most two queries when both optional exports are present.
Missing exports, an unsupported initial query, completely unreported
outputs and a single reported priority level select an ordinary
nonblocking stream. Partial or inconsistent reports and invalid range
ordering keep their typed terminal failures.

With a corroborated range the worker requests its least priority. If
creation explicitly reports unsupported and returns no handle, it
creates an ordinary nonblocking stream once. A returned handle belongs
to the worker's retirement path even when creation fails. Unexpected API
errors and null success handles terminate the device request. These rules
apply to decode, matching, compression and retained uniform work.

Each route requires the same fresh resource evidence, finite quanta and
independent CPU verification. Automatic background dispatch still uses
adapter activity and duty allowances. CPU operation and recovery remain
available through the existing supervisor and request policy.
[Installed-route and CPU recovery rules](GPU_RECOVERY.md#installed-windows-routes)

The CPU SDK probes cover missing required and optional exports, omitted and
inconsistent outputs, valid extrema, explicit unsupported creation and
cleanup after a failed call. The historical control retains its original
header from `5eaa07ae56bd360cecf5b31bcf3806f117540ab7` and its original
failure. The current probe corroborates the omitted report, then creates
and retires an ordinary nonblocking stream through the fake API.

## 26 November Windows qualification

The ROCm 10.2 SDK probe passed 285 assertions in Release and with
AddressSanitizer. The LZ4 and uniform-sweep probes passed 58 and 115 CPU
scenarios respectively. The uniform probe completed a byte-verified
multi-window sweep with both priority exports absent. The separately
compiled compression module passed offline checks for six AMD targets.

The rebuilt worker delivered two verified Radeon extents totaling
128 KiB over 32 native Windows callbacks. The worker exited and the
controller confirmed CPU readback. The retained-route refusal fixture
kept the CPU path available without opening HIP. Both live compression
preflights observed a peak around 100% and declined device dispatch.
Mounted throughput remains open.
[Source, device observations and executed tests](evidence/windows-hip-optional-streams-2026-11-26.json)

At a subsequent admitted opportunity, this same worker produced sixteen
stored extents from 1 MiB of input. The independent native CPU reader
recovered every byte and verified the prior image. In a second fixture,
withdrawing the next device grant after the first window selected CPU,
preserved exact readback and released the worker and locked pages. Initial
readiness took 3.18 seconds of the 3.26-second successful exchange.
This fixture supplies a startup observation for the mounted comparison.
[Installed-device write and fallback evidence](evidence/windows-private-seal-credit-2026-11-26.json)

## Executed priority capability checkpoint

This checkpoint records the earlier corroboration-only implementation.

At b2cf9899417d52c2266d86fa1246b874001376a3, SDK run43432703582 passed235 CPU
assertions across12 priority scenarios. The exact historical probe reported
category1179844613 with one query; the corrected probe reported1179844611 with
two. Neither loaded HIP or executed GPU work. Every original SDK/worker gate,
including20 confirmed exits, passed; core run43432703721 passed all13 jobs.
[Source-bound evidence](evidence/priority-capability-b2cf989.json).
