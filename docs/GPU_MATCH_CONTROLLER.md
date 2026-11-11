# Match worker recovery controller

GpuMatchWorkerController connects immutable prepared match input, the existing
GpuRecoveryPolicy and WorkerExchangeSession. A caller supplies its qualified
helper, arguments, workload and externally collected health evidence. There is
no automatic device search, service, retry thread or mounted default route.

A qualifying begin consumes the source and starts one supervised exchange.
An overlapping waiting request rejects before consuming the caller's source.
The policy uses the match search's real buffer bytes. The deadline is bounded
to 100 ms and cannot overflow. Decode and match request IDs share the existing
saturating process-wide allocator.

While waiting, use_cpu_fallback selects ordinary CPU encoding immediately.
It performs no OS call or wait. The immutable source remains available and
selected_proposals is empty. CPU selection is terminal. A later request can
select different CPU bytes while the earlier worker remains supervised. Its
old source stays retained for correct late-result validation.

An accepted worker result must have the bound request ID, exact input digest,
expected execution declaration and independently valid source-byte matches.
Post-run telemetry must match the original adapter/node, be fresh, retain the
memory reserve and show no nonlocal growth. Only then are proposals selected.
A later CPU call cannot rewrite an already selected worker outcome.

Late replies can update the pending configuration's integrity/availability
history once. They cannot change selected work. No new attempt can bypass the
actual child and supervisor exit checks. Missing runtimes and invalid results
block the configuration. HIP cancellation/timeouts retain device quarantine.
Separately qualified recovery evidence is required. Temporary pressure uses
the existing fresh-observation hysteresis, including two accepted rounds
before flapping history clears.

Source/proposal spans remain valid until the next nonoverlapping begin.
The caller serializes controller operations and separately checks the native
candidate's generation before submitting proposals. Ordinary CPU encoding can
complete even while the old worker is still retiring. Confirm retirement or
retain its explicit supervision before starting a replacement.

CPU Windows tests use the same trusted fixture binary and synthetic telemetry.
One fixture declares a HIP frame solely to exercise backend policy. It still
executes CPU code. Tests require selected proposals to reach native
publication/readback, terminal CPU selection, new CPU input during retirement,
quarantine, pressure recovery, invalid telemetry and unchanged final handle
accounting. Actual AMD execution and performance remain unverified.
