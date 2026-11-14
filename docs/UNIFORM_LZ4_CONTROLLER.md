# Resource-admitted uniform worker controller (experimental)

This controller connects version-2 owned sources, sized transport, the typed
worker's independent CPU verification and the existing evidence-aware recovery
policy. It is explicit internal infrastructure; normal Windows operations do
not launch it yet. No actual uniform GPU execution is claimed by CPU tests.

## Admission and complete accounting

The caller first admits preparation using current resource information. begin
then independently repeats the configuration/recovery/width/time/grid-keyed
window plan and requires the complete owned request to fit that plan. Unknown
costs or a different configuration, recovery generation or profiled width refuse.
Deadline admission uses the selected process lifetime; it does not inherit the
old prototype's 100 ms whole-exchange ceiling.

The controller also checks a concrete parent/child bound against the plan's
host peak: the ENFORCED child Job Object allowance, six request-sized copies
(including one no-larger replacement CPU source), three reply-sized copies,
size/offset directories, one block of verification scratch and supervisor
overhead. A qualified flag without sufficient accounting cannot launch work.
The transport independently repeats actual OS headroom admission and hard limits.

Persistent GpuRecoveryPolicy retains the same configuration suppression,
fresh-evidence hysteresis, device-recovery qualification and integrity quarantine
used by existing workers. Failure does not cause repeated discovery or timer-only
restarts. A new attempt requires exact child AND supervisor retirement.

## Terminal selection and separate retirement

CPU fallback is constant work: change the selected state and return owned source
bytes. It performs no allocation, deallocation, runtime call, OS query, process
signal or wait. Large candidate vectors deliberately remain owned until separate
maintenance or a later nonoverlapping begin. Cancellation remains a separate API.

Validation and acceptance use separate polls. The first successful result is
independently CPU-verified into private owned candidates and constant-time block
offsets, but remains invisible. A newer time and memory observation, obtained
AFTER that CPU validation returns, must still satisfy the deadline and memory
transition before final acceptance. An identical observation cannot authorize
data. CPU selection or expiry between the two polls stays terminal forever.

try_retire is explicit maintenance and is never called by begin or CPU selection.
It may verify a late response with one block of scratch, without copying a late
candidate payload. Valid late data cannot replace CPU data; corrupt late data
still strengthens quarantine. The old exact process/session identity remains
owned until both child and supervisor are confirmed retired.

While an older worker retires, one no-larger CPU replacement source may be adopted
without another launch. Overlapping in-flight work, or a larger replacement,
leaves the caller's owned input untouched. UniformLz4WorkerStart::source_adopted
makes that ownership decision explicit. There is no work queue. Borrowed source
and candidate views expire on the next adopted begin/move/destruction; destruction
must not race calls. Use one control thread.

## Qualification and next integration

Hosted CPU fixtures transfer real multi-block frames through their own trusted
test binary. They cover disabled optional decoding, source ownership, overlap,
fresh final observations, expiry and CPU selection after validation, incomplete
memory accounting, wrong configuration/width, a still-live held helper,
independent replacement CPU bytes, cancellation, late corruption and exact
retirement. These are CPU semantics and synthetic telemetry, not GPU execution,
measured hardware costs, power-loss qualification or performance evidence.
Publishing-source Windows/Linux/raw/sanitizer/SDK results are required.

The authenticated worker package at c96115e passed all 13 core jobs and full SDK
qualification, including 11 uniform admission exits and 38 typed CPU scenarios.
The next integration must acquire real configuration-keyed costs, supply matching
worker grants, compose controlled native Windows writes and perform a separately
admitted bounded actual-device calibration. Actual larger-window GPU execution
and useful end-to-end size/throughput benefits remain mandatory open work.
