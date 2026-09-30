# why-slow 1.0.0 — validation report

Validation date: 2026-09-17. The validation below uses the final source unless
explicitly identified as an earlier investigation. This is container validation,
**not physical Debian 13 validation**. No stress workloads were used.

## Environment and tools

- Ubuntu 24.04.3 LTS, x86_64; kernel interface reports Linux 6.18.44.
- GCC/G++ 13.3.0 (`Ubuntu 13.3.0-6ubuntu2~24.04`).
- CMake/CTest/CPack 3.30.5; Python 3.12.14 for integration tests.
- cppcheck 2.13.0; GNU groff 1.23.0; dpkg-deb 1.22.6; RPM/rpmbuild 4.18.2.
- Runtime scope is container/namespace-visible. Cgroup v2, a hypervisor flag,
  overlay filesystems and resource constraints were exposed. Earlier sessions
  also exposed nested NSpid context; the final live run did not expose that flag.
- Tool binaries were restored after an execution-service interruption. RPM and
  cppcheck were run from extracted distribution packages with their matching
  libraries/configuration. This is build tooling, not a runtime dependency.

## Final build and test results

| Configuration/check | Result | Exact scope |
|---|---|---|
| GCC Debug | PASS | Complete build; 110 unit cases + 41 CLI tests |
| GCC Release | PASS | Complete build; 110 unit cases + 41 CLI tests |
| ASan + UBSan Debug | PASS | Same 110 + 41; leak detection disabled |
| cppcheck | PASS | 8 translation units including unit tests; zero remaining diagnostics with the reviewed exclusions below |
| Man page | PASS | `groff -ww -Tutf8 -man`; empty diagnostic stream |
| Staged installation | PASS | Install payload, exact man content, help/version, 41 installed-binary CLI tests and live sample |
| CPack DEB | PASS | Generated package metadata, dependencies, payload, no hooks, extracted executable |
| CPack RPM | PASS | Generated package metadata, dependencies, payload, no hooks, extracted executable and decompressed man content |
| LeakSanitizer | UNAVAILABLE | Runtime fatal error under container restrictions; no leak-free claim |

CTest registers two entries: `unit` and `integration`. The **151 cases** are
110 individually named C++ cases and 41 Python unittest methods. CLI option
subtests are not inflated into extra case counts. Tests use assertions that
remain enabled in Release.

Every configuration compiles with C++17 and
`-Wall -Wextra -Wpedantic -Werror`. Debug and Release compile logs had no
compiler warnings/errors. The complete suites were rerun after the final parser,
quota-precision and synchronization corrections.

Reproduction:

```sh
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j2
ctest --test-dir build-debug -V
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-release -j2
ctest --test-dir build-release -V
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DWHY_SLOW_SANITIZE=ON
cmake --build build-asan -j2
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan -V
```

Builds and environment-dependent commands were externally bounded. Tests have
CTest deadlines (unit 30 seconds, integration 60 seconds); CLI subprocess calls
also have explicit Python timeouts. The production collector has its own deadline.

## Fixture and synthetic coverage

Unit tests cover CPU accounting/guest treatment, multicore percentages, zero and
negative elapsed time, zero accounting deltas, reset and huge counters, CPU-count
changes, duplicate CPU records, load parsing/context, MemAvailable/cache semantics,
swap occupancy/activity/reset/partial counters, dirty/writeback and units.

PSI cases cover valid resource lines, missing/partial/malformed lines, duplicate
fields, uint64 totals, interval percentages, impossible intervals and system CPU
full interpretation. Storage cases cover escaped and malformed mounts, embedded
NUL rejection, pseudo/duplicate/overmounted filesystems, capacity, per-device
statistics, stacked-device nonaggregation and resets. Process cases cover CPU,
RSS/age, I/O, unusual parentheses/spaces/control bytes, malformed stat, exit/new
lifetimes, PID reuse, zombies and inaccessible-source behavior.

Cgroup cases cover quotas, fractional/huge-period precision, max, malformed
values, cpusets, ancestor limits, scope changes, missing controllers, unsafe paths,
repeated slashes, root deduplication and v1 detection. Sensors cover units, signed
temperatures, plausible ranges, malformed values and counter-only throttle claims.

Dedicated causal tests verify that high load, occupied swap, low MemFree, high
iowait, busy processes, high I/O traffic, capacity and temperature/frequency do
not become unsupported slowdown diagnoses. Synthetic combinations test CPU busy
intervals, blocked load, active swapping plus memory PSI, I/O PSI, quota context,
multiple indicators and no selected indicators. Fixed forbidden positive causal
phrases are checked alongside finding IDs and classifications.

CLI tests exercise all four modes, text/JSON/Markdown, intervals/duration/count
limits, explain/top, invalid options, missing and malformed roots, symlink/FIFO/
oversized inputs, privacy, read-only fixture contents, SIGINT/SIGTERM, broken
stdout, cgroups, sensors, PID reuse and resets. A 100-reading run checks repeated
sampling. Unit tests also perform 20 consecutive supervised collections and a
slow injected-source deadline test; neither intentionally overloads the builder.
A one-hour soak or uninterruptible physical-device hang was not performed.

The PID-reuse test initially had a test-harness race under instrumentation: a
fixed sleep could replace the fixture before the baseline. It now waits for the
first flushed JSON sample, verifies its identity, then replaces the fixture and
verifies the new identity. Reset and signal tests likewise synchronize with actual
output. The final full sanitizer suite passed after that repair.

## Sanitizer and static-analysis interpretation

ASan/UBSan completed all 151 cases with `detect_leaks=0` and halt-on-error enabled;
no address or undefined-behavior report was produced. Final leak-enabled ASan
unit execution was bounded to 10 seconds and exited 1 with a fatal LeakSanitizer
runtime error and ptrace-related hint. A final separately compiled
`-DWHY_SLOW_LEAK=ON` unit execution, also bounded to 10 seconds, exited 23 with
the same runtime fatal error. Earlier attempts specifically reported inability
to open `/proc/<pid>/task`. Leak checking is therefore unvalidated; the final
standalone LSan full suite was not pursued after the bounded unit runtime failure.
Collector children use `_exit`; core collection/analysis logic is also tested in
the unit executable, but no child-process leak-check claim is made.

cppcheck used `--enable=warning,style,performance,portability --inconclusive
--inline-suppr --error-exitcode=1 --std=c++17 --library=posix` with the Release
compilation database. `missingIncludeSystem` is excluded because system headers
are modeled rather than analyzed. One local, documented `readdirCalled`
portability suppression is justified by the collector's single-threaded,
private-DIR use; there is no concurrent access. No application correctness warning
was blanket-suppressed. Mount separator bounds were rewritten as checked indices;
duplicate error branches, constness, declaration-name consistency and reported
small algorithm-style issues were addressed. The final run returned 0 with no
remaining diagnostics.

## Packaging and installation

CMake installation was staged using DESTDIR with prefix `/usr`; no package was
installed into the builder's live OS. Payload includes `/usr/bin/why-slow`, its
man page, README, MIT license and six design/semantics/privacy/output documents.
There is no service, timer, capability, setuid executable or configuration writer.
The installed binary passed all 41 CLI tests. The staged man page matched source
bytes exactly. Its help/version and final live sample succeeded.

CPack DEB generated `why-slow-1.0.0-x86_64.deb`. Actual control metadata reports
Package `why-slow`, Version `1.0.0`, Architecture `amd64`, Maintainer `guhshosting`,
and discovered dependencies `libc6 (>= 2.34), libgcc-s1 (>= 3.0), libstdc++6
(>= 13.1)`. Payload/control archives were inspected and extracted. Control files
were only `control` and `md5sums`; no maintainer scripts exist. The extracted
executable returned the correct version and the installed man matched source.

CPack RPM generated `why-slow-1.0.0-x86_64.rpm` using rpmbuild 4.18.2. RPM query
verified metadata, dependency requirements, a 16-entry payload and empty script
hooks. The RPM Vendor field is `guhshosting` and its description explains the application; CPack default vendor/installer text was removed. Regular files were extracted from its cpio payload; the extracted binary
returned the correct version, and the compressed manual decompressed to exact
source content. Runtime library requirements include GLIBCXX_3.4.32 from this
Ubuntu toolchain. RPM-generated build-ID links are package metadata only and are
not present in the source archive.

No RPM installation transaction or RPM-native operating-system test was performed.
The separately supplied native Debian debhelper recipe and native RPM spec were
not built through dpkg-buildpackage/rpmbuild -ba; tested generation was CPack.
Project maintainer metadata uses the established project owner without inventing
a personal email. A distribution submitting the native Debian recipe may require
its actual packager's contact address under local policy. No placeholder contact
or project URLs remain. Builder binaries are not substitutes for compiling on
Debian 13. Binary packages are deliberately excluded from the source deliverable.

## Actual live environment validation

The final staged Release binary executed:

```sh
why-slow sample --samples 3 --interval 0.2 --format json
```

It exited 0, produced valid JSON with three readings, and wrote no diagnostics.
Actual adjacent intervals were 0.199502808 and 0.200700819 seconds. Collection
costs were 0.004083, 0.004270 and 0.004232 seconds. This validates timing behavior,
not an application-latency benchmark.

Available: aggregate/per-CPU counters; 9 logical CPUs; affinity/cpuset count 9;
load and task context; memory/cache/swap/vmstat observations; 5 visible process
leaders and their readable data; 26 disk records; 5 deduplicated capacity records
of types overlay/tmpfs; unified cgroup controls with an 8 CPU-equivalent quota and
15,032,385,536-byte visible memory limit; hypervisor-flag context.

Unavailable: system and cgroup PSI; thermal-zone and cpufreq readings. Bounded
direct checks of `/proc/pressure/{cpu,memory,io}` and cgroup cpu.pressure returned
ENOENT. No absence-of-pressure or absence-of-throttling conclusion is made.
An earlier live three-reading session exposed naturally changing process counts
and inaccessible process I/O; it was not used to manufacture findings. The final
run had no process-I/O unavailable entry. No physical sensor, real storage latency,
physical thermal throttling, host-level or Debian 13 result is claimed.

Changing to an ordinary UID/GID was rejected with EINVAL by this builder. Actual
unprivileged execution and chmod-based EACCES testing could not be established;
injected/missing-source tests cover the same unavailable branches. The application
never requests privileges, and partial process visibility was observed earlier.

## Final reviews

1. **Causal honesty:** findings, output and documentation separate all five
   classifications; evidence and limitations accompany each rule. No health
   score or causal attribution to a top process is generated.
2. **Metric correctness:** checked CPU guest/steal/iowait semantics, load windows,
   MemAvailable/cache treatment, swap activity, PSI scope, 512-byte disk sectors,
   process start identity/RSS units, cgroup ancestors and sensor conversions.
3. **False positives:** synthetic classifications and thresholds reviewed; raw
   traffic/capacity/temperature alone cannot become a proven slowdown cause.
4. **Parser robustness:** checked numeric bounds, zero/reset/wrap cases, input
   caps, strings, malformed mounts/cgroups, repeated CPU lines, NUL paths and
   source disappearance. Tiny nonzero quotas retain precision rather than
   rounding to zero. Zero MemTotal no longer counts as valid core data for exit 0.
5. **Sampling:** monotonic observed intervals, deterministic schedule tests,
   missed deadlines, topology/scope changes, process lifetime and zero-duration
   analysis cases verified. No catch-up samples are fabricated.
6. **Privacy:** selected name-only identities, no arguments/environments or
   unnecessary machine/network identifiers; private membership paths are not
   formatted; control bytes and Markdown/JSON metacharacters are handled.
7. **Read-only/external operations:** inspected all file opens and writes. Only
   read-only inputs, own-pipe output and stdout/stderr exist. No network, shell,
   optimizer or system-mutation API is called. Signals target only owned collector
   children returned by fork, never observed PIDs.
8. **Resources and long-running behavior:** descriptors and directory streams
   close on ordinary paths; worker lifetime and payload are bounded; failed
   collectors stop further sampling. Repeated tests passed. A kernel-uninterruptible
   child cannot be guaranteed to disappear immediately; parent reaping is bounded.

## Known limitations

Reads and process scans are not atomic. Short-lived processes/nonleader threads
can be missed; RSS is approximate and shared pages can be counted repeatedly.
PID reuse within one clock tick and device replacement with reused identifiers
cannot always be distinguished. Cgroup v1 is detected but not interpreted;
hidden/shared ancestors and namespace visibility limit conclusions. Block stacks
are reported separately, never summed; storage latency is not inferred. Local
filesystem allowlisting excludes remote/FUSE filesystems, and filesystem ordinal
identifiers last only for one snapshot. Input caps can limit very large systems.
No application-lock, network-latency or physical-hardware diagnosis is promised.
Names and workload metadata can still be sensitive. Slow stdout consumers can
backpressure output; fatal output errors may truncate JSON. Leak validation,
physical Debian 13 validation and native package-manager installation remain
outside the demonstrated coverage.

## Source archive and clean-room gate

The deliverable is `why-slow-1.0.0-src.tar.gz`, with top-level directory
`why-slow-1.0.0`. Packaging uses an explicit source-file allowlist, then checks
member names and bytes for generated artifacts, VCS metadata, binary formats,
logs, caches, package files and host reports. Only regular source/documentation
files are included; `.gitignore` and `.clang-format` are intentional source
configuration, not repository history.

**PASS:** A preliminary source archive was inspected and built/tested after
extraction. The final archive was then regenerated with this completed report,
inspected again, and extracted into a second brand-new directory. The exact final
archive passed separate Release configuration, a complete strict GCC build and
all **110 unit cases + 41 CLI integration cases** (2/2 CTest entries). No earlier
build tree or compiled object was reused. Its member list contains **30 regular
UTF-8 source/documentation files**, with no binaries, build directories, packages,
logs, caches, sanitizer artifacts, compressed man pages, host reports or VCS
metadata. The final archive is unchanged after that validation.
