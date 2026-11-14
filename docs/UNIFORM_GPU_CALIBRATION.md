# Experimental uniform GPU calibration

This diagnostic connects the version-2 uniform slab to native compressed writes without enabling an ordinary filesystem GPU route. It is not a performance qualification or a production cost model.

## Phase envelope

The explicit `--driver-uniform-lz4-profile-adapter` command uses the same worker arguments as the ordinary uniform adapter. Its declared reply capacity includes an additional 256-byte prefix. The ordinary version-2 frame remains unchanged; ordinary readers reject the profiling envelope.

The prefix starts with `FLULP001`. It binds the request identifier, ordinal, node mask, adapter LUID, source digest, input length, chunk geometry, device allocation and supplied work grant. Bytes 80–87 hold total worker wall nanoseconds, 88–91 hold the complete/finished mask, 92–95 hold the execution backend, and bytes 96–215 hold the fifteen ordered phase durations. Bytes 240–247 declare the following ordinary frame length; bytes 248–255 must be zero. The parent validates every binding and independently decodes the enclosed candidates against its immutable CPU source.

These are host wall times, including API and scheduling waits. Kernel submission plus synchronization is one phase; it is not GPU-event kernel-only time. The envelope's extra verification, wrapping and publication occur after the recorded total. The parent separately records exchange wall time and emits all fifteen phase values as an ordered array. Neither value measures the whole native write. Timing metadata is diagnostic, not authenticated performance evidence. The existing five-second diagnostic limit rejects incomplete, inconsistent or overlong observations. Disabled recording performs no clock queries.

## Explicit native experiment

`--gpu-uniform-native-once` accepts an explicitly qualified worker, adapter ordinal/LUID/node and installed runtime digest. Its synthetic source contains two identical 64-KiB extents. The slab size is calculated from that shape. The helper retains the complete source, checks fresh memory evidence, and launches at most one worker in its existing 128-MiB, 20-percent CPU, five-second job. The controller retains its existing 256-MiB job. These are experiment budgets, not filesystem allocation defaults.

A failed or refused worker selects independent CPU publication. GPU bytes are eligible only after confirmed successful child exit, fresh memory-transition admission and complete parent verification. Native publication then independently validates supplied extent advice. The disposable locked-memory image preserves the previously acknowledged sentinel and is reopened through the CPU reader. Repeated extents may deduplicate into one object; this fixture does not prove a storage-ratio advantage or two distinct stored objects. It checks exact supplied payload storage, current and prior data, process retirement, zero retained locks and working-set restoration.

`--uniform-native-cpu-fixture` qualifies the same native composition with independently constructed CPU advice and no AMD module load. Hosted admission checks also exercise the profiling command with a deliberately unavailable runtime identity and malformed grants/frames. They never execute GPU code.

Owner-device execution requires successful exact-source core and SDK qualification plus the existing reviewed staging, headroom and process-retirement gates. No driver installation, mount, clock change, automatic retry, normal I/O routing or physical power-loss qualification is introduced. One sample cannot qualify a reusable execution-time bound. Configuration/recovery keyed cost acquisition, useful repeated throughput measurements and controlled application integration remain required.

## Executed source qualification and owner observation

Source `7e87bb4f714c4c7fc852ca9052d6f9ef48fed204` passed core run 43604457180 (13/13 jobs, 151 native groups, Linux codec/raw, Windows raw and sanitizers) and full SDK run 43604457176. Uniform compute CPU qualification covered 40 scenarios (4 successes, 36 refusals); raw-only verification refused before runtime use. Fifteen real child exits qualified ordinary/profiling admission rejection. The retained first fixture failure at `fd59778` assumed repeated extents had exactly one data object; the correction verifies every matching object instead.

The explicit owner CPU fixture and GPU/native trial then passed. The GPU encoded 131,072 source bytes as 16,918 candidate bytes and stored them exactly. Current and prior data recovered independently on CPU. The CPU/GPU fixture active payloads were 17,045/17,037 bytes; both complete images were 983,040 bytes. The CPU fixture uses independently constructed advice, so the eight-byte payload difference is not a comparison against an optimized CPU compressor.

Exchange wall time was 602,848,900 ns; recorded worker wall time was 514,920,100 ns. Device initialization accounted for 419,350,400 ns; launch plus synchronization took 4,756,200 ns. All fifteen phases are retained in [the evidence](evidence/uniform-gpu-native-7e87bb4.json). Initialization dominated this single cold observation. It cannot qualify reusable cost estimates, predict larger batches or establish GPU speed improvement.

The explicit device slab occupied 328,288 bytes. Locked-memory peak was 4,194,304 bytes and returned to zero. Worker/aggregate job peaks were 88,502,272/95,408,128 bytes. Working-set limits restored, exact transport retirement completed, staged inputs and the installed runtime digest stayed unchanged, the GPU reported healthy and no FlitzFS process remained. No owner mount or driver change occurred. Physical power loss, universal safety and normal callback GPU routing remain unqualified.
