# Corroborated HIP priority capability (experimental)

Current official HIP 7.16 documentation served under the selected ROCm 10.1.0
scope labels the priority-range API as under development. The exact selected
package implementation was independently read at rocm-systems commit
7da43026e4f2ef7600433c38c820732f7a205d22: it assigns both non-null outputs to
its Low/High enum values. Preserve this documentation/implementation distinction.
The incomplete-report case below is defensive coverage, not an observed failure
of that implementation. Native captures and source identities are retained in
the current-AMD evidence. A successful API status alone
does not supply valid priority metadata or prove scheduling/preemption behavior.

The prior FlitzFS header initialized the two outputs to opposite integer extremes
and rejected an invalid order as device_fault. A success response that leaves
those values untouched therefore receives the same classification as an
explicitly reported invalid range. This is source analysis; the new CPU oracle
must reproduce it before the change is called qualified. It is not an observation
of a past owner-device failure.

The new initialization makes exactly two priority queries for the already
selected adapter, using opposite initial values. Matching results corroborate
the pair without assuming small priority numbers or zero being unsupported.
Omitted, partial or inconsistent reports refuse GPU work as
unsupported_configuration. A stable explicitly invalid order still reports
device_fault. API errors keep their existing typed classification at the
stream_priorities stage. No device rescan, timer, retry loop, stream, batch
allocation or kernel launch is added.

The existing configuration-suppression policy receives the refusal; independent
CPU selection and confirmed worker retirement remain required. No unreported
range is replaced by a guessed default. A corroborated range remains only
capability metadata, not proof of low background impact or guaranteed preemption.

The native CPU probe adds twelve capability scenarios: omitted/partial fields,
zero/negative/extreme valid ranges, inconsistent results and second-query errors.
A focused mode is built against both the current header and the exact prior
FlitzFS header. The historical test-only asset must have Git blob
e8f78c76a1b6b44759a1ea5ed6d236bdafd3384e from
5eaa07ae56bd360cecf5b31bcf3806f117540ab7. Its expected old failure must report the
actual old category, stage and one query; the current probe must report the new
category and exactly two queries. Neither executable loads HIP or executes GPU
code, and neither historical header/probe is linked into product targets.

The expanded SDK qualification retains every previous compiler/module/worker
gate and its existing process budgets. The initial design required new-source old/current regression,
complete API probe, SDK build and full integration execution; executed results follow below.
Normal file operations and kernels are unchanged.

## Executed priority capability checkpoint

At b2cf9899417d52c2266d86fa1246b874001376a3, SDK run43432703582 passed235 CPU
assertions across12 priority scenarios. The exact historical probe reported
category1179844613 with one query; the corrected probe reported1179844611 with
two. Neither loaded HIP or executed GPU work. Every original SDK/worker gate,
including20 confirmed exits, passed; core run43432703721 passed all13 jobs.
[Source-bound evidence](evidence/priority-capability-b2cf989.json).
