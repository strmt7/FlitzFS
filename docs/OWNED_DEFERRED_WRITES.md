# Owned deferred Windows writes (experimental)

The explicit native adapter can own one pending write on either a legacy or an
indexed volume. The indexed path uses its normal limits for the write, EOF
and allocation. It publishes verified LZ4 advice with the existing inode and
block header in the encoded record. Default application routes still select
CPU. [Indexed path and evidence](NATIVE_WINDOWS_LZ4_ADVICE.md#indexed-native-extents)

The earlier CPU/SDK and disposable mounted response paths passed at a4ad516.
Automatic useful GPU operation remains product work.

Source d384b6c passed all thirteen core jobs, all 53 prescribed SDK commands and
one fixed six-request owner comparison through this service. All three GPU
requests verified and stored sixteen candidates; every CPU/GPU request passed
independent current/prior CPU readback, one injected response and retirement.
Median request times were 24.2311 ms for CPU and 513.918 ms for GPU. Each produced
the same 1,310,720-byte image and 135,648 active payload bytes from the same 1 MiB
input. This case showed no speed or stored-size advantage.
[All trials and scope](evidence/owned-service-comparison-d384b6c.json). The response
sink remains injected; real GPU SendResponse/Explorer delivery and automatic
useful routing are still unqualified.

The explicit --deferred-native-cpu-once invocation uses the same owned-service
source, initial storage, cleanup and timing scope
without launching a worker or querying the GPU. The original
--deferred-native-cpu-fixture now uses the same resource-contained memory
fixture without requiring hosting flags. Receipts retain the historical
hosted_cpu, explicit_cpu or gpu labels for compatibility. The revised SDK check
requires the CPU alias to work with both hosting variables absent and preserve
the explicit CPU storage/retirement result. That changed alias requires new
source-bound SDK qualification; historical results below are not its execution
evidence. The local host is authorized; matched performance claims still require
actual new-source measurement.

The first CPU-mode qualification at 94af72c passed all thirteen core jobs and all
52 prescribed SDK commands, including both invocations and legacy-guard rejection.
One bounded explicit CPU invocation on Windows 11 also passed with complete
readback and retirement. [Evidence](evidence/owned-deferred-cpu-94af72c.json).
Its request reports of
0/16 ms exposed the existing GetTickCount64 granularity; they do not establish
zero execution time. The qualified d384b6c fixture records owned_request_ns with the existing
steady-clock facility over the same scope. Legacy milliseconds remain for
historical readers and timeout control is unchanged.
Both modes also report an unkeyed BLAKE3 source fingerprint, calculated before
the request timing scope. It binds comparison inputs without requiring identical
CPU/GPU compressed streams and does not authenticate the data. The full-process
scope includes that calculation.

The explicit --gpu-deferred-native-compare mode uses the same qualified worker,
ordinal, LUID, node and runtime-digest arguments as the single GPU invocation.
Its fixed order is CPU/GPU, GPU/CPU, CPU/GPU: six requests in one parent process,
each with new identical volume state. CPU trials clear GPU bindings and perform
no GPU query or worker launch. A refusal stops the series immediately; no later
request or automatic retry follows. Before each request, a five-second series
admission window is checked. This is not a five-second completion guarantee: each
request retains its deadlines, and the reviewed outer supervisor still enforces
the complete process wall limit. Use that supervisor, not an unsupervised launch.

Each row retains independent CPU readback and complete retirement checks. The
controller's peak commit figure is cumulative over the shared process job; it
is not a separate peak for each trial. Compare per-request owned_request_ns and
actual image/payload sizes. The one outer-process time covers the whole series
and cannot be assigned to individual CPU/GPU requests. The CPU-only
--deferred-native-cpu-series mode qualifies six successive ownership lifetimes
on the disposable SDK runner without hardware execution. Its source fingerprints,
storage accounting, response and cleanup must match the single CPU case. This
comparison passed the bounded owner case at d384b6c. Three trials per backend on
one fixed synthetic source do not qualify a reusable production cost model.

The observed CPU range was 20.0793–28.1673 ms; the GPU range was
505.7415–713.1048 ms. Each GPU request used a fresh worker. The request clock
includes input copy/enqueue through service stop/join, excluding initial volume
setup and final independent CPU readback. The complete six-request parent took
1,911 ms. Final locked bytes were zero, every transport was quiescent, and the
postflight found no named FlitzFS processes. Driver status was OK and the runtime
digest was unchanged. The bounded System warning/error query found no matching
GPU providers; this does not establish absence of every possible driver fault.

The next implementation must amortize setup over finite useful work and advance
protected collective compression. Do not repeat this unchanged comparison or
enable this slower small-request route automatically. CPU/GPU encoders and sizes
may differ; qualification compares independently decoded source bytes and full
stored cost, rather than requiring identical compressed streams.

## Ownership and ordering

Configuration is allowed only before callback binding. The callback validates
the handle and complete metadata prerequisites, copies immutable input, and
captures the opaque WinFsp request Hint while its operation context is valid.
An accepted request returns STATUS_PENDING and transfers its already owned
input vector into the coordinator. If admission fails, the request retains
its bytes for the synchronous CPU path. Publication uses the stable owned
bytes and captures only scalar completion facts. After verified worker
retirement the owner releases its source, worker frames and candidate
buffers. That retirement is scoped to the write's own worker. An unrelated
active exchange can prevent a new GPU launch while CPU-only completion and
buffer release continue. One owned control thread accesses the persistent
GPU coordinator.

Following native callbacks request CPU fallback and drain pending publication
before accessing the session. An already terminal verified result may complete
normally. This serializes append, overlapping writes, reads,
truncate, rename/delete, flush, cleanup and close. Callbacks wait for CPU
publication, never worker/driver retirement. The shared session lock protects
begin/finish and native mutation; worker polling/verification and retirement run
outside it. One outstanding request bounds the queue. A retiring worker owns
bytes only and cannot mutate the session after publication.

The [configured volume controller](AMD_RUNTIME_DISCOVERY.md#controller-owned-budget-observations)
collects GPU budgets on demand outside that lock. A fresh completion snapshot
must follow independent CPU validation and match the active request and
adapter. Resource unavailability selects CPU while the optional worker
retires. A blocked Windows telemetry call can still delay controller join.
The deferred write has its own finite result deadline and CPU path.

Each completion carries the original Hint, committed status/count and FileInfo.
Publication, response submission and worker retirement are separate facts.
FspFileSystemSendResponse returns no delivery status; a failed delivery must
never replay a mutation. Counters describe attempts, not acknowledged delivery.

## GPU evidence and fallback

The control thread consumes a copied health mailbox. Neither it nor callbacks
discover adapters or query GPU drivers. An independently supervised collector
must supply fresh qualified configuration/resource evidence. After CPU result
validation, a later observation is required before advice can publish. Missing
or stale evidence, unqualified work/costs, busy ownership, cancellation or stop
selects CPU behavior. CPU encoding may differ from GPU encoding; stored bytes
must remain independently CPU-readable.

GPU admission also requires an explicit trusted assertion that the helper and
its dependencies are independently available outside this volume. This flag is
not learning state or proof of automatic dependency validation. Configuration
and helper paths are immutable during service lifetime.

## CPU service cost from ordinary writes

When the build has a configured uniform write package, the Windows volume
owner enables measurement before binding the indexed callbacks. The callback
records successful full-length, extent-aligned synchronous CPU writes. It
uses the same geometry check as measured GPU admission. Append, constrained,
partial and failed writes leave the sample unchanged. An unmanaged or raw
volume and a missing independent CPU decoder refuse measurement.

Each aligned size keeps a rolling three-sample window for its exact
compression policy and backing isolation geometry. Samples contain elapsed
native CPU service time and a monotonic completion timestamp. Timing starts
after callback serialization and includes the native write and file-info path.
The owner can take the newest complete window at a quiet boundary using the
existing activity signal. The record retains no file names, contents or
recovery authority. It provides a workload baseline for a later matched
qualification. [Source-bound Windows evidence](evidence/windows-ordinary-amd-write-demand-2026-11-23.json)

### CPU references from completed writes

The adapter records each eligible aligned write's committed inode revision and
range in a bounded sample history. These scalar receipts require no allocation
or additional backing read in the write callback. Ordinary WinFsp Cleanup can
advance a receipt through an adjacent metadata-only revision. A handle can
close before capture, and an unrelated inode can commit during collection.
[WinFsp Cleanup contract](https://winfsp.dev/doc/WinFsp-API-winfsp.h/)

The owner chooses the newest eligible receipt and, when funded, recent receipts
from distinct files. It pins one checked snapshot under the adapter's try-lock
and reads after releasing that lock. The latest range must match its exact
revision. Superseded older members are discarded. The replay retains the order
and boundaries of the remaining writes.

Receipts can span up to two maximum CPU observation windows. Group size depends
on fresh memory and observed CPU service cost. The controller also requires an
idle opportunity, a quiet foreground epoch and the staged uniform package.
Foreground activity or shutdown cancels preparation. The complete capture,
verification and cleanup consume the existing maintenance budget. The owner
records one verified single-write reference and one grouped reference per
aligned size and attachment.

The private mirrored image uses the captured foreground codec and isolation
geometry. An independent CPU reopen compares every extent against its source.
The owner reports the input digest, byte count, native write time and occupied
image size after retiring scratch.
[Single-write evidence](evidence/windows-ordinary-amd-cpu-replay-2026-11-23.json) ·
[Grouped capture evidence](evidence/windows-ordinary-amd-capture-groups-2026-11-23.json)

### Windows engine activity for installed automatic routes

The existing Windows controller samples the local GPU Engine counters during
eligible background opportunities. The sampler uses the caller's live host
allocation grant and releases its query with the controller. It groups process
percentages by DXGI adapter LUID, physical adapter and engine. The busiest
valid engine determines the adapter's observed load. A second adapter's
sample cannot authorize the target.

A bounded pair uses a sample interval from one to two seconds. A long gap
starts a new baseline. The controller requires a fresh observation and
enough capacity for two quanta of the role's duty with a scheduling margin
of at least one eighth of the busiest engine. It passes that observation to
automatic installed read, shared-base and write-trial discovery. Discovery
filters the matching adapters before pinning the runtime or starting the
identity worker. A renewed attempt requires a new observation. A previously
deferred matching adapter stays deferred while its engine observation shows
insufficient headroom. Other resource, scope and process-ownership checks
still apply. Foreground requests continue through the CPU path.
[Windows activity admission evidence](evidence/windows-gpu-engine-admission-2026-11-25.json) ·
[Microsoft wildcard counter values](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw) ·
[AMD ROCm 10.2.0 release notes](https://rocm.docs.amd.com/en/docs-10.2.0/about/release-notes.html)

The same controller publishes an immutable observation for retained routes.
Installed reads check it before sizing device read-ahead, reserving a child
or submitting a prepared request. Shared-base search checks it before
staging and again before launch. The detached write trial checks the exact
bound adapter at preparation and submission. Eligible idle observations
continue while the trial is parked.

An expired observation pauses new work. A fresh refusal can retire unused
read preparation. Submitted read and shared-base work retain their existing
deadline, CPU verification and retirement contracts. There is no additional
sampling thread or per-request counter query.
[Retained-route validation](evidence/windows-retained-amd-admission-2026-11-25.json)

### Detached Radeon write trials

The detached Radeon arm uses the same mirrored native writer and isolation
geometry as the CPU reference. It admits a finite input with a pinned uniform
worker, current resource evidence and an explicit sweep grant. The immediate
exchange provides the owned input at startup. The prepared exchange checks
resources again after the runtime, stream and slab are ready, before providing
that same input. A stop signal cancels either route.

After exact retirement, the parent verifies and decodes the returned
candidates against the original bytes. The ordinary native writer chooses
which encoded extents to store. For a verified prefixed LZ4 candidate, it
can skip computing the CPU size control when even zero-length candidate
payloads would start the transaction's index at the same offset. All other
record sizes and the number and width of index entries remain fixed. A
different projected offset runs the ordinary CPU comparison. Invalid
advice uses the independent CPU path.

The complete image is opened and verified by the independent CPU reader.
The observation records occupied image bytes, stored device candidates
and the subset stored without re-encoding a CPU control. The proof covers
the current protected image extent and write bound. Settled size after
repacking remains a separate comparison.
[Allocation proof and validation](evidence/windows-verified-lz4-allocation-2026-11-25.json)

The measured write interval
charges the complete worker exchange and native transaction. Independent
private reopen provides evidence of correctness outside that interval.
[Windows trial evidence](evidence/windows-amd-detached-write-trials-2026-11-23.json)

A prepared exchange loads HIP, creates the stream and makes its bounded
buffers available before requesting the first work. It serves a single
group today. A supervised worker can reuse the same HIP state across a
finite burst. That extension needs fresh memory and device admission at each
handoff, per-group checked ownership and independent validation, responsive
cancellation, and confirmed retirement when demand falls or pressure rises.
Quantify the savings using real completed requests, including the cost and
idle residency of any early preparation.
[AMD initialization](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/how-to/hip_runtime_api/initialization.html) ·
[AMD stream lifecycle](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/reference/hip_runtime_api/modules/stream_management.html)

### Single-extent prepared write on Radeon

The standalone SDK executable initially reported that its application-relative
AMD package was unavailable. Staging the build-bound `FlitzFSAMD` files
in a privately owned fixture restored discovery with one identity query.
The first immediate device write verified successfully. The prepared
worker then refused a stale pre-initialization memory observation and
retired. A subsequent fresh observation admitted the full comparison.
The later worker can renew an otherwise admissible expired observation
before initializing HIP. [Startup budget policy](GPU_RECOVERY.md#fresh-observations-before-worker-startup)

On the 64 KiB CPython extent, the paired retry measured 0.389 ms on CPU,
3.663 s for immediate Radeon and 2.311 s for prepared Radeon including
startup. The prepared worker reached readiness in 2.258 s and took
52.4 ms from readiness to verified output. The CPU image occupied
216 KiB. Both Radeon replays stored one device candidate and occupied
224 KiB. All three images passed CPU readback. The shape favors CPU.
The next comparison needs an ordinary mounted workload with enough
useful demand to amortize preparation and preserve recovery.
[Measured Radeon comparison](evidence/windows-packed-inodes-and-radeon-2026-11-25.json)

### Grouped native write replay

The detached replay can send several complete writes in one worker exchange.
The prepared input records their request IDs and boundaries alongside
the combined source. Verification checks both the combined bytes and the
member map. The mirrored writer stores each member in its own inode and
transaction. An independent CPU reopen verifies every member before
the group can produce an observation.

The group is sized against live memory and the worker's finite grant.
Each individual write retains the ordinary native transaction bounds. The
observation reports the full private image footprint and accumulated native
write time. The AMD arm also charges setup and the full verified exchange.
Single-write route selection accepts only single-write observations.

The Windows fixture verified three files and two stored candidates using a
CPU-semantic worker. A separate five-write CPU fixture verified a 5 MiB
aggregate in both compressed and raw builds. Cancellation and changed
boundaries are covered by the same suite. The first live comparison found a
zero divisor in fixed-budget allowance calculation. The native replay and
fixture now share the sized worker admission rule. The corrected trial stored
two Radeon-produced extents in each of two device arms and independently
recovered both files.
[Grouped replay evidence](evidence/windows-amd-grouped-write-replay-2026-11-23.json)

The diagnostic comparison can send two equal-sized native writes as separately
admitted requests to one worker. The caller checks host and device conditions
before each handoff. It verifies each reply against its own captured input and
request identity after the worker retires. The mirrored writer stores both
files in independent transactions, then reopens them with the CPU reader.

The Windows Release and AddressSanitizer fixtures reopened three files and
stored two independently framed candidates using a CPU-semantic worker.
The private Radeon replay admitted both 64 KiB requests, stored two
GPU-produced extents and reopened both files byte for byte. The CPU control
and each device arm occupied 245,760 bytes. This comparison uses explicit
acquisition. The controller can also select the separate-request path with
a qualified package.
[Separate-request replay evidence](evidence/windows-resident-native-write-replay-2026-11-24.json)

### Writes captured after worker readiness

The version 6 native replay starts with one captured file and a grant for
a finite number of same-sized requests. Planning reserves room for the
largest supplied prefix, including the source bytes, reply framing and the
private replay. After the first request, a producer can supply a new owned
capture at the next readiness boundary. The recorder rechecks admission
before the capture and before providing its bytes. An exact size and a
strictly increasing request identity bind each supply.

Once the worker exits successfully, transport and native verification must
agree on the number of supplied requests. Each reply is decoded against
its own capture. The recorder builds the private image from the verified
prefix and uses the CPU reader to reopen every file before recording a
result.

The Windows callback fixture committed its second file after the Radeon
worker asked for the next request. The original volume used the
memory-volume provider, so capture selected a checked concurrent-read
snapshot. The fixture reserved three requests, supplied two 64 KiB writes
and reopened both files independently. It also finished a separate burst
after one request. The duplicate-identity case cancelled the GPU replay
and recovered both acknowledged files on CPU. The existing immediate,
grouped prepared and version 5 exchanges also passed through this build.
[Arriving native write evidence](evidence/windows-native-arriving-write-burst-2026-11-24.json)

### Controller-owned paired trials

With an approved [uniform write trial profile](AMD_RUNTIME_DISCOVERY.md#uniform-write-trial-profile),
the controller can take the same captured closed-file writes through both arms.
It checks inode revisions in the pinned snapshot before charging each member
to the byte, service-time and memory grant. The CPU arm verifies the private
image first. The existing acquisition slot owns the captured bytes while the
Radeon arm reuses its prepared stream across the group's bounded launches.
The parent takes the response after confirmed retirement and verifies every
byte independently. Matching compares the ordered member digest, transaction
count and measured shape. Both arms replay the active image's encoded
isolation unit, bank layout and root spacing with its indexed data layout
and compression policy. The image extent includes the private image roots,
pages, protected data copies and allocation padding. The enclosing volume
selector and provider allocation are separate measurements.

When the profile qualifies separate admission, the controller can give each
same-sized captured write to the same worker as a separate request. The shared
plan charges the combined owned source, all reply frames and verification
views before it starts the acquisition. It also supplies the budget check used
during periodic host observation and before each request. Current GPU memory
and device identity govern every handoff. The grouped exchange handles
profiles and captures that do not qualify for separate admission.

The controller monitors live CPU and memory during the scoped trial. New
foreground activity cancels active device work. Shutdown or lost host
headroom also requests cancellation. The controller settles the trial before
making its status visible. Timings include discovery,
host preparation, readiness, supervised exchange, parent verification and
private replay, with whole-arm times alongside. A verified pair finishes
calibration for its initial write size under the current storage policy.
Other sizes can qualify after the shared cooldown and new demand. A change
to the active image's layout or compression policy invalidates captured sources
and completed sizes for the old layout. Settlement checks the active layout.
Further attempts after an infrastructure refusal require changed
inputs where applicable, fresh writes and pacing from the complete cost.
Enabling a foreground route still requires repeated matched measurements for
the supported request geometry.

A single prepared exchange now launches from the planned layout and
capacities. After the worker signals ready and the host rechecks admission,
the owner packs the captured plaintext into the immutable request. It
charges packing, submission, transport, retirement and independent
verification to the post-ready interval. The whole-arm timing still
includes setup, and the private replay records its write and readback
separately. This interval describes a detached prepared request. Foreground
qualification also needs real callback service costs and repeated
matched outcomes. [Source-free prepared validation](evidence/windows-source-free-write-trials-2026-11-25.json)

The initial real-host 1 MiB control captured two committed writes and verified
sixteen Radeon-produced extents with the independent CPU reader. Both private
images occupied 488 KiB. CPU replay took 4.02 ms. Radeon replay took 3.00 s,
with 2.92 s attributable to readiness and 0.44 s of separate discovery.
The initial fixture flagged a higher post-run handle count. Separate signed
runtime and DXGI metadata probes measured initialization effects. Two full
scoped Radeon runs then recovered all sixteen device extents in each run.
Their parent handle counts settled at 326. Two foreground cancellation
scenarios recovered the acknowledged writes after cancelling discovery.
The timed CPU replays took 3.66–4.02 ms and Radeon took 1.52–2.60 s before
adding discovery. The prepared worker spent most of that interval reaching
readiness. The subsequent combined build deferred a fresh GPU run during
host contention.
[Windows background priority](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setthreadpriority) ·
[Controller evidence](evidence/windows-owner-amd-write-pairs-2026-11-23.json)

On the RX 9070 XT, two controller-owned trials each submitted two independent
64 KiB requests through one worker. Both files reopened exactly and both GPU
extents were stored. The CPU and GPU images each occupied 253,952 bytes.
The legacy package completed two 1 MiB groups using its single grouped
exchange, with sixteen verified GPU extents in each group. In two interruption
runs the foreground write cancelled device acquisition before the first GPU
request and recovered the acknowledged files. The complete device arm exceeded
the CPU cost for these small workloads. The scoped callback trials did not
mount a filesystem.
[Separate admission through the controller](evidence/windows-controller-separate-request-admission-2026-11-24.json)

### Reusing the worker for a later write

An approved `await_later_writes` profile can start with one ordinary,
already-committed write. After completing a request, the worker keeps its
initialized HIP runtime, stream and bounded working buffer while it sleeps
on a control event. The wait is limited by recent same-sized write intervals,
the measured time to first readiness, the remaining exchange budget and the
host observation window. The worker issues no GPU work during this wait.
[AMD HIP initialization](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/hip_runtime_api/initialization.html)

Foreground callbacks continue while the worker is parked. The controller
offers a single newer same-sized capture after checking the revision, fresh
CPU and memory observations and the foreground epoch. Device admission is
checked again before supply. When the window expires normally, the worker
releases its resources and returns the completed requests. Shutdown or
refusal discards the speculative result. CPU replay uses the exact verified
prefix, and the original writes remain readable independently throughout.

The controller fixture on the RX 9070 XT covered a later write, expiration
with one completed request and shutdown during the wait, twice each. Every
original file reopened byte for byte on CPU and every worker retired. In the
two later-arrival fixtures, paired CPU replay took 1.00–1.15 ms. The GPU arm
took 2.68–2.75 s including readiness and the arrival wait. Discovery has its
own timing. These measurements describe the two synthetic 64 KiB
writes in a driver-free callback fixture.
[Controller arrival evidence](evidence/windows-controller-arriving-write-bursts-2026-11-24.json)

An approved `allow_smaller_writes` profile can capture a shorter committed
write while the worker is parked. The initial write funds the largest
per-request stream and transport allocation. The owner considers the latest
commit even if its size has no calibration history. Starting a worker
still requires the normal rolling samples. The continuation must share
the storage policy, belong to the current source revision and fit the
original request allocation. A larger arrival stays on its original CPU
path. The private replays compare exactly the ordered set and lengths that
were accepted by the worker.

On the RX 9070 XT, two controlled and two public-source callback runs each
reused one worker for 128 KiB followed by 64 KiB. Each paired replay
verified all three extents and both originals reopened on CPU. The
oversized controls kept a 128 KiB arrival on CPU while the 64 KiB worker
drained normally. Equal-size controls still replayed both files. CPU
replay took about 1–2 ms in these examples. Complete GPU replay took
2.45–4.63 s, including readiness and the admission wait. The callback
fixtures exercised the private replay and original CPU volume.
[Mixed-size controller evidence](evidence/windows-mixed-size-resident-worker-2026-11-24.json)

A single RX 9070 XT controller completed a 64 KiB comparison and, after
the shared 439-second cooldown, a sealed 1 MiB comparison. Ordinary writes
during cooldown still completed on CPU, and writing another 64 KiB file
after cooldown did not repeat the small comparison. An independent CPU
reader recovered all four acknowledged files. The larger private CPU replay
took 7.20 ms. Its complete GPU arm took 2.56 s plus 289 ms of route discovery.
This workload continues to favor CPU.
[Geometry calibration evidence](evidence/windows-amd-write-geometry-calibration-2026-11-24.json)

## Activating a route on a running volume

The Windows volume controller can receive an independently acquired foreground
route while callbacks are already serving CPU requests. It applies the offer
at a quiet foreground boundary. The reported outcome stays pending while a
callback owns the volume, and callback completion wakes the owner. The adapter
requires an indexed managed volume, a pinned uniform worker and runtime, a
matching measured write size and storage policy, and an independent CPU
decoder. It binds the existing budget event before admitting the first
deferred write. Each deferred admission rechecks the active layout inside
the volume lock before copying input or asking the device for a budget. A
layout change sends the next eligible write directly through the CPU path.
Shutdown keeps ownership until publication and worker retirement settle.
[Layout replay and fallback checks](evidence/windows-foreground-write-layout-2026-11-25.json)

The live SDK callback fixture wrote a marker on CPU, rejected an offer with a
different codec policy and applied the matching offer without remounting. An
aligned 1 MiB write used the AMD worker. The independent CPU reader verified
all sixteen extents, the retained prior state and the marker. The worker
bodies matched all 32 mirrored records for the accelerated write. The
activation test used explicit diagnostic costs.

A separate matched fixture wrote 1 MiB of public FlitzFS source bytes in
three alternating pairs. CPU callbacks took 4.37–4.85 ms and occupied
3,145,728 bytes per image. The finite AMD callbacks took 2.23–3.42 s and
occupied 3,276,800 bytes. Every arm passed independent CPU readback.
The measured workload selects CPU. Automatic workload qualification and
mounted operation remain to qualify.
[Windows foreground activation evidence](evidence/windows-amd-foreground-activation-2026-11-21.json) ·
[WinFsp 2.2 response contract](https://github.com/winfsp/winfsp/blob/ada6de157df4fa99f5a866b6ac7af66918d6d838/inc/winfsp/winfsp.h#L1220)

Shutdown first selects CPU completion, sends the single response, cancels and
observes worker retirement, then joins the owned control thread before stopping
the dispatcher/deleting its filesystem. Cleanup does not depend on mutable
hosted-environment flags. Unconfirmed retirement disables the service; it cannot
silently retry or authorize recovery. Existing isolated worker supervision may
retain its own transport frame until actual child exit; it retains no filesystem
reference. No perpetual service, scheduled task or driver change is added.

### Preparing for a later write

The measured indexed write route can require a ready worker. A successful,
matching CPU write requests preparation without supplying source bytes.
The control thread starts one supervised worker only after funding the
whole measured shape. The child may initialize its runtime, stream and
buffers while the CPU remains available to service callbacks.

A subsequent write can use the prepared worker if the protected layout,
complete extent shape, current budget and device work grant agree. The
worker must be ready and retain enough of its original job lifetime for
the qualified service window. A missing, expired or mismatched slot takes
the CPU path before the caller's data is copied for deferral. The complete
worker reply is still collected after exact retirement and checked on CPU.
The volume alone owns commitment and the callback response.

Unused preparation is retired on a resource refusal, scope change,
expiry or shutdown. A new preparation requires successful matching CPU
work. A process that fails setup makes the route unavailable for the
current service. Readiness evidence and immediate-launch evidence qualify
their own admission modes. Default installed activation still requires a
matched client comparison establishing benefit.
[Prepared write evidence](evidence/windows-prepared-foreground-write-2026-11-25.json)

### Event-driven write service

The owned write thread waits for its private activity event, the exact
exchange supervisor, or the remaining service deadline. Direct CPU
completion, new budget evidence and shutdown wake the activity event.
The owner removes the signalled supervisor from the wait after validating
the reply. Publication still needs an observation taken after CPU
validation. A callback waiting for forced CPU completion waits only for
the native publication state. GPU retirement follows that result.
An OS wait failure disables the optional service and completes its
still-owned request from the original CPU source.

An unused preparation also watches its own supervisor. A worker that
fails before readiness releases its slot when retirement is confirmed.
The fixture uses a 12-second job with a child that rejects preparation.
It verifies early retirement, CPU reads of both writes and no new GPU
budget request. The prepared acceptance, pressure refusal and shutdown
cases remain covered by the same SDK harness.
[Windows result and source evidence](evidence/windows-deferred-write-events-2026-11-26.json) ·
[Windows event and thread waits](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects)

## Driver-independent control target

The owned service now builds as flitzfs_native_write_control. It consumes the
pinned WinFsp ABI headers without linking its runtime library or calling SDK
functions. Direct construction requires both a hint reader and a response sink
before starting a thread. Missing either rejects before storage access.

The actual adapter supplies its SDK functions when callers leave those options
empty. Existing mounted behavior and explicit fixture injection are preserved.
This separation prepares GPU service qualification without loading the WinFsp
DLL or a driver in the diagnostic process. The changed source still requires
exact-source qualification; it is not new hardware evidence or owner-trial
permission.

## Optional advice receipts and driver-free composition

NativeUniformWorkerBinding.capture_advice_receipt enables a compact integrity
receipt for verified encoded advice before storage publication. It records the
first payload's length and unkeyed BLAKE3 digest and whether every offered
payload has that same representation. It neither authenticates data nor changes
the stored format. Disabled normal writes perform no additional payload hashing.
Nonuniform representations remain fully supported; their receipt simply cannot
establish the repeated-source diagnostic's single-fingerprint comparison.

The service retains the committed result through completion_receipt_locked().
Callers must own its control lock. Accepting a new request clears the preceding
result. These fixed-size advisory facts do not retain callback buffers or
decoder dependencies. Their retained copies are included in resource accounting.

The existing optional runtime probe adds --deferred-native-cpu-fixture and
--gpu-deferred-native-once. Both exercise one owned 1-MiB synthetic request,
overwrite the caller's original buffer after enqueue, require one commit-bound
response, join the service, and independently recover current and prior bytes.
The GPU mode additionally compares stored encoded payloads against the observed
advice fingerprint. A codec identifier alone is insufficient: CPU fallback can
also encode LZ4. These are unmounted diagnostics, not ordinary Explorer I/O.

The explicit GPU fixture supplies a manually reviewed configuration-bound grant
for the same finite two-block/64-KiB quantum and sixteen-block source shape.
It is not a reusable measured cost model; cost_profile_qualified stays false.
Its values cannot become automatic routing policy or authorize other workloads.
Fresh telemetry uses the supplied verified LUID/node, never adapter-order scans.
Only the diagnostic supervisor samples DXGI; the service can select CPU without
waiting for that sampling code. General product collectors still require their
own supervised lifecycle and qualified costs.

The private SDK fixture stages pinned ABI headers on a disposable runner and
links only the control target. Loader metadata must contain no WinFsp or AMD
runtime dependency in the parent probe; the runtime presence check also refuses
an unexpectedly loaded WinFsp DLL. No WinFsp runtime is packaged. The exact
upstream license is retained alongside the existing dependency notices.

A verified GPU result and matching stored payload are distinct report fields.
The owned_request_ms scope begins before input copying/enqueue and ends after
confirmed control-thread exit. It excludes volume setup and the final independent
readback. Image and payload accounting occurs after native context cleanup/close.
Unknown hardware execution after a rejected child is null, not a fabricated
false. CPU fallback means controller source selection; storage can independently
choose a smaller CPU encoding. Source-specific core and full SDK qualification
are mandatory before any owner experiment. The narrow source-d384b6c hardware
comparison is qualified; this is not an automatic GPU route or a general
filesystem performance result.

## Qualification

Source [94aaa2a](https://github.com/strmt7/FlitzFS/commit/94aaa2a772e03689044df4c0b7504903faa9d553)
passed all thirteen [core jobs](https://github.com/strmt7/FlitzFS/actions/runs/43692752566),
154 native groups (57 deferred and 226 controlled-write checks), and the full
[SDK gate](https://github.com/strmt7/FlitzFS/actions/runs/43692765002).
The new driver-free owned-service CPU fixture passed on the disposable runner.
The one bounded owner GPU case encoded 1 MiB into 135,344 candidate bytes,
matched their stored digests, independently recovered current/prior data, sent
one injected response and joined the service. Locked memory returned from 4 MiB
to zero; package inputs remained unchanged and no named FlitzFS processes remained.
The request scope took 641 ms; the complete 1,310,720-byte image exceeded the input.
No competitive speed or total-size advantage follows from this synthetic case.
The [receipt](evidence/owned-deferred-gpu-94aaa2a.json) retains source, package,
telemetry-failure, adapter-identity, retirement and postflight evidence.

Source [a4ad516](https://github.com/strmt7/FlitzFS/commit/a4ad5166d5063d820cfdd704a600ea20a23f1d93)
passed all 13 [core jobs](https://github.com/strmt7/FlitzFS/actions/runs/43633235875),
154 native test groups (54 new deferred checks) and the complete
[ROCm SDK gate](https://github.com/strmt7/FlitzFS/actions/runs/43633243098).
The real CPU-only RAM-handoff mount passed and retired: zero contexts and locked
bytes, observed client flush, exact independent readback. Digest-verified
[receipts](evidence/owned-deferred-a4ad516.json) retain the detailed scope.


The direct SDK test uses four bounded synthetic CPU cases: deferred completion,
CPU-semantic verified worker advice, held-worker fallback triggered by a read,
and queued shutdown with concurrent stop callers. It checks immutable input, exact response identity/facts,
current/prior independent readback, one completion and joined resource ownership.
It does not execute a GPU or create a mount.

The existing disposable hosted RAM-handoff mount scenario explicitly enables
CPU-only deferred writes. Its real client/SDK response path must complete,
retire and preserve recovery; its report schema and resource limits are unchanged.
All other existing scenarios remain default CPU behavior.

Primary API semantics and exact captures are recorded in
[the research receipt](evidence/native-async-write-research-2026-11-14.json).
The historical [bridge plan](ASYNC_NATIVE_WRITE_PLAN.md) describes the preceding
design boundary. Finite cross-operation batching, automatic costs, actual GPU
callback qualification, scalable metadata, collective compression, persistent
learning and release qualification remain required. The one separately recorded
owner GPU case does not establish production data safety or arbitrary-failure
survival.
