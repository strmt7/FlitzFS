# HIP runtime worker development contract

This is the next implementation/qualification gate, not an enabled accelerator.
Normal filesystem paths remain CPU-only. No owner-PC execution is authorized.

Official native GitHub release/tag APIs identify HIP 7.2.4 at
bc9af25177f96c0fea93198b89cf4c3cf08f3ea3 and its CLR platform headers at
fe5035afc8713dfc6adedd3c00c4306c93a160f8. Use the actual official headers;
hipGetDeviceProperties is mapped to hipGetDevicePropertiesR0600 and its Windows
LUID/node fields must not be guessed with a hand-written structure or an absent
standalone LUID function. These source observations are not runtime ABI tests.

The isolated HIP-runtime Crawl4AI group must successfully capture AMD deployment
and current module/stream contracts plus Windows loader/trust contracts before
dependent loading decisions. Native URL/status/errors/content/digests remain
retained; rejected pages provide no source evidence. Full short documents and
bounded substantive sections are printed for review. Native API/header byte
captures keep their own native transport.

## Required implementation boundary

- Put all HIP initialization, adapter queries, module loading, allocation,
  dispatch, synchronization and cleanup inside the bounded child. Parent CPU
  selection and durable storage must never depend on driver cleanup.
- Use driver-supplied HIP runtime according to AMD's distribution contract;
  never privately copy an SDK runtime DLL merely to bypass an absent driver.
  Verify loading identity and constrain dependency search using qualified OS
  behavior. Unknown authorization or compatibility retains CPU operation.
- Match exact LUID and a single-node mask, verify supported target/runtime and
  fresh DXGI budget before device allocation, and preserve the existing small
  batch/reserve/no-nonlocal-growth contract. Bound adapter enumeration.
- Load only the source-bound built-in code object for the qualified target,
  never executable GPU code supplied by file contents or request frames.
- Bound stream/event work and runtime calls under external supervision.
  Error classification must distinguish missing/incompatible runtime, resource
  pressure, device faults and integrity failures; time alone proves no repair.
- Keep input/task/status/output ownership private and verify exact CPU parity
  before publication. Failure and unknown retirement block replacement.
- Stage only verified header dependencies in fresh hosted application-owned
  build directories; no global SDK/runtime installation or PATH dependency.
  Licensing, exact source identities and ABI compile evidence must be retained.

A fake runtime may exercise orchestration, ABI calls, buffer/task geometry,
error paths and cleanup on disposable CPU runners. Such results are CPU semantic
tests, never HIP hardware execution or proof of runtime/driver compatibility.
Any eventual real GPU launch still needs explicit hardware/VM qualification,
resource containment and kill-switch evidence before owner-device testing.
