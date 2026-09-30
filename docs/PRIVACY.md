# Privacy

No network API, telemetry, remote lookup, external executable or persistent report
writer exists in the application. Reports go only to stdout; diagnostics go to
stderr. Saving a report is an explicit shell redirection or consumer action.

The program does not read process command lines, environments, executable links,
usernames, hostnames, home directories, IP/MAC addresses, machine IDs, serial
numbers or filesystem UUIDs. Mountinfo and cgroup membership are read internally
to resolve scope, but their paths and source names are not reported. Device
major:minor and short kernel block names are reported to distinguish counters.

Names from procfs are sanitized to printable ASCII (other bytes become `?`) and
bounded to 256 bytes. JSON escapes special characters and control bytes. Markdown
escapes dynamic text that could otherwise become formatting. Terminal escape
sequences from process names cannot execute.

Process names themselves are user-controlled and may still reveal application
usage or sensitive text chosen by an application. PIDs, workload rates, counts,
relative uptime and resource limits can characterize a system. Review reports
before sharing. There is no full-command-line opt-in because this version never
collects command lines at all. `--explain` reveals only the same selected safe
observations already present in JSON, not additional private fields.
