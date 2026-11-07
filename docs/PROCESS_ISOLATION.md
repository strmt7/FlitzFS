# Bounded trusted CPU helpers

The Windows process runner is a resource-control boundary for future filesystem
experiments. It is **not** a security sandbox: it does not restrict file access,
network access, privileges, VRAM, driver memory, or kernel behavior. Only an
explicitly selected trusted helper may be launched; never run untrusted programs.

## Contract

- Commit memory: default 128 MiB, accepted range 64–256 MiB per process and job.
- CPU hard cap: default/maximum 2,000 per 10,000 cycles (20%). Nested job limits
  can make this stricter. This is not a measured CPU performance claim.
- One active process; no breakaway. Below-normal job priority.
- Caller-selected wall deadline 1–40,000 ms. Timeout terminates the job and
  allows a further 2,000 ms to confirm exit. `exit_confirmed=false` is a failure
  to verify termination, never a success report. Windows calls/driver faults
  themselves cannot be given a no-hang guarantee by this API.
- Creation-time `PROC_THREAD_ATTRIBUTE_JOB_LIST` assigns the child before its
  code runs. No create-then-assign orphan window, shell, inherited handles,
  PATH search, privilege elevation, driver installation or persistent service.
- All limits must be installed successfully. Unsupported CPU/nested-job policy
  fails closed; there is no unrestricted fallback.
- Require the memory budget plus 512 MiB of both physical and commit headroom
  before launch. This is a snapshot, not a reservation against other programs.
- An unnamed, non-inherited job uses kill-on-last-handle-close. Exceptional
  paths retain that cleanup. Killing the child does not undo its file writes,
  hence actual filesystem experiments still require disposable owned fixtures.

The executable pathname and arguments are explicitly bounded. Argument quoting
uses Microsoft's CRT convention, including empty strings, quotes and trailing
backslashes; use helpers with that convention. The executable and its parent
location must be trusted and not concurrently replaced. Lexical validation and
an ordinary-file check do not prevent all hostile directory/reparse races.

## Tests

Portable tests cover budget rejection, pathname restrictions, argument-count
and length limits, NULs, empty arguments, whitespace, embedded quotes and
backslash quoting. All seven portable executables passed in local GCC/Zstd,
GCC/raw and Clang ASan/UBSan configurations.

Windows tests launch only their own compiled test binary. They query the real
job settings, check argument round trips and exit codes, attempt one above-cap
virtual-memory commit without touching its pages, verify a second process is
refused, and terminate a sleeping helper on timeout. No busy-loop benchmark,
user files, GPU operation, filesystem mount or driver call is involved. These
Windows assertions require their matching CI result; Linux tests do not run them.

## Iterative research (7 November 2026)

1. Evaluated create-suspended then AssignProcessToJobObject; rejected that
   sequence because the supervisor can die before assignment. Use creation-time
   job attributes supported by the Windows 10+/Server 2016+ target instead.
2. Verified job/process commit limits and CPU hard-cap semantics, including
   nesting and failure cases. Never silently relax unsupported restrictions.
3. Verified CRT quoting against Microsoft documentation and added actual child
   argv round trips, rather than relying solely on a reimplemented parser.

Primary sources:
- https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information
- https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_cpu_rate_control_information
- https://learn.microsoft.com/en-us/windows/win32/procthread/nested-jobs
- https://learn.microsoft.com/en-us/cpp/c-language/parsing-c-command-line-arguments

The preceding lifecycle CI found a Windows SDK `interface` macro collision in
a test parameter. That parameter was renamed, not hidden with a compiler flag
or by disabling the failing test. The production session library compiled in
that run; the new native tests still needed successful execution afterwards.
