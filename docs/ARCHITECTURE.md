# Architecture and testing design

The application is a single executable linked with an internal C++ library.
There are no runtime dependencies beyond the C++ and C runtimes. No shell or
external helper executable is called by the application.

| Module | Responsibility |
|---|---|
| `parsers.cpp` | Bounded-source text parsing and checked numeric conversions |
| `source.cpp` | Read-only files, process collection/identity, mount capacities, collector supervision and private transport |
| `context.cpp` | Sysfs thermal/frequency context and cgroup membership/mount mapping |
| `sampling.cpp` | Monotonic clock and deterministic next-reading scheduling |
| `analysis.cpp` | Counter deltas, derived metrics, correlation and structured findings |
| `format.cpp` | Terminal-safe text, Markdown and JSON serialization |
| `main.cpp` | CLI validation, signal handling, sampling orchestration and exit codes |
| `core.hpp` | Observation, finding and injectable data-source contracts |

`Source` is a virtual interface for read, directory listing and capacity queries.
Tests inject `Fake` with explicit content; missing data models inaccessible reads.
Analysis accepts snapshots with supplied timestamps: synthetic clocks are entirely
deterministic. Scheduling math accepts explicit times and never sleeps in unit
tests. The production loop waits using short poll sleeps and the steady clock.
A delayed schedule never catches up by inventing measurements or dividing by the
requested interval. The baseline is always separately identifiable as lacking
rates. Each report describes the immediately preceding pair, not a cumulative
whole-session average. JSON retains each reading for independent trend analysis.

## Collection isolation and bounds

A disposable child performs each snapshot. The parent drains an anonymous pipe
with 50 ms polls, a configured deadline and 16 MiB payload cap. On timeout it
terminates **only that collector child**, then performs at most 200 ms of nonblocking
reaping. The application never signals an analyzed process. A kernel task stuck
in uninterruptible sleep cannot be guaranteed to disappear immediately; the parent
never waits indefinitely for it. Collection failure returns an explicit unavailable
snapshot; it cannot be mistaken for a zero-utilization system.

Kernel/fixture files open read-only, nonblocking and close-on-exec; nonregular files
and final-component symlinks are refused. Reads are capped at 1 MiB, directory
entries at 32,768, words per parsed record at 65,536, mount records at 4,096, and
visible cgroup ancestor traversal at 64. The parent transport caps key/value sizes.
These bounds mean very large or unusual systems may produce unavailable or partial
population information. Directory capping is explicitly reported. The worker's
allocation is bounded by these caps, rather than by untrusted file sizes.

Live sysfs has legitimate directory symlinks, which may be followed before the
final regular file. Fixture mode traverses every path component with openat and
O_NOFOLLOW, preventing a fixture symlink from reaching live data. It does not
apply host affinity or statvfs to fixture mount paths. Sysconf supplies page and
clock-tick units for CLI fixtures; pure analysis fixtures may inject other units.

Process stat is parsed before and after I/O reads; PID plus start ticks must
match. Across snapshots the same identity must still match for a rate. The
process scan is deliberately non-atomic; short-lived tasks, thread states and
fast PID reuse within one kernel tick are limitations. Privately retained cgroup
scope data suppresses rates across membership changes; it is never formatted.

## Fixture layout

```
fixture/
  proc/stat
  proc/meminfo
  proc/vmstat
  proc/loadavg
  proc/uptime
  proc/pressure/{cpu,memory,io}
  proc/self/{cgroup,mountinfo,status}
  proc/42/{stat,io}
  proc/fixture-capacity/1
  sys/fs/cgroup/cpu.max
  sys/fs/cgroup/memory.max
  sys/fs/cgroup/memory.current
  sys/fs/cgroup/cpuset.cpus.effective
  sys/class/thermal/thermal_zone0/temp
  sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
```

A capacity fixture has six unsigned decimal fields:
`blocks free_blocks available_blocks block_bytes files free_files`.
N corresponds to the deduplicated accepted mount ordinal for that snapshot.
Cgroup descendants are placed beneath `sys/fs/cgroup` according to membership.
Tests build fixtures dynamically inside temporary directories, then delete them.
Fixtures contain no captured host identifiers.

## Test layers

Unit cases verify parsers, rate math, scope tracking and conservative classifications.
Dedicated synthetic cases cover each misleading inference prohibited by the
requirements. Integration cases run the compiled executable with real temporary
fixture files, including malformed/oversized/FIFO/symlink inputs, read-only checks,
sampling, signals, broken pipes and machine-readable output. Dynamic fixture
changes exercise PID reuse and counter reset. No stress workload is used.
Sanitizer builds exercise the same suites. `VALIDATION.md` identifies which
checks ran successfully in this builder, including capability limitations.

A collector deadline/failure ends the sampling session after reporting that failed
reading (exit 1), preventing accumulation of stuck collectors.
