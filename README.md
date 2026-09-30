# why-slow 1.0.0

Read-only Linux performance observations, with explanations that distinguish
measured activity from possible resource pressure. C++17, CMake, CLI only, MIT.
No network access, telemetry, automatic optimization, or root requirement.

A metric does not prove why your application is slow. why-slow reports its
sampling scope, evidence and limitations, including missing data. It never
produces a health/performance score.

## Build and test

Requires Linux, GCC with C++17 support, CMake >=3.16 and a build tool. Python 3
is required for the CLI test suite, not to run the installed executable.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/why-slow --help
```

Strict `-Wall -Wextra -Wpedantic -Werror` is always enabled. No dependency is
fetched at configure, build, test or runtime. To omit tests on a runtime-only
build, pass `-DBUILD_TESTING=OFF`.

## Use

```sh
why-slow snapshot
why-slow sample --interval 1 --samples 6
why-slow sample --duration 30 --interval 2 --format json
why-slow processes --interval 2 --top 15
why-slow summary --interval 2 --format markdown
why-slow snapshot --explain
```

Snapshot captures gauges and cumulative counters; it does **not** calculate
current CPU utilization from boot totals. `sample` defaults to three readings;
`processes` and `summary` are convenient two-reading modes using the same complete
analysis. All modes include findings and process rankings. Human output ranks
by sampled CPU when available, otherwise RSS. JSON includes all collected
processes and per-device counters, independent of `--top`.

`--samples` is total readings, including the initial baseline, not intervals.
`--duration` and `--samples` are alternatives. Intervals are 0.05–60 seconds;
duration is at most one hour; at most 1,000 readings are allowed. Very short
intervals have substantial scheduler/accounting/read-skew noise. Use seconds
rather than milliseconds for investigation. Rates use actual monotonic elapsed
time, not the requested sleep interval. Collection duration is reported.

`--timeout` bounds each entire collection (default 5 seconds, range 0.1–30).
If a collector misses the deadline its snapshot is marked unavailable; the
program does not silently return partial data as a complete sample. Slow
stdout consumers can backpressure output; no output is written to disk by the
application. Redirect stdout yourself only if you want a persistent report.
Ctrl-C/SIGTERM finishes a valid JSON document where output is still writable.
It may take the remainder of the collection deadline plus bounded child cleanup.

Exit codes: **0** report with at least one core source; **1** a collector failed, or no CPU, memory or
process core data in any reading; **2** invalid usage; **3** output/internal error;
**130** SIGINT; **143** SIGTERM; **141** broken stdout pipe. Optional unavailable
metrics are normal and do not alone make exit status nonzero. Exit 0 is not a
claim of good performance. After a fatal output error, JSON may be incomplete.

## Read the evidence

- **OBSERVED**: a measured gauge, counter or delta.
- **INDICATOR**: quantitative evidence worth investigating; it is not a cause.
- **CONTEXT**: information needed to interpret other observations.
- **UNAVAILABLE**: missing, inaccessible, malformed or unusable data.
- **INSUFFICIENT EVIDENCE**: a stronger explanation is not supported.

Low MemFree alone, occupied swap, high I/O traffic, a busy process, high load,
or high temperature cannot establish the cause of a slowdown. See
[metric semantics](docs/METRICS.md) and [finding methodology](docs/METHODOLOGY.md).

## Install and package

```sh
cmake --install build --prefix /usr/local
# Choose /usr as configure-time prefix for distribution packages:
cmake -S . -B build-pkg -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-pkg -j2
ctest --test-dir build-pkg --output-on-failure
(cd build-pkg && cpack -G DEB)
(cd build-pkg && cpack -G RPM)
```

Use appropriate privileges for installation, never for normal diagnosis. DEB
requires dpkg tools; RPM requires rpmbuild. Native Debian and RPM source recipes
are also in `packaging/`. The binary and uncompressed man page are installed;
no service, timer, capability, setuid bit, config file or maintainer script is
installed. See [packaging notes](packaging/README.md).

## Reproducible testing

`--fixture-root DIR` reads `DIR/proc` and `DIR/sys`. Fixture files must be real
files/directories, not symlinks. Filesystem statistics come from explicitly
supplied `proc/fixture-capacity/N` fixtures, never live host mount paths. The
fixture format and injectable source/clock design are in
[architecture](docs/ARCHITECTURE.md). Tests do not create pressure on the host.

[VALIDATION.md](VALIDATION.md) records exact tested configurations and limitations.
Container validation is not physical Debian 13 validation. Primary limitations:
non-atomic reads, namespace scope, no application-latency/lock/network diagnosis,
no per-thread scan, no cgroup v1 interpretation, and no storage latency inference.

Privacy details: [PRIVACY.md](docs/PRIVACY.md). JSON contract:
[OUTPUT.md](docs/OUTPUT.md). Security/read-only review:
[SECURITY.md](docs/SECURITY.md).

A collector deadline/failure ends the sampling session after reporting that failed
reading (exit 1), preventing accumulation of stuck collectors.
