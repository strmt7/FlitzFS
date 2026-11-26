# Windows GPU and storage priorities · 26 November 2026

## 1. Get verified AMD output into ordinary Windows requests

The installed LZ4 profile tested here requires two frames. The earlier
controller had no way to capture both when each callback requested one
64 KiB extent. Widen to the profile minimum when the same handle has
observed an aligned, sequential run, adequate lead and a current resource
grant. Give CPU preparation only the original request range if acquisition
or dispatch declines. This addresses an admission failure before HIP runs.
The remaining performance question is how often a verified result reaches
the requesting application before CPU service finishes.

- Measure on the installed Win32 path with a stable handle and trace
  offered, captured, submitted, verified and consumed bytes. Include
  small sequential requests, queue depth, cache hints, sparse extents,
  writes that change the selected root and actual CPU misses. Windows
  documents sequential access as a cache hint. HIP streams provide a
  separate mechanism for asynchronous work. Measure full host time
  from capture through checks and consumption.
  [Windows cache hints](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-createfile2_extended_parameters) ·
  [AMD HIP asynchronous execution](https://rocm.docs.amd.com/projects/HIP/en/docs-10.2.0/how-to/hip_runtime_api/asynchronous.html)
- Compare cold startup and the existing prepared-worker mechanism.
  Attribute time to package checks, process launch, HIP setup, transfers,
  compute, reply verification and retirement. If setup stays dominant,
  reuse authenticated preparation within live idle and memory allowances.
  Learn from timely results and confirmed late demand. A finished job
  that misses the application request has spent resources without serving
  its intended bytes.
  [NSDI 2026 delayed-hit analysis](https://www.usenix.org/conference/nsdi26/presentation/keren)
- Keep host-clock measurements and confirmed retirement as the decision
  inputs. AMD's trackers include a Windows event-timing report across
  multiple architectures and an independent Windows submission-stall
  report on gfx1100. These reports identify modes to account for in the
  supervisor and measurement design. Their applicability to FlitzFS
  depends on observed runtime, device and path.
  [AMD Windows HIP event report](https://github.com/ROCm/rocm-systems/issues/12925) ·
  [AMD Windows HIP submission report](https://github.com/ROCm/rocm-systems/issues/12535)

## 2. Qualify the client, commit ordering and writable reopen

- Run the existing Win32 witness through Explorer attachment, restart
  and private dependency setup on supported Windows 11 versions. Bind
  each result to its app, kernel component, signature and OS build.
  Microsoft's October 2026 HLK catalog has distinct 25H2 and 26H1 kits.
  Keep the exact kit and playlist with each qualification result.
  [Microsoft Windows HLK](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/)
- Record each acknowledged write and the backend barrier actually
  crossed. Compare independent CPU reopen at publication, relocation,
  retirement and migration cuts. Run physical failure qualification
  against declared provider contracts in a disposable environment.
  Windows documents `FlushFileBuffers` as the mechanism to drain
  system buffering. The OSDI metadata write-once work motivates
  coalescing dependency-safe metadata updates, with experiments on
  persistent memory. Test each intended Windows storage provider with
  its own update and flush behavior.
  [Windows buffered writes and flushes](https://learn.microsoft.com/en-us/windows/win32/fileio/flushing-system-buffered-i-o-data-to-disk) ·
  [OSDI 2025 metadata write ordering](https://www.usenix.org/conference/osdi25/presentation/pan)
- Use the full indexed inventory verifier as the reference for a
  changed-page proof. Compare outcomes at every commit cut before using
  the proof for normal writable reopen. Measure attach time, index
  reads, resident bytes and callback tails across independently grown
  file counts, size mixtures and directory shapes. Hold out the largest
  feasible tier and report its prediction error before extrapolating
  toward tens of millions of files.
  [Detailed scaling and recovery steps](../architecture/RELEASE_REVIEW_2026-11-26.md#3-prove-ordered-persistence-and-bounded-writable-admission)

## 3. Improve complete settled size first

- Compare an exact, bounded similarity oracle with the current sparse
  candidates. Label each trial by saved occupied bytes after accounting
  for metadata, parity, dictionary/base retention and retirement.
  Measure discovery recall and sampled read amplification. RubikFS
  groups similar chunks for read-only images. The November SkyFS
  preprint evaluates windowed reordering with sampled compression on a
  distributed system. These support testing bounded layout candidates
  and independently accessible members on writable FlitzFS.
  [FAST 2026 RubikFS](https://www.usenix.org/conference/fast26/presentation/huang) ·
  [November 2026 SkyFS preprint](https://arxiv.org/abs/2611.22847)
- Prioritize reversible conditioning for families with concrete
  evidence of residual savings. Microsoft’s DirectStorage preview
  combines Zstandard CPU and GPU decompression with asset conditioning.
  Test the lossless technique on a documented file family, charge its
  restart index and require byte-exact CPU restoration. Keep the
  filesystem path, durability and mixed-file workloads in the evaluation.
  [Microsoft DirectStorage 1.4 and asset conditioning preview](https://devblogs.microsoft.com/directx/directstorage-1-4-release-adds-support-for-zstandard/)
- Train a compact local ranking candidate against measured allocation
  gains. Split validation by related file families and collect query
  cost, rewritten bytes and issued device duty. Use the existing
  rebuildable advisory state and account for its size. Advance the
  candidate only when its retained savings survive the same read and
  recovery tests as deterministic selection.
  [Autonomous policy implementation package](../IMPLEMENTATION_PLAN.md#10-e07--autonomous-learning-resource-control-and-background-policy)

## 4. Compare completed systems on the Windows host

Alternate FlitzFS, plain NTFS and native compressed NTFS with matched
client data, allocation geometry, compression state, cache mode and
durability semantics. Use `compact /c` and confirm with
`FSCTL_GET_COMPRESSION`. Use DiskSpd XML latency and throughput
controls alongside populated-file and namespace workloads. Classify
buffered and unbuffered results separately. Keep NTFS-specific BypassIO
out of the matched comparison. Report complete allocated bytes after
maintenance settles and verify the data independently.
[Windows compact](https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/compact) ·
[Native NTFS compression state](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_set_compression) ·
[Microsoft DiskSpd switches](https://github.com/microsoft/diskspd/wiki/Command-line-and-parameters)
