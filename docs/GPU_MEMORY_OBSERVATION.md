# Bounded worker memory observation

Experimental, 11 November 2026. Default filesystem/CLI operations remain CPU-only.
This is an explicit SDK-worker diagnostic, not a filesystem data reply or a
performance/admission policy.

## Why this exists

At 9028be3fc9480c367bba6bd8261fc169af67fda7 the tiny owner decode reached
completed_nonlocal_growth. In that pinned source this stage occurs only after
the actual launch, stream completion, all task statuses and byte-for-byte CPU
comparison succeed. Thus the 12,288-byte synthetic kernel result was internally
correct, but the final memory rule rejected it and published no candidate.
The worker exited in 1,012 ms, with 80,261,120 bytes child peak commit and
81,657,856 aggregate. The driver remained unchanged/OK; no mount occurred.

The current Microsoft memory-source capture in
[evidence/gpu-memory-research-9028be3.json](evidence/gpu-memory-research-9028be3.json)
documents process-wide local/nonlocal counters. Inferring a specific allocation's
eviction from aggregate growth is unsupported. Before changing the existing
zero-growth rule we need the worker's actual initialization and completed-work
budgets/usages. A parent-process counter is not a substitute.

## Explicit operation and retirement

The parent mode is --run-amd-driver-memory-once WORKER ORDINAL LUID SHA256.
It builds the same tiny owned synthetic raw/fill request as the existing decode
probe. The child mode is --driver-memory-adapter ORDINAL LUID MASK SHA256,
followed by the existing protected worker-channel marker/token. The request is
the existing validated decode frame. No filesystem files or device paths enter
the channel.

Both modes retain current runtime/kernel hash and path verification, exact
HIP/DXGI adapter identity, optional independent single-node proof, 128 MiB worker
and 256 MiB aggregate budgets, 20 percent CPU, below-normal priority, five-second
child allowance, tiny batch limits and independent CPU comparison.

An optional worker-owned observation records the three existing budget samples:
before HIP initialization, after initialization/selection, and after completed
GPU output has been compared with CPU. It additionally observes the OS nonlocal
budget. That field is informational and changes no gate or existing snapshot
validity rule. Successful launch/stream/status/CPU comparison steps are recorded
only after their calls/checks succeed. Resource-release failure remains sticky
even if a later destructor sees no remaining buffer handles.

Only this explicit diagnostic catches a typed compute refusal for reporting.
It returns metadata after its owned resources unwind and both modules retire.
A failed module close, malformed request, early setup failure, allocation
exception or unconfirmed child exit returns no observation. A successful
metadata child exit means the observation was transported; it does not mean
compute admission passed, every release succeeded, or a GPU data reply exists.

Normal decode/match operation retains its failure-empty behavior. The diagnostic
discards candidate data bytes and never calls the normal reply encoder.
The parent copies metadata only after confirmed successful child/supervisor exit,
validates its exact binding, and always reports gpu_candidate_accepted=false.
It does not retry, route application data or change GPU recovery policy.

## Private 256-byte reply

The eight-byte FLHMEM01 magic cannot pass decode-request/reply or identity-metadata
parsers. All integers are explicitly little-endian; no native struct is copied.

| Offset | Bytes | Field |
|---|---:|---|
| 0 | 8 | Magic |
| 8 | 8 | Nonzero request ID |
| 16 | 4 | Ordinal, 0..63 |
| 20 | 4 | Single-bit requested node mask |
| 24 | 8 | Requested adapter LUID |
| 32 | 8 | Bound explicit device-buffer bytes |
| 40 | 8 | CPU-verified GPU bytes: zero or exact requested decoded length |
| 48 | 4 | Internal typed failure category, or zero on admitted compute |
| 52 | 4 | Internal failure stage, 0..31 |
| 56 | 4 | Captured points: only prefixes 0, 1, 3, 7 |
| 60 | 4 | Defined execution/retirement flags |
| 64 | 192 | Three 64-byte snapshot records |

Each snapshot serializes capture time, dedicated bytes, local budget/usage,
nonlocal budget/usage (six 64-bit values), LUID (eight bytes), node mask
(four bytes) and valid/AMD bits (four bytes). Uncaptured records must be zero.
A valid captured record must match the bound adapter. Raw counters, including
an invalid collected record, are observations rather than permission to compute.

Flags are resources released (1), modules retired (2), compute admitted (4),
launch submitted successfully (8), stream completed (16), task statuses verified
(32). Unknown bits fail. Stream completion requires launch submission; verified
statuses require stream completion. A nonzero CPU-verified count requires all
three work flags and all samples. Admitted compute additionally requires no
failure and successful owned-resource release. Module retirement is mandatory
for every published observation. A missing step flag does not establish that no
driver-side activity occurred before a failure.

## Qualification and next step

Portable tests use independent golden bytes, truncation/binding/flag/record
rejection, partial failures, completed diagnostics and cross-protocol rejection.
The Windows CPU helper exercises actual protected transport, malformed metadata,
failure after publication, malformed requests and unchanged handle accounting.
Its synthetic execution flags are framing fixtures, never GPU evidence.
The complete-SDK job adds a bad-runtime refusal for this mode while preserving
all 17 existing admission exits. Exact source-bound checks must pass before any
new owner diagnostic; its result must remain separately identified.

The next hardware observation is one bounded explicit request, not a retry loop.
Use its measured counters with current official semantics to design and test
bounded local/nonlocal/RAM admission. Do not infer zero paging, global free VRAM,
speed, production readiness or universal driver safety from these samples.
