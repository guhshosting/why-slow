# Findings and causal honesty

A finding is `{id, category, classification, evidence, explanation, limitations}`.
IDs are stable rule names; device/CPU/pressure instances append their source key.
IDs do not encode severity. Filesystem ordinal suffixes identify only the current
snapshot. Evidence supplies quantitative observations; raw values remain in JSON.

| Classification | Meaning |
|---|---|
| OBSERVED | Directly collected gauge or calculated counter change |
| INDICATOR | Evidence meriting attention; no cause is asserted |
| CONTEXT | Scope or interpretation context |
| UNAVAILABLE | Data missing, restricted, malformed, reset or otherwise unusable |
| INSUFFICIENT EVIDENCE | Required evidence for a stronger conclusion is absent |

## Explicit heuristics

| Rule | Condition | Interpretation |
|---|---|---|
| cpu.utilization | Execution >=90% of accounting delta | Most visible execution capacity was busy in this interval |
| memory.available | MemAvailable <10% of valid MemTotal | Limited estimated host-visible headroom |
| process.blocked | At least one scanned D-state leader | Uninterruptible work was observed |
| load.blocked_context | Load1 > logical CPU count, execution <50%, D leaders present | Load may include blocked work; time windows differ |
| pressure.* | Valid sampled resource stall fraction >0 | Stall time was recorded; significance depends on workload |
| storage.capacity.* | Unprivileged available blocks <=10% | Capacity attention only |
| storage.inodes.* | Free inodes <=1% with nonzero inode total | Inode capacity attention only |
| memory.swap_pressure | Active swapping and memory-some PSI delta >0 | Concurrent traffic and stalls, without causal attribution |

These deliberately broad attention rules are not universal performance boundaries.
They do not produce a global verdict. A 50 ms interval and a 30 second interval
have different noise and relevance, and the measured interval is provided. No
rule treats a short-window observation as evidence of a sustained condition.
Changing thresholds to make the output look more dramatic is not appropriate.
CPU load is never interpreted using only load/CPU count. Affinity and cgroup
limits are context rather than scaling host counters to a fictional container CPU
percentage. The tool does not infer arbitrary process-count limits; it reports
the population, zombies, D leaders, and observed population growth.

Swap occupancy is context. Positive swap deltas are observations. Block and process
I/O traffic are observations. PSI is a separate measure of resource stalls.
Kernel CPU-controller throttling increments are observations of quota enforcement;
thermal-throttle counter increments are distinct hardware-specific observations.
Temperature/frequency gauges never substitute for throttling evidence.

A "no indicator" finding means only that no selected attention condition matched
available data. It is explicitly insufficient evidence, not a clean bill of health.
Missing PSI or procfs coverage cannot be treated as proof of no pressure.

## Manual investigation

Read the scope and unavailable entries first. Prefer a representative sample while
the reported issue occurs. Compare repeated windows and correlate resource PSI,
execution, runnable/blocked observations, swap activity and process rates. An
application's own latency/lock/queue metrics may be needed. None of the report's
correlations establishes which application or resource caused the user's symptom.
No automatic corrective action is implemented or recommended as universally safe.

## False-positive acceptance

Tests explicitly reject converting high load to a CPU bottleneck, occupied swap
to active swapping, low MemFree to inadequate RAM, iowait to a disk bottleneck,
busy processes to blame, I/O traffic to contention, capacity to a slowdown cause,
or high temperature/reduced frequency to thermal throttling. Tests assert both
classification/absence of findings and forbidden causal wording. Counter resets,
missing baselines and scope changes suppress unsupported rates.
