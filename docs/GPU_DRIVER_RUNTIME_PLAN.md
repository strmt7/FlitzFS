# Trusted installed-driver runtime preparation

This is an implementation plan. The existing worker still uses its authenticated
private SDK bundle; no installed-driver selection or hardware execution is
implemented by this document. The metadata-only preparation is implemented
separately in SYSTEM_RUNTIME_METADATA.md and retains its own qualification gate.

## Problem and evidence

The selected complete ROCm distribution supplies the headers, compiler and
kernel build. Its private runtime is not the same binary as the installed AMD
driver runtime. A current installed runtime has all 11 SDK-derived callable
exports, including the versioned properties API, but export presence does not
qualify runtime or hardware compatibility. Keep the independent CPU result
and default CPU routing while preparing the installed-driver path.

The reference is FlitzZip at 01eca140a9289342ddf2cdfd39270d90ad0eb2a7,
src/gpu/hip_device.cpp blob 19e5bb62e3d54c98356cc8ceac4764f6d47aebde.
The file was verified against the clean current owner checkout and native
repository API on 11 November 2026. Reuse its trusted-runtime mechanism while
preserving FlitzFS's per-child ownership, stricter file/directory pins and
confirmed retirement. Do not import a process-lifetime parent runtime.

## Required implementation

- Use only the architecture-correct system directory discovered through the
  Windows API and the selected SDK's runtime basename. Do not enumerate SDK
  installations or modify PATH, drivers, services, registry or installed files.
- Require an explicit expected runtime SHA-256 from the trusted supervisor.
  Bound metadata and file reads, pin regular files and direct ancestors, and
  verify the exact bytes plus cached-only Authenticode without UI or network.
  A missing or unverifiable runtime selects CPU; it does not start a search loop.
- Keep a kernel-only private bundle for this route. Authenticate the filesystem
  kernel and its import policy; do not redistribute an AMD runtime in the
  qualification artifact. Preserve the existing private-SDK route separately.
- Clear child LLVM_PATH before loading. Restrict native loader search, compare
  the actual loaded module path and use the selected SDK's versioned properties
  ABI to match both LUID and node before selection or compute.
- Keep the existing explicit allocation, low-priority stream, immutable source,
  independent CPU checks, terminal selection and process retirement rules.
  Runtime identity changes belong in the recovery configuration identity.
- First qualify metadata/refusal and fake-runtime paths on disposable hosted
  VMs. A metadata-only mode must never load the AMD DLL or initialize a device.
  The first owner-device compute test remains a separate tiny synthetic probe
  using exact VM-qualified binaries, verified headroom and unchanged hard caps.

## Process retirement investigation

An earlier hosted crash profile at f86db457b446b79331558a9200ef17b067745a58
timed out and could not confirm child exit within its existing termination
observation. The gate refused to proceed. All mounted scenarios subsequently
passed at bed5f061dd13990aa442a9ab1ed781bdf49baa4c with unchanged mount/supervisor
source. That later pass does not establish the failure's cause.

Investigate the exact failed hosted job and Microsoft's termination semantics;
retain the original failure and any repeat result. Do not increase caps,
manufacture retirement, suppress the case or activate a driver on the owner's
PC. A timeout is not proof of process exit or kernel quiescence.

The original f86 crash job passed its exact second attempt (run 42950900802,
job 130303167376) under unchanged caps: timeout 1460 with actual exit confirmed,
followed by independent reconciliation. Its earlier unconfirmed-exit cause
remains unknown. No retirement or owner-driver permission was weakened.
