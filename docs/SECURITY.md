# Read-only and parser security

The executable opens input files only with O_RDONLY, never creates a report file,
and uses read-only statvfs/sysconf/sched_getaffinity queries. There are no mutation
paths for sysctl, swap, caches, governors, affinity, storage, services, packages,
process priorities, cgroups or firewall rules. No system(), popen(), exec(), socket,
connect(), ptrace(), tracing attachment or shell command is used. No elevated
privileges, setuid mode or Linux capabilities are requested.

The only write calls deliver the collector's serialized observations to its own
anonymous pipe; standard streams deliver reports/diagnostics. The only kill calls
terminate a PID returned directly by the immediately preceding fork for an owned
collector that exceeded its deadline. An observed PID is never passed to a signal
operation. Build/install/package tools are separate development operations and
are not linked into the runtime executable.

Untrusted numeric fields use full-consumption conversions, checked integer ranges,
finite floats and explicit units. Underflowing counter differences are discarded.
CPU sums cannot wrap integer arithmetic. PSI percentages and sensor readings are
validated. Files and collections have explicit bounds; special files and symlink
fixtures are rejected. See ARCHITECTURE.md for limits and timeout behavior.

Process and sensor names do not become shell commands. Mount paths are used only
for statvfs and cgroup mapping; cgroup traversal rejects `..`, embedded NUL and
oversized paths, strips leading slashes before joining, and stops at the visible
mount root. Fixture path components are opened without following symlinks.

A read-only diagnostic still uses CPU, memory and kernel read interfaces and can
affect access accounting. It cannot guarantee termination of a kernel task stuck
in an uninterruptible syscall, nor can it diagnose a frozen kernel. The supervisor
has bounded waits and reports a failed snapshot in that case. Slow/blocking output
consumers control stdout backpressure; an output failure may leave a truncated
report. Run it as an ordinary user with trusted output consumers.
