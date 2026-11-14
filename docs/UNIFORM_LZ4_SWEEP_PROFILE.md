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

## Executed qualification at 11fbf07

All 13 core jobs passed in run 43619821755, including 153 native Windows CTest
groups, raw/codec builds, sanitizers and disposable-VM mount/recovery scenarios.
Full SDK run 43619822169 passed: 3,485 compute checks across 83 scenarios
(10 successes/73 refusals), two raw missing-decoder refusals and 25 confirmed
uniform admission exits. The sweep envelope passed 190 CPU checks. Package
counts now come from checked receipts and advertise FLUSP001 explicitly.

One owner-authorized finite synthetic hardware operation then encoded 1,048,576
bytes into 135,344 payload bytes, stored the exact accepted representation and
independently recovered current/prior bytes on CPU. Eight serial two-chunk quanta
used a 328,288-byte device slab under the unchanged 128-MiB worker limit.
Exchange: 625,472,600 ns; measured compute scope: 467,675,900 ns; setup:
423,295,500 ns; accumulated kernel/full-wait: 21,927,100 ns; slowest kernel/full-wait:
4,414,600 ns; slowest admission-to-completion quantum: 17,776,500 ns.

These are one sample's host wall observations. No reusable cost bound, speed or
compression superiority follows. The complete image was 1,114,112 bytes; the
manual CPU control is not a competitive encoder benchmark. The new
sweep_trace_complete is true; legacy 15-phase trace_complete=false and zero
legacy times mean unsupported/unmeasured, not a failed sweep or zero work.

Worker/supervisor exit, input pins, independent CPU recovery, zero remaining
locked bytes and restored working-set limits passed; no named FlitzFS processes
remained. Runtime hash and reported driver/status were unchanged. A bounded
System warning/error subset had no matching GPU entries; that does not prove
absence of every fault/reset. No owner mount, driver change or physical image
operation occurred. [Full source-bound evidence](evidence/uniform-sweep-profile-11fbf07.json)

Do not repeat this unchanged operation. Next acquire varied, repeated,
configuration/recovery/shape-bound evidence and deliver useful automatic
Windows GPU I/O, scalable storage and real collective compression.
