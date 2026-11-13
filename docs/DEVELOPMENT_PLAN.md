# FlitzFS development execution plan

Baseline: 11 November 2026, main 8a5633b588869eb3808d089fca76937c6781991c.
Read the [repository assessment](REPOSITORY_ASSESSMENT.md) first.
This is the execution plan for the existing active full-product goal, not a
replacement goal or a claim that a plan is a finished filesystem.
[The JSON ledger](DEVELOPMENT_PLAN.json) supplies dependency order, exact exit
criteria and continuation instructions for subsequent agent turns.

## Operating contract

Keep the owner's filesystem test backing exclusively in bounded locked RAM.
No raw-device, partition, boot, owner-driver, privileged, registry, persistent
service or scheduled-task operations. Preserve all owner data and dirty source.
Owner GPU use remains tiny, synthetic and conditional on exact-source isolated
qualification and an audited bounded harness. Never repeat an unchanged failed
experiment. Confirm every owned process/handle/locked allocation retires.
Small explicitly authorized HDD source/tool/report staging is not filesystem
backing. Driver-backed owner mounts remain prohibited.

Development remains main-only, without branches, PRs, force pushes, release tags,
product numbering or automatic publication. The owner controls release. Apply
the mandatory efficiency, source-routing and research skills. New important
behavior requires current reputable primary-source research and an actual
reviewed Crawl4AI capture for webpages; native APIs/archives retain their native
transports. Existing research captures must retain their true publication dates.

The current worker/batch/namespace limits stay in force until an explicitly
reviewed replacement passes equivalent fault and resource tests. CPU semantics,
SDK compilation, actual GPU execution, accepted GPU bytes, mounted behavior,
performance and power-loss durability are separate evidence categories.

## Execution order and harness integration

P00 and the bounded G01 admission package are complete. G02 now has an actual
native GPU proposal/compression composition checkpoint at 645a1d1, but no measured
benefit. Prioritize D01 causal retirement repair, then G03 pipeline/lifecycle
work and useful G02 compression candidates. G03's entry gate is the demonstrated
G02 integration checkpoint, not completion of G02's still-unmet benefit exit.
No delegation is implied. S01 and L01 supply the
storage foundations. A01–A04 implement the resource governor, adaptive foreground
policy, persistent learner and collective optimizer in that order. R01 and W01
complete recovery and Windows integration; Q01 gates comparative/security claims.

For each continuation: verify current main and CI, read only the selected
package's applicable contracts, inspect the smallest relevant source scope,
implement and test one bounded substantive mechanism, retain failures, and
update the compact ledger with exact source/evidence. Do not repeatedly replan
unchanged work or turn documentation passes into product completion. No
calendar, compression ratio or speed promise substitutes for an exit criterion.

## Work packages

### P00 — Keep authoritative evidence and an executable continuation plan

Status at publication: complete in the commit publishing this plan. Dependencies: none.

Deliverables:

- Refresh the stale state ledger, preserve historical qualification and publish this source-bound assessment and plan.
- Record compact phase evidence, exact source hashes and changed-scope reads as the continuation method. Record secure immutable candidate qualification as a future CI efficiency investigation; no CI gate is changed by this publication.

Validation:

- Validate JSON, dependency DAG, referenced source identities and tree changes; preserve LICENSE, workflows and owner edits.
- Record the plan publication separately from runtime implementation. Do not mark future packages complete.

Exit criterion: The assessment, plan and current state are published coherently on main and the next implementation package is explicit.

### G01 — Admit useful GPU work with live resource envelopes

Status: complete for the bounded 04d884b admission checkpoint. Dependencies: P00.

Deliverables:

- Define one shared memory/admission contract for worker, core result gate and match controller. Collect current physical and commit headroom, both DXGI segment budgets/usages, workload footprint, identity and freshness.
- Derive bounded envelopes from those observations and independently enforced safety limits. Distinguish supported temporary resource pressure from unknown device faults; preserve immediate CPU-byte selection, event/evidence-driven retry, quiescence and terminal per-request behavior.
- Keep the current batch bounds and independent CPU comparison. Do not hardcode this owner's GPU, RAM, drive, observed 176.6/12.2 MiB usage or capacities.

Validation:

- Table-driven fake-API cases: missing/stale/overflow telemetry, shrinking budgets, concurrent pressure, unified/shared memory, identity changes, faulty device, cancellation and incomplete retirement.
- Exercise all three admission sites with identical cases; raw-only, codec, sanitizers, Windows native/worker fixtures and SDK compilation.
- After exact-source disposable-VM qualification and harness audit, at most a tiny synthetic owner test with confirmed cleanup. No unchanged retries.

Exit criterion: A bounded successful candidate is admitted when justified by fresh evidence; every unknown or unsafe case returns independently verified CPU bytes without repeated device searches. This is not proof of native compression speed.

### D01 — Resolve intermittent hosted writer/child retirement failures

Status: in progress; cause remains unproven. Dependencies: P00.

Deliverables:

- Add bounded phase, wait, cancellation and process/job ownership evidence to isolated crash and crash-restart fixtures.
- Reproduce the observed 15-second operation plus 2-second unconfirmed-exit behavior. Inspect acknowledgement, dispatcher-stop, pipe/object ownership and cancellation order without assuming a deadlock.
- Fix the cause and guarantee cleanup/retirement ordering or explicit terminal failure; retain bounded logs.

Validation:

- Regression must fail for the observed cause and pass after the fix; repeat the affected isolated scenarios with bounded resource accounting.
- Verify cancellation at every owned lifecycle phase and absence of stale workers/handles/locked pages. A green rerun or longer timeout alone is insufficient.

Exit criterion: The earlier failures are explained by captured evidence and a targeted regression; repeated isolated qualification confirms child exit and cleanup.

### G02 — Connect qualified GPU work to native filesystem compression

Status: in progress; actual integration works at 645a1d1, benefit not demonstrated. Dependencies: G01.

Deliverables:

- Qualify the actual match-proposal to native lossless encode route and map which native formats have genuine GPU execution.
- Retain immutable input, CPU-readable encoded output and byte-exact independent verification. Unsupported codecs and runtime faults select CPU immediately.

Validation:

- Mixed synthetic and bounded public real data: offsets, truncations, overlaps, malformed proposals, incompressible bytes and corruption.
- Separate CPU emulation, SDK compilation, actual kernel execution, result admission and filesystem use in reports.
- Measure complete cold and warm write/read paths, output image bytes, metadata, validation, transfers and fallback costs.

Exit criterion: A native filesystem operation demonstrably uses GPU work with exact recovery and measured end-to-end benefit in stated conditions; otherwise retain CPU routing and record why.

### G03 — Reduce GPU pipeline overhead while preserving containment

Status: pending. Dependencies: G01, D01; entry gate: demonstrated G02 native integration (645a1d1).

Deliverables:

- Instrument process/runtime/context/module startup, transfer, kernel, comparison, native encode and retirement separately.
- Research and compare safe amortization, bounded batching and verification strategies. A resident worker or relaxed batch cap requires a reviewed replacement lifecycle/security contract and equivalent fault containment before implementation.

Validation:

- Matched alternating cold/warm CPU/GPU trials with confidence/noise reporting, responsiveness and live resource evidence.
- Test shutdown/cancel/driver disappearance in isolated VMs or mocks only; never stress owner WDDM or leave idle workers without a justified bounded lifecycle.

Exit criterion: Measured whole-operation gains survive the safety and correctness gates; no optimization depends on omitting integrity checks or unbounded persistent processes.

### S01 — Scale the store and namespace with bounded incremental work

Status at publication: pending. Dependencies: P00, D01.

Deliverables:

- Design scalable object/extent indexes, allocator, incremental checksums, history reclamation and reader pins. Preserve old wire compatibility or provide an explicit verified migration path.
- Replace whole-state work where measurements justify it; distinguish sparse logical sizes from allocated bytes and enforce quotas/reserves.

Validation:

- Small-RAM sparse/mock backends exercise large logical address spaces, adversarial indexes, interrupted allocation/reclamation and reader pin races.
- Legacy CPU reader, create-new output, capacity exhaustion, rollback and independent recovery tests; no giant owner images.

Exit criterion: Explicit scaling targets and measured resource bounds pass without merely increasing constants or abandoning recovery.

### L01 — Persist bounded crash-safe optimization state

Status at publication: pending. Dependencies: P00.

Deliverables:

- Define a small accounted internal area for workload summaries, policy versions, confidence and bounded benchmark history, writable only through controlled helper/driver transactions.
- Treat advisory learning state as rebuildable. Dictionaries/models required to decode data are immutable referenced format objects with their own integrity, retention and durability rules.
- Specify size caps, wear/write budget, transactional publication, rollback and eviction before adding any learner.

Validation:

- Torn writes at every commit boundary, stale/unsupported policy, corrupt records, full reserved area and interrupted compaction.
- Recovery preserves acknowledged user data; damaged advisory state safely rebuilds, while missing decode-critical objects are detected and recovered only from verified redundancy.

Exit criterion: A CPU-readable bounded optimization area survives the defined fault model without making data readability depend on a mutable learned model.

### A01 — Discover capabilities and govern foreground/background resources

Status at publication: pending. Dependencies: G01, L01.

Deliverables:

- Discover supported CPU features, processor availability, live CPU/GPU pressure, physical/commit/VRAM headroom and storage characteristics without installation-path or owner-hardware assumptions.
- Use a bounded event/evidence-driven governor with low-priority CPU/I/O/GPU work, small admitted units, cooperative yield, fairness and bounded debt. Timers may bound operations but cannot substitute for a retry or idle decision policy.
- Resolve missing optional dependencies safely in verified private locations where permitted; no global driver/package changes, elevation, services or silent unsupported runtime substitution.

Validation:

- Mock heterogeneous CPUs/GPUs, no GPU, unknown metrics, foreground load, budget changes, suspend/resume and multiple volumes.
- Measure actual foreground interference and immediate yield; enforce hard process-tree memory/CPU/time limits and confirmed cleanup.

Exit criterion: The same OS/architecture runs without host-specific paths/specs; unknown headroom fails closed and background work obeys a documented bounded interference budget.

### A02 — Select foreground compression from measured benefit

Status at publication: pending. Dependencies: G02, A01.

Deliverables:

- Choose useful fast lossless compression on writes; use stronger GPU-assisted work only when measured throughput, resources and reduction justify it.
- Maintain a bounded codec/transform portfolio with raw fallback for incompressible data. Optimize physical bytes, sustained reads/writes, read/write amplification and resource cost together.
- Research current official APIs and recent relevant primary papers before each important algorithm/format change; evaluate competing candidates rather than copying archive software.

Validation:

- Held-out mixed workload evaluation with cold/warm reads, partial writes and pressure; include codec metadata/dictionaries/history in ratio.
- All encoded forms have bounded decoders, malicious-input rejection and independent CPU recovery.

Exit criterion: Foreground policy improves the published throughput/reduction tradeoff over the fixed baseline on held-out workloads, with safe degradation under load.

### A03 — Learn workload policies and run bounded idle microbenchmarks

Status at publication: pending. Dependencies: L01, A01, A02.

Deliverables:

- Implement a local bounded learner over content/workload features, measured encode/decode costs and confidence; include concept drift, version/source identity and reproducible policy decisions.
- Trigger small experiments when fresh resource/work evidence supports them, prioritizing useful uncertainty reduction rather than a fixed maintenance timer.
- Track evaluated image/generation/profile identities and future measurable benefit. Preserve duplicate-work suppression and avoid endless rewrites.

Validation:

- Deterministic policy tests, cold-start/fallback, corrupt learning state, workload shifts, adversarial observations and repeated no-benefit samples.
- Compare against fixed/simple adaptive baselines; bound memory, exploration work, writes and actual foreground interference.

Exit criterion: Learning persists safely and shows held-out benefit over simpler policies without changing correctness or consuming uncontrolled resources.

### A04 — Collectively optimize cold data and layout

Status at publication: pending. Dependencies: A03, S01.

Deliverables:

- Investigate shared dictionaries, duplicate/near-duplicate representations, content transforms and layout/defragmentation against current primary research and relevant baselines.
- Run bounded incremental candidates, verify complete CPU readback, publish transactionally and retain source until reader pins and durability permit reclamation.
- Account for metadata, recovery replicas, dictionaries, temporary candidates and read amplification; stop negative-benefit work and yield promptly.

Validation:

- Concurrent readers/writers, cancellation, resource pressure, interrupted handoff/reclamation, capacity failure and recovery.
- Real heterogeneous held-out corpora plus incompressible/adversarial data; report total image size and steady-state performance, not payload-only ratios.

Exit criterion: Unattended collective optimization produces verified net gains without manual intervention, runaway rewrites or material foreground disruption.

### R01 — Qualify payload repair and acknowledged-write durability

Status at publication: pending. Dependencies: D01, S01, L01.

Deliverables:

- Define failure domains, acknowledgement/barrier ordering, payload redundancy, checksums, journal/root selection and independent CPU recovery.
- Implement corruption localization and verified source-preserving repair. Report irrecoverable loss instead of synthesizing bytes; distinguish metadata repair from payload recovery.

Validation:

- Deterministic fault injection at every persist boundary, short/torn/reordered writes, corrupted replicas, OOM, kill and candidate publication.
- Use disposable VM/backend models for crash tests. Physical power-loss claims need matching storage/barrier qualification elsewhere; owner RAM tests cannot provide it.

Exit criterion: The explicit fault model and acknowledgement contract pass independent recovery with no unexplained lost acknowledged bytes. Universal survival of arbitrary bugs or loss of all copies is not a claim.

### W01 — Complete Windows semantics and helper/application lifecycle

Status at publication: pending. Dependencies: D01, S01, R01.

Deliverables:

- Complete request-bound authority, owner/group, ACL/inheritance, sharing/locking, notifications, cancellation, mapped I/O and unresolved-session recovery matrices.
- Build the helper/app and installation/update/uninstall lifecycle with independent CPU recovery and host-agnostic dependency discovery.
- Qualify latest supported Windows 11 AMD64 client VMs and the selected driver integration; preserve separate mounting gates.

Validation:

- Read-only disposable fixture first, then isolated writable tests with strict existing caps; no owner driver installation or mounted host probe.
- Verify crash/restart, upgrades/rollback, no stale services/workers/tasks/handles and safe uninstallation without touching unrelated files.

Exit criterion: Published Windows capability matrix and reproducible client qualification cover the promised app/driver behaviors; hosted callback success alone is insufficient.

### Q01 — Security and independent comparative qualification

Status at publication: pending. Dependencies: G03, A04, R01, W01.

Deliverables:

- Review threat model, parser/decoder limits, package/runtime provenance, update trust and current Dependabot alerts. Define encryption/authentication/key recovery explicitly before advertising security features.
- Use appropriate recognized benchmarks and reproducible mixed real corpora against several current filesystem/compression baselines with equivalent durability/resource settings.
- Publish failures, noise, unsupported data/operations, total physical costs and exact source/tool/host identities. The owner alone decides release.

Validation:

- Malicious/corrupt input, fuzzing, OOM, partial writes, downgrade/compatibility and independent CPU recovery.
- Repeated alternating trials and held-out corpora; separate RAM-backed logical performance from physical-media and power-loss results. No owner raw-device or physical backing tests.

Exit criterion: Claims match reproducible evidence and unresolved blockers are disclosed. No selected result establishes universal superiority over every filesystem.

## Full-product completion matrix

| Owner requirement | Required packages and evidence |
| --- | --- |
| Windows-only latest Windows 11 app/helper/driver | W01, R01, Q01; client-VM and lifecycle qualification |
| Effective AMD acceleration, immediate smart CPU fallback | G01–G03; actual accepted native work and whole-operation measurements |
| High writes/reads with strong initial reduction | A01–A02; held-out end-to-end throughput and full-image cost |
| Collective stronger background compression and layout | A03–A04, S01; verified net gains with bounded interference |
| Hardware/installation independence | A01, W01; varied/missing capabilities and private dependency lifecycle |
| Persistent local self-learning and idle experiments | L01, A03; crash-safe bounded state and measurable learning benefit |
| Power-loss resilience and self-repair | D01, R01, W01; explicit failure model and independent recovery |
| No resource exhaustion or stale host processes | Every package; hard limits, live headroom, confirmed retirement |
| Evidence of exceptional compression and speed | Q01; fair reproducible comparisons, inclusive storage accounting |
| No owner data loss or physical filesystem backing | Every package; source preservation, RAM-only backing and separate mount gates |

The scope includes arbitrary file types, including incompressible/encrypted
bytes. No lossless algorithm can reduce every possible input; raw fallback and
honest net-byte accounting are necessary. Repair requires surviving verified
information. Future unknown bugs and total loss of all copies cannot be covered
by an absolute guarantee. These limits do not reduce the required engineering
work or authorize risking owner data.

## Executed integration update, 11 November 2026

[Source-bound evidence](evidence/native-gpu-ram-645a1d1.json) records all 13 core
jobs, four CPU-worker composition cases and one owner synthetic RAM/GPU trial.
Accepted/forwarded proposals: 56; selected encodings: zero; CPU/proposed payload:
8,316/8,316 bytes. CPU/proposed profile: 5.4/714.2 ms, with 706.5 ms combined
worker exchange. These are single cold observations, not comparative throughput.

G02 remains incomplete. Startup amortization alone cannot create a size gain
when the suggested encoding is never selected. Instrument CPU preparation,
startup, GPU execution, full CPU oracle, candidate/baseline encoding and
verification separately before choosing an optimization.

D01's current investigation is process ownership. Crash sources already use a
separately supervised external client. The next change converts basic, mutations
and delete while preserving the original workloads and strict independent CPU
recovery. Restart/resumer, RAM-handoff and read-only mounted actor conversion
remain prerequisites to adapter-wide self-origin protection. The upstream report
still has no maintainer confirmation; the historical kernel state is unknown.
Do not widen deadlines, bypass a guard with a fake origin, repeat an unchanged
GPU experiment or treat a green run as a causal D01 fix.

## Current actor qualification and next implementation

[Source6dac1f1](evidence/ordered-actors-6dac1f1.json) passed all thirteen core jobs.
The basic/mutation/delete host, client and independent verifier were distinct;
source and supervisor retirement preceded verification. Original bytes, metadata,
namespace and limits remained checked. This does not establish the historical
shutdown cause or qualify a production mounted filesystem.

The next source extends external clients to ordered restart, both writable
resumers and the read-only normal/kill/remount campaign, adds bounded read-only
controller supervision and requests current primary event/process research.
Its publishing-source qualification is pending. Finish the RAM-handoff multi-round
protocol from admitted research, then enable/test adapter-wide self-origin
protection without a synthetic-origin escape. Preserve every full-product
package and exit criterion; no owner mount/driver or unchanged GPU experiment.

## Finite memory-handoff client preparation

Source9dd6a98 passed all thirteen core jobs and admitted eleven current native
Microsoft event/process captures. Restart sources, both writable resumers and
read-only normal/kill/remount workloads used external clients with exact host/
supervisor retirement before independent verification.

The next publishing source moves the RAM-handoff mounted client into its external
controller through a finite seven-event conversation and preserves every original
handoff oracle. It adds independent CPU sequence and real helper fault/handle
checks; new-source mounted qualification remains required. Then implement and
qualify mandatory adapter-wide self-origin protection, preserving direct CPU-only
test injection without a synthetic-origin escape in live mounts. D01's historical
cause and every full-product package remain open.

## Product origin admission after actor qualification

Sourcefa9fe90 passed all thirteen core jobs, including the finite external RAM
client and CPU peer-fault/strict-handle checks. All existing mounted fixtures
have external data clients. The next source requires explicit session/callback
origin binding, denies host Create/Open before context/native mutation and
tests actual product rejection through the SDK-driver callback path.
CPU-only fake origins are not mounted-fixture evidence. Publishing-source
qualification is pending. Preserve the historical unknown D01 cause and all
full-product exit criteria; this does not enable owner mount/driver/GPU actions.

## Executed parent and current L01 continuation

At 053ade8, all 13 core/mounted jobs passed, including mandatory product origin
admission. See [source-bound evidence](evidence/origin-guard-053ade8.json). D01's
historical cause remains unproven. Independent L01 work now adds a separate
[failed-advice rebuild control](ADVISORY_REBUILD.md) with retained ownership and
live-handle preservation; its publishing-source gates remain pending. Provider
mapping/reclamation, persistent workload-derived write credit and automatic
learning are still required. The full product/GPU/Windows/recovery exits above
remain intact.

## Windows helper advice integration

The controlled rebuild at 745d023 passed all 13 core/mounted regressions and 687
advisory assertions. Its [helper consumer](ADVISORY_HELPER.md) now prepares actual
process-lifetime area ownership, accounting and one checked maintenance rebuild.
New-source qualification remains required. Physical mapping/reclamation,
persistent write credit, authorization and automatic learning remain open; the
full Windows/GPU/resilience objective is unchanged.

At 9977674, the helper recovery consumer passed all 13 core/mounted jobs,
687 core advisory assertions, 461 VM app assertions and a healthy owner GUI.
The next test-only step qualifies four allocator cuts after replacement admission,
including final readback. It does not implement persistent rewrite credit or
advance any full-product package to complete. Source-bound results and current
pending gates are recorded in DEVELOPMENT_STATE.json.

L01's next implementation is explicit persistent native write-input provenance,
with transactional counter/data selection, legacy defaults and no automatic
consumer. Four helper allocator cuts now passed at a892bfa (563 VM app checks),
while its full regression matrix continues. The new workload implementation
requires its own complete source-bound qualification. Spending/debt, loss-safe
baselines, durable mapping and the automatic governor/learner remain required.

At14cfb12 the persistent native workload foundation passed all13 jobs,
3,945 workload assertions/696 modeled cuts per examined log and563 VM app
assertions. The next managed/helper consumer keeps activation explicit and
before NEW helper-volume exposure. It needs its own full integration qualification.
Persisted spending/debt and loss-safe baselines remain next; the full goal is active.

At5b58568 the managed/helper consumer passed all13 jobs,4,645 workload assertions,
590 VM app assertions and a healthy owner GUI. The next L01 source adds persistent
funded advisory settlement and charged loss/repair transitions, preserving legacy
defaults. Its complete source-bound qualification remains required. Durable mapping,
funded lifetime continuation, authorization and automatic governor/learning remain
unfinished; the full goal is active.


At `5db3ae7`, the funded journal passed all 13 integration jobs. Each of the five
core configurations passed 375 budget assertions with 42 actual write/flush
faults, alongside the existing workload and advisory suites. The application
model passed 590 assertions over three balanced lifecycle cycles and its GUI
workflow exited cleanly. See [source-bound results](evidence/optimization-budget-5db3ae7.json).

The next L01 implementation connects the explicit helper to funded creation and
replacement. A checked loss-baseline preflight suppresses candidate allocation
and repeated source verification when input is unchanged; actual rebuild still
independently verifies its authority. Real helper-engine funding, preserved
post-admission allocator cuts and complete new-source qualification are required.
The full goal remains active; durable mapping/authorization, long-lived funding,
the automatic governor/learner and useful GPU gains remain unfinished.


The funded helper at `99d5365` passed all 13 integration jobs and all five core
configurations, each with 398 budget assertions and 42 injected failures. Its
application model passed 728 assertions across three balanced lifecycle cycles;
the hosted GUI and one healthy owner GUI passed with exact retirement. See
[source-bound evidence](evidence/funded-helper-99d5365.json).

Independent G02 candidate evaluation now prepares a closed upstream semantic
scope for current AMD hipCOMP source, with exact source/control identity and
notice retention. This prerequisite enables kernel analysis without changing
runtime behavior. Its admission and actual query execution passed at `905a4d3`,
as recorded below.
Subsequent audited Windows compilation, CPU-readable format recovery, equivalent
worker containment and whole-operation gain measurements remain mandatory.
The G03 lifecycle prerequisite and historical D01 uncertainty remain explicit;
L01 continuation, durable mapping/authorization, automatic learning and every
full-product exit criterion remain unchanged.


The closed candidate navigation at `905a4d3` passed its 22 admission cases and
actual queries in both scopes: 143 FlitzFS files/eight queries and 142 pinned
hipCOMP files/two queries, with zero reported index errors. Relevant LZ4 source
and complete notices were independently byte-checked. Known dependency pins were
current in the native inventory; research-runtime freshness remains open.

The next G02 step is the [optional offline object gate](HIPCOMP_OFFLINE.md).
It retains current SDK staging and all existing worker gates, verifies source
before downloading the SDK, and compiles one private byte-identical translation
unit for a synthetic target. The 20 new admission/staging cases and actual native
compiler outcome passed at `df51d8c`. The object is 217,673 bytes; its digest,
source identities and original SDK/worker results are retained in
[offline evidence](evidence/gpu-lz4-offline-df51d8c.json). Compilation cannot close
CPU-format, runtime, performance or full-product requirements.

The next [allocation planner](GPU_LZ4_LAYOUT.md) accounts for all candidate tables,
inputs, worst-case outputs, guards and scratch within the unchanged 256-KiB total
explicit device-buffer cap. Its new CPU tests and expanded native LLVM object
inspection require exact-source execution. No unchanged owner GPU/GUI repetition
is part of these changes.


At 0879cce, all five core configurations passed the complete allocation planner
(1,908,560 assertions each). Its optional object reader compiled and validated
AMD64 headers, then failed on an advertised unselected LLVM action. The exact
source root cause and failed artifact are preserved; the correction selects the
reviewed read-only metadata path and adds five rejection cases.

The next [independent CPU candidate verifier](GPU_LZ4_CPU_VERIFY.md) reuses the
existing optional LZ4 dependency, checks immutable-source equality in disjoint
bounded scratch, and preserves unavailable/raw-only behavior. New-source
verification remains pending. This is a prerequisite, not GPU interoperability,
stored-format support, automatic filesystem routing or performance qualification.


The independent CPU verifier at 86bc830 passed all five core configurations:
806 assertions with the codec and 389 in codec-disabled builds. The native
read-only object command also worked, but the parser rejected a captured empty
AMD64 host placeholder. The next schema requires exactly that zero-byte host
entry plus one positive gfx1100 image, binds URIs/extents to the object and checks
unchanged compiler outputs. Its 28 admission/metadata cases remain pending.

The next [kernel review](GPU_LZ4_KERNEL_REVIEW.md) also opens a closed three-page
current-AMD warp/model/stream capture scope. Native extraction and actual content
review are required before synchronization or scheduling changes. No device code
or ordinary filesystem path is changed, and all full-product packages stay active.


At 1d20f40, the bounded offline candidate gate passed all 28 admission cases,
AMD64 headers, exact file-bound gfx1100 metadata, unchanged compiler outputs and
all original SDK/worker checks (20 confirmed exits). No module was loaded.
The current-AMD capture needs an observed-page/version/substantive-marker
correction after an absent-literal failure; nine scope cases and corrected
native captures remain pending. Search snippets supply no semantic evidence.

Independent maintenance routing at the same source completed two actual queries
over 148 files, with live confirmation of the explicit native-session API.
Infrastructure hits limit this search; it does not establish an implemented
automatic governor or durable provider mapping. Those full-plan requirements
remain active alongside G02/G03 and every other unfinished package.
