# Experimental finite-sweep observations

This extends [the reusable compute scope](UNIFORM_LZ4_REUSE.md). It adds explicit,
opt-in host wall-time observations for one finite operation. It changes neither
ordinary worker frames nor stored codecs and does not enable ordinary file I/O.

## Measurement boundary

`--driver-uniform-lz4-sweep-profile-adapter` uses the same authenticated helper,
adapter/runtime binding, live admission, immutable source, one low-priority stream,
one slab, serial quanta, full synchronization and independent CPU verification.
The explicit parent diagnostic is `--gpu-uniform-sweep-profile-native-once`.

The recorder requires setup once; admission, upload preparation, upload,
kernel launch plus full stream wait, download, CPU/guard verification and fresh
completion admission for every quantum; and successful retirement once. It
records each phase's accumulated wall time, complete window count, minimum and
maximum kernel-wait wall times, maximum admission-to-completion quantum wall
time, and complete compute-scope wall time. State and work per phase are fixed;
there is no allocation, sampling thread or retained per-window array.

These are host wall times including scheduling and synchronization. They are
not GPU event measurements. The compute scope starts after request decoding,
bundle/module admission and entry binding; it excludes reply encoding, module
release, publication and parent acceptance. Full-result assembly/verification
between quanta and final retirement remains in total scope time, even where not
assigned to an individual phase. Parent exchange time remains a separate cost.
Disabled observation makes zero clock queries.

The existing five-second diagnostic observation limit remains. A backward,
throwing or over-limit clock, wrong phase order, failed quantum or unsuccessful
retirement produces no finished trace or successful reply. Observations never
weaken a resource grant, deadline, recovery quarantine or exact retirement rule.

## Separate envelope

The explicit mode prepends a 256-byte `FLUSP001` envelope to an ordinary version-2
uniform reply. It binds request identity, source digest, input/count/chunk width,
adapter ordinal/LUID/node, backend, exact work grant, derived quantum/window/slab
plan, full canonical extent and ordinary reply length. Completion/reserved fields,
phase sums and extrema must be consistent and bounded. Ordinary and single-window
profile readers reject this envelope. The parent independently decodes all returned
bytes and accepts only after successful confirmed child exit and supervisor
quiescence. A complete child trace alone never authorizes acceptance.

## What this permits next

Use source-bound, repeated, varied-content observations with independently
recorded process peaks and fresh resource snapshots to acquire costs. Keep exact
configuration/recovery/shape identity, contention and adverse cases. Missing or
stale observations select CPU; successful observations are not automatically a
qualified cost model. Never derive kernel cost by dividing total exchange time,
extrapolate an untested quantum from a single success, or treat elapsed diagnostic
time as a no-freeze guarantee.

This round does not implement automatic cost qualification, ordinary GPU routing
or collective compression. CPU-generated measurements are not hardware evidence.
Hardware use still requires the complete exact-source isolated gates and a fresh,
audited owner experiment; do not repeat an unchanged previous experiment.

## Encoding independence

CPU fallback need not reproduce GPU compressed bytes, ratio, algorithm or format.
It preserves continued operation and byte-exact independent reads of every admitted
stored format. GPU encoders may use different or more expensive collective/learned
representations; their protected dictionaries, bases or models must remain
CPU-decodable and recoverable. Candidate comparison checks decoded source bytes,
not equality to a CPU-compressed stream. LZ4 is one initial implementation,
not a limit on the cold compression design.
