# Windows helper advisory maintenance (experimental)

The helper now uses the controlled funded journal for NEW memory-volume creation
and failed-area replacement. This source requires its own Windows application,
GUI, resource-retirement and full integration qualification. The underlying
journal at `5db3ae7` passed all 13 core/integration jobs, including 375 funded
assertions and 42 injected journal failures in each of five core configurations.
Those results do not qualify this new helper consumer.

## Admission and file access

`VolumeModel.maintain_optimization_state` remains an explicit serialized
maintenance opportunity. Optimize invokes it before creating an image candidate.
Ordinary saves and reads never audit, allocate or publish advisory state.

NEW helper volume creation explicitly enables native workload accounting, then
creates capability-2 advice using the managed volume's one initial provision.
The existing single-publication allowance is unchanged; creation consumes it.
Healthy identical-state audits verify both actual records without advancing a
sequence, writing records or renewing that allowance.

Damaged advice becomes terminal independently of the file writer. Before
allocating a replacement, the helper calls
`prepare_budgeted_optimization_rebuild`. The first opportunity establishes a
verified current-input loss baseline and forfeits historical entitlement. Later
opportunities with insufficient new input return unavailable before allocating a
provider store. Repeated unchanged opportunities use cached facts only to decline
work and perform no further native snapshot reads.

At least 256 KiB of genuinely new committed input is required for replacement.
A positive preflight is not publication authority: the managed rebuild verifies
current native input and every existing binding, alias, ownership and resource
condition again. It then creates at most one NEW empty area and settles the
charge. A storage or allocation failure does not restore the spent attempt,
bootstrap, old allowance or lost entitlement.

Four total bindings remain the limit. Exhaustion defers optional optimization
without a new area, while healthy file access and existing cleaned handles
remain available. There is no timer, retry loop, hardware search, autonomous
scheduler or automatic image-rewrite authorization.

## Ownership, resources and failure

The helper retains every core-admitted area in its fixed array, including a
partial area whose creation throws. Adoption uses only the core's nonallocating,
no-I/O scalar getter and a statically checked nonthrowing shared-pointer move.
Earlier bytes, reservations and poison-held ownership remain retained.

The private domain, fixed registry, fresh headroom/residency checks, 12-MiB
capacity and existing process/CPU/time limits remain unchanged. At most four
128-KiB advice reservations are included in the aggregate. The view counts every
written advice extent separately from provider allocation. Advisory readiness
is cached diagnostics, not continuous integrity attestation.

Provider capacity refusal after funding preserves the same loss baseline and
current mapping. Released capacity may permit a later distinct opportunity,
which independently rechecks admission. After a core-admitted creation failure,
another replacement requires new input relative to that failed attempt.

Explicit discard retires the session, every advice reference and then the
provider domain. Cleanup failure remains terminal. These process-lifetime
mappings do not survive process termination and provide no physical power-loss
or globally authenticated anti-rollback guarantee.

## Qualification

The actual Windows model suite retains its prior file, retained-context,
source-byte, alias, accounting, capacity, allocator, GUI and lifecycle checks.
It adds repeated unfunded deferral, no provider-growth evidence and real
accepted-input funding before every replacement. The test-only friend writes
bounded synthetic files through the existing shared engine and checks their
bytes and committed input; it never forges counters or changes the product's
editor limit or provider.

The four post-admission allocator cuts still select the measured heap request
for the exact 64-KiB vector type. Their test-only thread-local predicate additionally
requires the expected actual core binding count, using the public nonallocating
diagnostic getter. This prevents newly added preflight verification allocations
from accidentally consuming a journal-publication fault. Each cut must actually
throw after admission, retain the mapping and old bytes, preserve live file
access, defer reuse of the same input and recover only after new accepted input.
The fixture disarms before unwinding and exists only in the qualification binary.

Historical evidence remains in [the original helper results](evidence/advisory-helper-9977674.json),
[managed workload integration](evidence/workload-helper-5b58568.json) and
[the funded journal qualification](evidence/optimization-budget-5db3ae7.json).
The first historical allocation-size fixture at `f4729f1` failed because a
payload-size match missed the STL heap request; its failed run and measured
correction remain recorded. No owner allocation-fault run is authorized.

Durable provider mapping/reclamation, helper authorization, long-lived funded
continuation, a resource-governed automatic learner, protected decode-critical
objects and useful end-to-end GPU compression remain unfinished.
