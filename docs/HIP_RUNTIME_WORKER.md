# ROCm distribution governs HIP integration

Important implementation and dependency decisions must use recent reputable
sources and current primary-source evidence. Do not infer current behavior or
versions from model training, a legacy GitHub latest-release pointer, search
snippets or an unverified installation assumption.

## Current reference, verified 9 November 2026

FlitzZip main was inspected at 113ca5f30655f0d373c17d1448e35ce1b69f2819.
Its tools/rocm-sdk-lock.json selects the complete native Windows ROCm 10.1.0
distribution. It records package build 10.1.0rc3, observed HIP
7.16.26385-7da43026e4 and AMD Clang 24.0.0git. ROCm and HIP use different version
numbers: the selected whole distribution controls component identity.

The previous FlitzFS 7.2.4 direction was an incorrect selection from the legacy
HIP repository release pointer. Its standalone-header provisioner/ABI drafts
were not published or executed and are discarded. That direction is superseded.
Historical captures remain historical evidence, not current SDK authority.

Reuse FlitzZip's current complete-SDK bootstrap, strict archive admission,
bounded process runner, compiler-environment scoping, native compile helper and
trusted-runtime mechanism. Pin the source and verify actual consumed inputs.
Do not rebuild an independent subset of headers or mix compiler/import libraries/
device bitcode/runtime metadata from different distributions. No archive format,
archive CLI or codec registration becomes a filesystem product dependency.

The same lock's SHA-256 was locally observed from AMD's HTTPS artifact on
25 October; it is NOT an independently published vendor checksum. Preserve that
provenance distinction and verify every downloaded archive byte before use.
Current official release/install pages are captured through the isolated
Crawl4AI provider; inspect successful native content and dates before claiming
a distribution is the newest compatible choice.

## Safety and remaining qualification

Complete SDK provisioning and compile tests run only on fresh disposable hosted
Windows VMs with capacity checks, no global PATH edits, no driver install and no
GPU execution. Missing eligible SDK files trigger the verified complete-package
bootstrap. Insufficient resources or unverifiable integrity must refuse safely,
preserving owner data and CPU operation.

The runtime must remain in a separately supervised child. Adapt the current
trusted driver loader and stream/device/resource mechanisms only after verifying
their current sources and tests. Keep immutable CPU verification, explicit
qualification, bounded work, terminal failure, configuration quarantine and
exact retirement. A compiler-only check is not GPU execution or a speed result.

The current one-shot/predecoded batch remains a development correctness fixture;
it cannot establish production GPU throughput. Runtime/device compatibility,
worker lifecycle and actual benefit still require off-host hardware evidence.
Normal filesystem paths stay CPU-only and owner-PC execution remains forbidden.


## Executable complete-package qualification

qualify_rocm_sdk.py uses the unchanged current reference bootstrap and native
HIP compile helper from the pinned private checkout. It verifies reference Git
identity/cleanliness and byte-identical whole-distribution locks before import.
It runs the reference bootstrap regressions, admits the entire download/extracted
size plus 4 GiB free reserve, automatically provisions the complete package,
and uses that same SDK for headers, import library, compiler and device inputs.

The reference bounded command runner limits each owned tree to 2 GiB committed
memory, below-normal priority, bounded output and explicit deadlines; its fresh
RAM admission also retains half free RAM and at least 2 GiB. SDK provisioning
has a 3600-second outer deadline; compilation commands have 900 seconds.
No runner software is removed to make room. Insufficient capacity is a failure,
not permission for a partial SDK or an unbounded retry.

The filesystem kernel is copied byte-for-byte into a fresh include-layout fixture
so the unchanged reference compile helper can consume it. The full release-target
preset is compiled. Header observation and a separate kernel DLL link use the
same complete SDK. The DLL is NEVER loaded in this job. The CPU probe's actual
PE imports are audited before execution; it reports actual header and required
runtime identities without initializing HIP. Critical SDK hashes are compared
again afterwards. Only JSON provenance/results are uploaded, no generated
binary, SDK archive, driver or release package.

Current-source inputs include the reference lock/bootstrap, native process runner,
compiler-environment helpers and source GPU kernel. These mechanisms are build
dependencies only; no FlitzZip archive code is linked into FlitzFS. Successful
exact-commit results are required before claiming compile qualification.
