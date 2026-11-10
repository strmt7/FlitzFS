# Host-independent execution and private dependency provisioning

Owner requirements: for each supported OS/architecture, behavior must be
host-agnostic and installation-agnostic, with no machine-specific package/app
paths or assumptions about what is installed. Missing dependencies should be
prepared automatically without damaging the host. This document is the product
contract and qualification ledger, not a claim that the complete installer or
portable mounted filesystem exists already.

## Runtime contract

CPU data recovery and committed codec decoding must be self-contained.
Redistributable user-mode components must be bundled or resolved from explicit,
application-owned locations relative to a verified package/configuration.
Do not depend on a developer's SDK path, current working directory, global
PATH, registry install location or an unrelated application's copy of a DLL.
Declare supported OS/architecture/ABI capabilities and validate them explicitly.

Optional AMD acceleration must never become a prerequisite for opening data.
Missing/incompatible accelerator components keep the CPU path functional while
eligible user-mode components are prepared off the foreground I/O path.
Use the existing failure-aware recovery/evidence policy, not repeated discovery
or an assumption that a vendor SDK/driver/app is installed.

## Automatic dependency setup

Use a bounded, private, versioned application-owned staging area. Verify package
identity, OS/architecture, download size and cryptographic integrity before
extracting or executing anything. Reject unsafe archive paths, links, special
files and output growth; preserve existing packages/data. Validate the candidate
in isolation and switch the application's own selection only after success.
Retain rollback and exclude versions still used by active workers/readers.

Automatic setup must not overwrite other applications, silently edit global
PATH/registry settings, install system services or consume recovery reserves.
Use low-priority bounded work with trustworthy headroom and cancellation.
Network failure or an unavailable optional package must preserve CPU operation
and existing user data. Installation failures are explicit diagnostics, not
silent acceptance of a different/unknown binary.

Kernel/driver and other protected OS integration require their separately
qualified installation/consent/recovery mechanism. App-local provisioning does
not bypass OS security or prove a driver cannot freeze a machine. Current
development permission remains hosted-VM-only; these requirements do NOT
authorize installing any package, compiler, SDK or driver on the owner's PC.

## Current hosted tooling implementation

prepare_hosted_tools.py now stages official SHA-256/size-pinned CMake 4.4.5 and
Linux LLVM 23.1.5 under a fresh RUNNER_TEMP directory. It ignores preinstalled
compiler/CMake paths, verifies the actual staged executables, and exports paths
only into the current hosted job. General build tooling uses stable Python 3.15
through the exact pinned official setup action. The separately reviewed research
provider remains isolated on its qualified Python/lock version pending review.

LLVM provisioning scans a verified archive and extracts selected CPU compiler,
linker, shared support libraries, C++ headers and resource files. Static tools,
documentation and unrelated payload are omitted. Each downloaded byte is checked
against the pinned archive hash before extraction. Metadata/selected-output/link
validation precedes use. No GPU runtime or module is loaded.

Linux provisioning has 4 GiB address-space, 720-second CPU and no-core limits;
downloads have a 300-second deadline; tar scanning/extraction share 480 seconds.
LLVM requires 10 GiB initial free space, selects at most 2 GiB output, and keeps
4 GiB free reserve. CMake selects at most 512 MiB. Members are capped at
768 MiB, metadata at 100,000 entries and total scanned expansion at 50 GiB.
All targets are fresh, exclusive fixture files; no source/other-host cleanup
or replacement occurs. These are hosted development controls, not yet a general
end-user dependency installer.

build_linux_codecs.py explicitly verifies the latest Zstandard/LZ4 source commits,
builds only static single-threaded libraries with at most two build workers, and
writes an explicit CMake cache. Sanitizer jobs instrument those libraries too.
Linux builds no longer silently discover whichever codec packages are installed.

## Portability qualification still required

Relocation/clean-environment tests must prove product execution after moving it
outside a build tree and clearing development-only environment variables.
Audit DLL/ELF dependencies, runtime bundling/static linkage, installed-package
absence, Unicode/space-containing paths, updates/rollback and unsupported ABI
behavior. Qualify missing-GPU/private-runtime setup and protected OS integration
separately. A self-contained build-tools job is not proof of a portable product.

The verified LLVM Zstandard package requires a decoder window above Python's
default tar-stream allowance. The provisioner reads the frame header, admits at
most a 2 GiB window with an additional 2 GiB available-memory reserve, and uses
the public ZstdFile window_log_max option. Each pass closes/releases that decoder
before starting another. The 4 GiB process address-space ceiling remains.
Filesystem record-decoder ceilings and foreground behavior are unchanged.

## Self-contained CPU build and qualification

The explicit FLITZFS_PORTABLE_CPU=ON build selects the static MSVC runtime on
Windows and fully static linkage on Linux. This option is deliberately separate
from sanitizer/developer builds and historical comparison lanes. Optional
third-party codecs must also be supplied as static libraries with compatible
runtime settings; the hosted builders do that from exact reviewed source pins.
Unsupported OS/compiler combinations and shared FlitzFS library builds fail
explicitly. This is a CPU diagnostic executable, not a mounted product package.

Windows CLI arguments now enter through wmain, convert strictly from UTF-16
to UTF-8, and explicitly reconstruct Windows filesystem paths as UTF-8. OS file
access still uses the existing wide-character path validation and file APIs.
POSIX paths retain their original bytes; no locale-dependent recoding is added.
Catalog virtual-path grammar remains unchanged.

The hosted-only check_portable_cpu.py reads the executable's actual PE/ELF loader
metadata before execution. Linux must have neither a dynamic interpreter nor
dynamic segment; Windows admits only a small explicit set of OS DLL names and
rejects delay imports, codec/GPU/SDK and VC/UCRT dependencies. It then moves a
package containing only that executable into a fresh Unicode/space-containing
directory, uses a different working directory and an environment with an empty
PATH directory and no build/runtime library variables. Windows retains only its
OS root identity. Fixtures exercise actual bundled compression, independent byte
readback, Unicode OS paths, catalog commands and existing-file overwrite refusal.
Linux additionally checks a filename containing a non-UTF-8 byte.

Every fixture is new on a disposable hosted VM and is retained, not deleted.
The parent has Linux 512 MiB address-space/30 CPU-second/no-core limits or a
Windows idle-priority 768 MiB job/20% CPU/two-process/15 CPU-seconds-per-process
containment. Commands have ten-second wall deadlines; Linux children additionally
have 256 MiB/five CPU-second ceilings. Synthetic file bytes excluding the
executable must total at most 8 MiB, checked before accepting the receipt;
2 GiB backing-volume headroom is required.
Receipts bind binary SHA-256, commit, imports, environment and executed commands.

These source changes and checks require successful exact-commit CI before
qualification. They do not establish compatibility with every Linux kernel or
Windows build, a complete installer, optional GPU provisioning, protected
integration or full filesystem portability. Existing default build behavior,
storage formats and historical comparator link settings are preserved.

## Verified Windows AMD distribution boundary

The official published HIP SDK 7.2 deployment page was captured by the reviewed
hosted Crawl4AI provider at 4951f209ddface6871d4b2f50ae11cf6bfaf5ec4
(run 42664639886, native HTTP 200). Its Markdown SHA-256 is
8b0bfff2b11aef34082ea5b4972272a7bffbd4e439dc942098860d9794f166e6.
The earlier latest challenge and incorrect docs-7.2.0 404 are retained failures.

AMD's guidance supplies the HIP runtime through the AMD GPU driver; applications
do not redistribute that runtime. SDK libraries used by an application are a
separate deployment category. FlitzFS must therefore distinguish its private,
verified worker/code-object/eligible-library setup from protected OS driver
integration. Do not copy an SDK runtime DLL into a private package merely to
make discovery appear installation-independent. Runtime/compiler/driver minimum
contracts and actual compatibility still need qualification.

CPU opening/recovery must work without a HIP SDK, runtime or compatible AMD
driver. A missing/ineligible driver retains CPU operation and suppresses repeated
discovery for unchanged evidence; it never causes an automatic global installer
or PATH edit. Future protected integration needs its own qualified mechanism.
No driver/runtime installation on the owner's PC is authorized by this source.

Source: https://rocm.docs.amd.com/projects/install-on-windows/en/docs-7.2/conceptual/deployment-guidelines.html


## Current ROCm distribution supersedes the legacy HIP selection

For current implementation decisions, the earlier HIP SDK 7.2 deployment capture
above is historical evidence. It does not establish current ROCm 10.1.0 policy.
Consult fresh official distribution documentation and the current source-bound
FlitzZip toolchain before changing setup/loading behavior.

ROCm 10.1.0 is selected as one complete package by tools/rocm-sdk-lock.json,
matching reference source 113ca5f30655f0d373c17d1448e35ce1b69f2819.
Its observed HIP/compiler versions remain component observations, not separate
version selections. The hosted qualification reuses the current complete-package
bootstrap and scoped compiler helpers; it never generates replacement SDK
headers, mixes components, installs a driver or changes the owner's PC.
This development setup remains distinct from the unfinished end-user installer.

## Windows qualification job admission and failure evidence

At 6cc05e654b006bd0a13b2a0bdaefc695bfcbe63d, CPU Windows raw job
129528969561 passed all 43 CTest entries, then failed during relocated
CreateProcess for verify with WinError 1816 (not enough quota). Earlier help,
pack and info commands had launched. No accounting snapshot was retained, so
the exact exhausted quota and historical cause are UNPROVEN. Preserve this
failed run; do not call it a passing portability result or silently retry it.

The qualification harness contains its Python supervisor and one child in a
private job with active-process limit two, 512 MiB per process, 768 MiB per job,
20 percent CPU and existing time/priority/kill-on-close limits. Its old launcher
did not check job accounting between sequential children. The new launcher
requires actual private-job ActiveProcesses==1 before launching and after
retiring each child. An unexpected count, query failure or changed configured
limit fails closed. A bounded three-second accounting wait checks retirement,
rather than treating elapsed time as proof. CreateProcess is attempted once;
quota errors retain before/after accounting and are rethrown without retry.
Receipts record accounting observations and zero launch retries.

The ABI was checked against current Microsoft .NET source
8b9903ebf160fed45050edda447792363ce96dc0 (repository head observed
9 November 2026), whose job interop exposes the accounting/extended-limit
structures and QueryInformationJobObject. Its process-handle code separately
consults active accounting before retiring jobs. This supplies current primary
source for the query and lifetime distinction; it does NOT identify the missing
historical quota telemetry or justify copying unrelated retry behavior.
Native Windows tests additionally compile sizeof/offsetof assertions against
the actual selected SDK, binding the Python AMD64 layouts to that SDK.

A separate hosted Windows regression holds one controlled Python child alive
until the guard observes two active members, releases it from that observation,
requires retirement to one without any extra launch, then launches exactly one
continuation child. It creates no files. The production qualification fixtures,
executable/import checks and all old resource caps remain unchanged.
Configured guards/tests require exact-commit runtime inspection before
qualification; no owner-PC action or product-runtime Python dependency follows.

[Microsoft job interop](https://github.com/dotnet/runtime/blob/8b9903ebf160fed45050edda447792363ce96dc0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.JobObjects.cs),
[current process-handle lifetime code](https://github.com/dotnet/runtime/blob/8b9903ebf160fed45050edda447792363ce96dc0/src/libraries/System.Diagnostics.Process/src/Microsoft/Win32/SafeHandles/SafeProcessHandle.Windows.cs).

## Executed accounting qualification checkpoint

At b348615dd0fb1d1bb3c5cf01e9017e31cff66589 all six triggered hosted
workflows succeeded. Windows raw job 129535121240 and native static job
129535115972 each passed the actual held-child retirement test: private-job
accounting observed two active members, released the controlled child from that
observation, returned to one and launched exactly one continuation. Total
assigned processes were one, two and three; no extra launch or retry occurred.

Both relocated packages then completed all 21 commands with before/after
supervisor-only accounting and zero retries. Each command increased the actual
job total by exactly one. Raw/native binary SHA-256 values were
28ec2fb2319bf391a1651ee7218201ca16ef854015985d86518e406455fad731
and a42206fbaa24be3d943861155d0dc5817f292b66a63c007cc0f406d3a6a7dca8.
No resource limit was raised. This validates the new admission mechanism;
the exact cause of the older 1816 result remains unproven without its missing
accounting evidence.

Windows native job 129535121493 passed all 45 entries; ASan/UBSan job
129535120786 passed all 39. Both retained 400 basic invariants, 1,038 basic
fault cases, 2,064 lifetime faults, 1,026 context faults and 271 Flush checks.
The Windows basic API oracle was emitted explicitly. Hosted read-only mount
job 129537473508 passed normal I/O, forced helper exit, CPU recovery/remount
and source parity, with peaks 6,131,712 / 6,160,384 / 5,234,688 bytes.

The b348 change did not modify the filesystem, metadata producer/controller
or dedicated metadata workflow. The 712 metadata and 976 legacy writer
termination results remain bound to their executed 6cc source. No owner-PC
operation, latest Windows 11 client, live GPU or writable mount was qualified.
