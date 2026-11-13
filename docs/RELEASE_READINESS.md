# Release readiness checkpoint — 13 November 2026

Stage: experimental prototype, not a release candidate. The owner alone decides
when to release. No release, tag, package publication or version change is authorized.

The native helper/editor/tray, bounded memory provider, CPU native compression,
storage/namespace primitives and isolated Windows mount/recovery fixtures are
implemented to their documented experimental scopes. Ordinary product file
operations remain CPU-only and read-only Windows routes do not constitute a
production writable filesystem. Numerous passing tests do not measure percentage
completion of the requested product.

GPU acceleration is a mandatory product capability with independent CPU fallback.
It is not optional scope and cannot be replaced by SDK compilation or CPU mocks.
The earlier owner GPU match experiment ran real kernels but selected zero records
and demonstrated no throughput or compression gain. The new LZ4 compute path
compiled at 4dda4fe, whose first CPU fault fixture failed; the corrected
positive/negative matrix passed at df8b08e. The original failure remains retained. No real GPU compression result or
normal filesystem integration is claimed.

Major unfinished release gates are:
- Actual GPU compression through native file writes, CPU-readable persistent
  encoding, byte-exact verification and safe failure fallback.
- Whole-operation read/write throughput and stored-size benefit on varied real
  data, including cold/warm costs, transfers and verification.
- Causal resolution of historical unconfirmed child retirement and complete
  writable Windows semantics, cancellation, shutdown and helper/driver lifecycle.
- Scalable bounded metadata/store operations.
- Automatic resource governance, persistent workload learning and low-priority
  collective recompression/layout improvement with measured foreground impact.
- Acknowledged-write durability and recoverable corruption under an explicit fault
  model; memory/process-interruption evidence alone is not physical power-loss proof.
- Physical/virtual volume creation integration, installer/update behavior,
  security review and independent comparative qualification.

## Correcting development emphasis

The GPU prerequisites are relevant, but their volume has outpaced demonstrable
product integration. Stop using isolated assertion totals, documentation growth
or green infrastructure jobs as a proxy for product readiness. Close the current
compute failure, then prioritize one complete bounded vertical slice:
Windows write -> actual GPU compression -> native stored bytes -> independent
CPU readback -> injected worker failure -> CPU fallback -> exact retirement.
Runtime/module authentication and target coverage remain necessary; do not remove
integrity, resource, recovery or supervision checks to accelerate that milestone.

After that slice is qualified, measure useful size/throughput benefit before
expanding codec complexity or GPU workload. Preserve GPU acceleration as required
scope while measuring when foreground or background execution is advantageous.
Advance scaling, background optimization, durability, Windows integration and
security against explicit product exits. The full development plan remains active.

Use extensive recent primary-source research when a concrete architecture or API
question requires it. Record the question, current source/version, consequence and
validation. Prefer integration and root-cause fixes once those decisions are
supported; repeated unchanged research or host experiments are not progress.

No reliable release date or completion percentage is established. This remains
substantial implementation and qualification work, not final packaging or polish.
A schedule should be revised after the complete GPU file-operation slice and
scaling/recovery results expose the real remaining costs.
