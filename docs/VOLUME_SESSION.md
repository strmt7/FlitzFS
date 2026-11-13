# Managed native volume sessions (experimental)

NativeFileSession can now own a custom-backend volume head and active image
together. Explicit compaction/post-compression replaces its image writer while
preserving the SAME bounded context table, weak owner identity, monotonic tokens,
access grants and Cleanup state. This connects native image handoff to real
session I/O; it is not a mounted maintenance service, file-provider, installer,
idle scheduler, GPU runtime or physical-power qualification.

## Provider and serialized ownership

create_volume requires NEW empty, distinct, qualified head/image backends,
distinct nonzero volume/image identities, admitted CPU codec options and the
existing durability/resource contracts. It initializes an image, actually retires
its writer, creates/fences the head, and resumes the image under that owner.
Normal create() behavior is unchanged. A shared pure contract validator keeps
new admission consistent with the existing object-store rules.

resume_volume requires the exact expected head epoch, a qualified immutable
image-ID resolver and actual retirement of the previous controller/dispatcher/
external file contexts. It selects independently, rejects unresolved persisted
lifetime state, repairs only the selected image's metadata, retires that repair
writer, repairs/acquires the head owner and resumes the selected image.

Both APIs remain custom-backend only. No ordinary-file opening/resume, mount,
driver, process, thread or OS state change is introduced. The provider MUST
globally exclude competing controllers and aliased storage facades, never reuse
an image identity, retain durable image-ID mappings/source files and qualify
flush/failure domains. Per-object leases and a caller assertion cannot prove
these properties. Historical image-ID reuse beyond the retained transition
requires the provider's durable registry; this layer is not that registry.

One control thread/outer operation guard must serialize callbacks, maintenance,
recovery, moves and destruction. No operation may overlap handoff. Destruction
releases the active image writer BEFORE the head owner. Existing CPU snapshots
pin their original backing lifetime and remain independently readable; no source
file is deleted, renamed, truncated or overwritten by these APIs. This does not
implement physical reclamation or an unbounded archive of old images.

## Live-context handoff

compact_volume preserves encoded payloads; recompress_volume uses the explicit
CPU re-encoding primitive. Each requires a NEW target and identity. Reject
current/head aliases, current/volume/previous-image identity reuse, incompatible
options/contracts and nonempty targets before retiring the writer.

Adopted linked contexts, including cleaned contexts that may receive later
paging I/O and unprocessed initial-delete options, survive. Provisional contexts,
requested/latched deletion and unresolved native lifetime state defer maintenance.
Do not invent Cleanup/Close, remove an inode or manufacture external retirement
to make maintenance proceed. The existing fixed 256-context/16-KiB table limits
remain; the optional bounded volume-control record is separately capped.

Before retirement, retain the exact verified source snapshot, source/candidate
backings and candidate identity. Mark the session fenced, then really destroy
the old writer. Build the bounded candidate, destroy its writer, independently
compare/flush/publish the head, and resume the chosen image. Only nonthrowing
ownership/scalar updates follow successful adoption. The stable external volume
identity stays unchanged; the internal image identity/selection epoch advance.

Candidate compression is separate from the retained foreground write policy.
Stronger post-compression therefore does not silently make later foreground
writes use the stronger candidate settings. This is not an automatic policy,
optimal codec claim or measured speedup.

## Failure recovery without losing contexts

Any error after retirement leaves the session fenced with its original context
table, source snapshot and both images retained. Normal reads, writes, Flush,
Close and another maintenance call reject without backing I/O. Diagnostics are
not cached recovery authority.

recover_volume permits ONE explicit recovery attempt for that owned failed
transition. It actually retires the former image/head owners, independently
reads the head and resolves ONLY its retained source/candidate. It accepts the
original epoch/source or the immediately succeeding bound candidate, never an
older/foreign image. Independently compare the selected native namespace and
every live payload against the retained acknowledged source before repair.

Repair selected image metadata, verify again, repair the selected head peer
first through the existing authority protocol, then resume the SAME context
table and original foreground policy. No failed candidate is recompressed,
retried or selected by elapsed time. Failure remains terminal; a second recovery
attempt performs zero I/O. Foreground-write failures and arbitrary old-session
state are not silently reclassified as this maintenance transition.

A damaged source/candidate or both lost head copies can still make recovery
unavailable. Checksums cannot reconstruct missing unique payload. Physical
replicas, global provider ownership, Windows external-context recovery and
hardware/firmware failure assumptions remain separate full-product work.

## Qualification

Five new CPU groups cover both metadata contracts, live/grant/cleaned/initial-
delete contexts, immutable old snapshots, repeated handoff, actual controller
retirement/resume, unresolved startup rejection, and every observed maintenance
and recovery write/flush under all three RAM fault modes. Each retains the
existing 45-second process limit, byte/object/context/resource budgets and all
older tests. Inspect exact-commit execution; configuration is not a pass.

The locked-RAM Windows probe now drives public managed-session create, per-file
writes, retained-context reads, handoff, independent CPU selection, real Flush
and actual Cleanup/Close. It keeps the SAME 40-MiB locked/128-MiB child/20%-CPU/
15-second caps. Do not run a changed binary on the owner PC before its own VM
qualification. Model flushes remain volatile RAM checkpoints, not physical
power-loss evidence. Ordinary CLI/callback defaults still select no managed
maintenance or GPU path.

For staged recompression, see [Incremental candidate copy](RECOMPRESSION.md#incremental-candidate-copy).
The source remains under the original control thread during staged copying.
Only successful candidate sealing proceeds to the same fenced handoff and
recovery state. No caller may overlap stage calls with foreground operations.

A hosted idle opportunity uses the same staged copy and handoff. It defers to
callbacks, remembers the evaluated source generation, and declines to publish
a candidate whose projected image and live-payload savings fail admission.
The normal mounted test lifecycle owns any unfinished candidate through stop.


## Controlled advisory state

The optional optimization_state control binding now belongs to this same volume
owner. See OPTIMIZATION_STATE.md for the checked API, failure isolation and
provider preconditions. Normal file operations never consult it. Failed or
unavailable advisory state must not fence user data, close handles or make
startup depend on a learned model. A bound or partially attempted advisory
area cannot become a maintenance target. Image handoff preserves the area.

This is a shared-engine capability for the product backends specified in
WINDOWS_VOLUME_BACKENDS.md. Native provider reservation, helper IPC, tray
provisioning and persistent write-credit policy remain unfinished.

## Failed advisory-owner rebuilding

The separate [rebuild control](ADVISORY_REBUILD.md) can replace unavailable or
poisoned advice while preserving the same file handles and data owner. It starts
empty state in a new provider-reserved area and retains all prior areas/leases
within a four-area aggregate ceiling. Both immediate and staged image maintenance
reject current and prior advisory backings. Original create/resume binding rules
and all user-data publication/recovery rules remain unchanged. This does not
publish a durable provider mapping or implement automatic learning.
