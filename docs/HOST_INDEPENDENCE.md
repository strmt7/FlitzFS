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
