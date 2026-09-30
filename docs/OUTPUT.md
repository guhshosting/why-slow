# JSON and human output

JSON stdout is one document:

```json
{"schema_version":1,"version":"1.0.0","mode":"snapshot","samples":[],"interrupted":false}
```

Each samples element contains monotonic_seconds, observations, derived,
unavailable, and findings. A live run always attempts at least one snapshot unless
interrupted before collection. Streaming writes begin the document before sampling
and finish it after the final sample. Consume it as a complete JSON document, not
JSON Lines. SIGINT/SIGTERM finalize it when stdout remains writable.

Observations map dotted source keys to **strings**, including decimal integer
counters. This preserves uint64 precision for consumers whose default JSON number
type is IEEE double. Missing observations are absent rather than zero; unavailable
maps contain source keys with plain reasons. Derived maps contain finite JSON
numbers; units appear in key suffixes (`_bytes`, `_seconds`, `_percent`,
`_per_second`). `disk.*.read_sectors_per_second` uses 512-byte sectors. Raw CPU
fields and process `*_ticks` are clock ticks; `vm.pswp*` are pages; PSI `total_us`
and cgroup `*_usec` are microseconds. Fractional derived values are rounded only
when rendered, not before analysis. Snapshot derived includes gauge ratios and
process RSS/age, but never CPU or counter rates without a valid baseline.

Findings always include id, category, classification, evidence, explanation and
limitations. No score or severity field exists. Fixed keys are stable within
schema version 1; dynamic PID/device/CPU keys depend on visible data. New optional
observations/findings can be added without making missing fields an error.

Text and Markdown show findings with their limitations and the first `--top`
process rows. `--explain` appends selected raw and derived values. JSON is complete
regardless of --explain or --top. Normal output never contains cgroup paths, mount
paths or full process command lines. Dates, hostname and wall-clock timestamps
are deliberately omitted; monotonic time is meaningful only within the run.
