# Metric semantics and scope

## CPU and load

`cpu.*` raw counters are USER_HZ ticks from `/proc/stat`. The first eight fields
are used for deltas: user, nice, system, idle, iowait, irq, softirq, steal. Guest
and guest_nice are exposed but not added again: they are already included in
user/nice. `busy_percent` means execution time (user + nice + system + irq +
softirq) divided by the delta sum of those eight fields. Idle, iowait and steal
are separate. This is an explicit definition, not necessarily another tool's
"100 minus idle" convention. Aggregate and individual CPU percentages use their
own accounting deltas. The integer counters remain exact through subtraction;
sums use long double to avoid unsigned overflow. No current utilization is
inferred from one reading. A regression in any field (including iowait) invalidates
that CPU interval. Zero totals and changed aggregate logical CPU count suppress
aggregate utilization. Newly visible CPUs need a new baseline.

Iowait accounting can decrease and has ambiguous attribution on multicore
systems. Neither high iowait nor low execution utilization measures disk latency.
Steal is separately reported virtualization context. The visible `/proc/stat`
CPU count is not guaranteed to be the count available to this process.

The 1/5/15 minute load averages describe runnable and certain uninterruptible
entities. They are not CPU percentages; their history is not the sample window.
The loadavg runnable/total pair counts scheduling entities, while the process scan
counts leaders. `/proc/stat` running/blocked fields are additional point readings;
they can disagree with other reads. D-state leaders are only a subset of blocked
threads and D state is not specific to a storage device.

## Memory and swap

MemTotal, MemFree, MemAvailable, Buffers, Cached, SReclaimable, Shmem, Dirty and
Writeback are exposed when valid, converting procfs kB to bytes (1,024 bytes).
MemAvailable estimates headroom without swapping, including reclaimable memory.
Low MemFree alone is not a finding; unused RAM is deliberately used for cache.
The tool does not sum overlapping cache fields into an invented "used RAM" value.
Absent or inconsistent MemAvailable remains unavailable, without a fallback
based on MemFree. Dirty/writeback are gauges, not rates or latency measurements.

SwapUsed = SwapTotal - SwapFree, when consistent. Occupancy is context: inactive
pages can remain in swap. `pswpin`/`pswpout` deltas are pages per second, using
the measured interval and kernel page size context. Active swap traffic is an
observation, not proof of a user's delay. pgmajfault and supported pgscan counters
are also exposed as rates; major faults can involve file-backed data and do not
by themselves mean swapping. No memory pages are touched to manufacture evidence.

## PSI

`some` describes time with at least one resource-stalled task; `full` describes
time when all non-idle tasks in the scope are stalled. CPU full at system scope
is not used for attention findings. avg10/60/300 are rolling exponentially
weighted percentages. `total` is accumulated microseconds; interval percentage
is delta_total_us / (elapsed_seconds × 10,000). Values over 100.5% are rejected;
100–100.5% is clamped for small read skew. Raw counters remain visible. Parsing
requires all four fields for a line, accepts valid some/full lines independently,
and reports malformed partial lines. Missing PSI is normal on some kernels and
containers. Cgroup PSI describes that hierarchy's workload, not the whole host.
A nonzero stall interval is worth inspection but need not be material to the user.

## Processes

PID plus starttime (field 22 of stat) identifies a lifetime. The name is parsed
between the first opening and last closing parenthesis; spaces and parentheses
are legal. I/O is bracketed by two identity reads; changed/exited lifetimes are
excluded. CPU = delta(utime + stime) / USER_HZ / elapsed × 100, where 100% is one
logical CPU and multithreaded processes can exceed 100%. The two counters are
subtracted separately before summation. New/exited/reused PIDs have no paired
rates. Descendant CPU is not added. Process age uses uptime and start ticks.
RSS is pages × page size; it is approximate and can double-count shared pages
across processes. RSS proportion uses visible MemTotal, not a cgroup limit.

read_bytes/write_bytes are storage-accounted process I/O; rchar/wchar count
requested character transfers and can include cached I/O. Write cancellation,
shared mappings, buffered writes and kernel accounting complicate attribution.
These values do not establish latency or contention. Process names are
user-controlled labels, not authenticated executable identities. Short-lived
processes and blocked nonleader threads may be missed. The tool itself adds a
small amount of observed activity, including a temporary collector process.

## Block devices and capacity

Diskstats are reported **per major:minor**, retaining the sanitized kernel name.
Read/write sector deltas use 512-byte sector units regardless of hardware logical
sector size. I/O milliseconds are raw counters only. No utilization, service time,
latency, busy-device verdict or host aggregate is fabricated. Partitions, dm/LVM,
RAID, loops and underlying disks may represent the same traffic; their counters
are never summed. Device disappearance yields no paired rate; a regression
invalidates that counter. Replacement with the same major:minor and increasing
counters cannot be identified reliably and is a limitation.

statvfs reports total/free/available blocks, fragment size and inode counts.
Available excludes reserved blocks; capacity percentage uses available/total.
Read-only statvfs is limited to a documented local-filesystem allowlist:
ext2/3/4, xfs, btrfs, f2fs, zfs, vfat, exfat, ntfs/ntfs3, overlay and tmpfs.
Network, FUSE and other pseudo filesystems are excluded. A device number is
sampled once to avoid bind duplicates; for overmounted paths the last mountinfo
entry is used. Overlay and its backing store remain separately scoped: they
are never totaled. Tmpfs capacity is a limit rather than physical disk free space.
Quota/subvolume semantics can differ from statvfs; inode counts can be synthetic.
Paths, mount sources and UUIDs are not printed. `filesystem.N` is an ordinal for
that snapshot only, not a stable persistent filesystem identity. Capacity cannot
explain current slowness without other evidence, and this tool makes no such claim.

## Cgroups, virtualization and hardware context

Unified membership is mapped against cgroup2 mount roots. CPU quotas and memory
limits are read up to the visible root (at most 64 levels). The smallest finite
visible ancestor quota/period is a CPU-equivalent upper bound, not dedicated
capacity; siblings can share it. Visible cpuset count and caller affinity are
separate observations. Memory.current at the leaf cannot simply be subtracted
from a shared ancestor limit. `max` means no limit at that level, not globally
unlimited. Memory.high, pids values, CPU throttling counters, memory events and
leaf pressure are exposed. I/O constraints are reported only as present/absent,
not parsed into device claims. Membership changes suppress cgroup deltas. Cgroup
v1 is detected but deliberately not interpreted; hybrid setups use only v2.
Namespace-hidden ancestors may impose tighter constraints. Membership paths
remain private. Nested NSpid and a hypervisor flag/interface are heuristics;
absence cannot prove a bare-metal host.

Thermal zone temperatures use millidegrees Celsius, accepted from -40 to 150 °C.
Frequency values use cpufreq kHz, converted to MHz and accepted from 1 to 10,000
MHz. These broad plausibility bounds detect invalid inputs, not throttling.
Frequency policy caps, idle states and workload affect readings. Only increments
of kernel core/package thermal_throttle counters support an observed thermal
throttle finding; duplicated package/core counters are not totaled. Quota CPU
throttling is separately named. Sensors outside these interfaces are not probed.

## Primary interface references

- [Kernel procfs documentation](https://docs.kernel.org/filesystems/proc.html)
- [Kernel PSI documentation](https://docs.kernel.org/accounting/psi.html)
- [Kernel disk statistics](https://docs.kernel.org/admin-guide/iostats.html)
- [Kernel cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html)

These describe interfaces; why-slow's collection limits and heuristics above
remain implementation choices, not claims endorsed by those references.
