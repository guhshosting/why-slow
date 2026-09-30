#include "why_slow/core.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
namespace ws {
static std::string fmt(double v) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(2) << v;
  return s.str();
}
Report analyze(const Snapshot &b, const Snapshot *a) {
  Report r;
  r.snapshot = b;

  auto finding = [&](const std::string &id, const std::string &cat,
                     const std::string &cls, const std::string &evidence,
                     const std::string &explanation, const std::string &limit) {
    r.findings.push_back({id, cat, cls, evidence, explanation, limit});
  };

  const double elapsed = a ? b.time - a->time : 0;

  auto change = [&](const std::string &key) -> std::optional<U> {
    if (!a || elapsed <= 0 || !std::isfinite(elapsed))
      return {};
    if (key.rfind("cgroup.", 0) == 0 && a->identity != b.identity)
      return {};
    auto x = get_u(a->values, key), y = get_u(b.values, key);
    return x && y ? delta(*x, *y) : std::nullopt;
  };

  if (a && elapsed > 0 && std::isfinite(elapsed))
    r.rates["elapsed_seconds"] = elapsed;

  std::set<std::string> cpus;

  for (const auto &x : b.values)
    if (x.first.rfind("cpu.", 0) == 0 && x.first.size() > 5 &&
        x.first.substr(x.first.size() - 5) == ".user")
      cpus.insert(x.first.substr(0, x.first.size() - 4));

  const std::vector<std::string> fields = {"user",   "nice", "system",  "idle",
                                           "iowait", "irq",  "softirq", "steal"};

  for (const auto &p : cpus) {
    long double total = 0;
    std::vector<U> ds;
    bool valid = !(a && p == "cpu.total." &&
                   get(a->values, "cpu.logical") != get(b.values, "cpu.logical"));
    for (const auto &field : fields) {
      auto d = change(p + field);
      if (!d) {
        valid = false;
        break;
      }
      ds.push_back(*d);
      total += *d;
    }
    if (valid && total > 0) {
      for (std::size_t j = 0; j < ds.size(); ++j)
        r.rates[p + fields[j] + "_percent"] =
            static_cast<double>(100.0L * ds[j] / total);

      r.rates[p + "busy_percent"] = static_cast<double>(
          100.0L * (static_cast<long double>(ds[0]) + ds[1] + ds[2] + ds[5] + ds[6]) /
          total);

    } else if (a)
      r.snapshot.unavailable[p + "delta"] =
          "counter reset, changed topology, missing counters, or zero "
          "accounting delta";
  }
  for (const auto &x : b.values) {
    if (x.first.size() > 9 && x.first.substr(x.first.size() - 9) == ".total_us") {
      auto d = change(x.first);
      if (d) {
        double pct = static_cast<double>(*d) / 10000.0 / elapsed;
        if (pct <= 100.5)
          r.rates[x.first.substr(0, x.first.size() - 9) + ".interval_percent"] =
              std::min(100.0, pct);
        else
          r.snapshot.unavailable[x.first + ".delta"] =
              "PSI delta exceeds interval; scope/reset/read skew uncertain";
      }
    }
    if (x.first.rfind("disk.", 0) == 0 &&
        (x.first.find("_sectors") != std::string::npos)) {
      auto d = change(x.first);
      if (d)
        r.rates[x.first + "_per_second"] = static_cast<double>(*d) / elapsed;
    }
    if (x.first.rfind("throttle.", 0) == 0) {
      auto d = change(x.first);
      if (d && *d > 0)
        finding("thermal.throttle_counter." + x.first, "thermal", "OBSERVED",
                x.first + " increased by " + std::to_string(*d),
                "A kernel thermal-throttle counter increased during this interval.",
                "Hardware-specific scope; counters may duplicate package/core "
                "events. Does not establish application impact.");
    }
  }
  for (const auto *counter :
       {"pswpin", "pswpout", "pgmajfault", "pgscan_kswapd", "pgscan_direct"}) {
    auto d = change(std::string("vm.") + counter);
    if (d)
      r.rates[std::string("vm.") + counter + "_per_second"] =
          static_cast<double>(*d) / elapsed;
  }
  for (const auto *key :
       {"cgroup.cpu.stat.nr_throttled", "cgroup.cpu.stat.throttled_usec",
        "cgroup.cpu.stat.usage_usec", "cgroup.memory.events.high",
        "cgroup.memory.events.oom", "cgroup.memory.events.oom_kill"}) {
    auto d = change(key);
    if (d)
      r.rates[std::string(key) + "_delta"] = static_cast<double>(*d);
  }
  if (a && elapsed > 0 && std::isfinite(elapsed))
    finding("sampling.window", "sampling", "CONTEXT",
            "elapsed=" + fmt(elapsed) + " seconds; collection=" +
                get(b.values, "collection.seconds") + " seconds",
            "Rates use the measured monotonic interval between collection starts.",
            "Reads are sequential, not atomic. Collection time and scheduler delay "
            "create skew, especially with short intervals.");
  std::size_t active_disks = 0;
  for (const auto &value : b.values) {
    if (value.first.rfind("disk.", 0) != 0 || value.first.size() < 5 ||
        value.first.substr(value.first.size() - 5) != ".name")
      continue;
    auto prefix = value.first.substr(0, value.first.size() - 4);
    auto rd = r.rates.find(prefix + "read_sectors_per_second"),
         wr = r.rates.find(prefix + "write_sectors_per_second");
    if ((rd != r.rates.end() && rd->second > 0) ||
        (wr != r.rates.end() && wr->second > 0))
      ++active_disks;
  }
  if (active_disks)
    finding("io.device_activity", "io", "OBSERVED",
            std::to_string(active_disks) +
                " visible device records have positive traffic deltas",
            "Per-device sector rates are available in derived observations.",
            "Stacked devices and partitions may describe the same traffic. Rates "
            "are never summed and do not demonstrate latency or contention.");
  if (b.values.count("cpu.affinity_count") || b.values.count("cgroup.cpuset_count"))
    finding("cpu.constraints", "container/cgroup", "CONTEXT",
            "caller affinity CPUs=" + get(b.values, "cpu.affinity_count") +
                "; cgroup cpuset CPUs=" + get(b.values, "cgroup.cpuset_count"),
            "Placement constraints may differ from the logical CPU count "
            "visible in procfs.",
            "Counts do not establish dedicated capacity. Quotas and shared "
            "ancestor limits are separate constraints.");
  auto cpu = r.rates.find("cpu.total.busy_percent");

  if (cpu != r.rates.end())
    finding("cpu.utilization", "cpu", cpu->second >= 90 ? "INDICATOR" : "OBSERVED",
            "busy=" + fmt(cpu->second) +
                "%; iowait=" + fmt(r.rates["cpu.total.iowait_percent"]) +
                "%; steal=" + fmt(r.rates["cpu.total.steal_percent"]) + "%",
            cpu->second >= 90
                ? "Most visible CPU accounting time was executing work during this "
                  "interval."
                : "CPU execution utilization was measured from accounting deltas.",
            "An aggregate can hide one saturated CPU. Utilization alone does not "
            "establish contention or the cause of application latency; iowait is "
            "not a storage bottleneck measurement.");

  else
    finding("cpu.no_rate", "cpu", "INSUFFICIENT EVIDENCE",
            "No valid paired CPU counters", "CPU utilization needs two valid readings.",
            "Snapshot counters are cumulative, not current percentages.");

  auto load = get_n(b.values, "load.1"), logical = get_n(b.values, "cpu.logical");

  if (load)
    finding("load.context", "load", "CONTEXT",
            "1-minute load=" + fmt(*load) +
                "; logical CPUs=" + get(b.values, "cpu.logical") +
                "; runnable=" + get(b.values, "load.runnable") +
                "; blocked=" + get(b.values, "tasks.procs_blocked"),
            "Load average includes runnable tasks and certain uninterruptible "
            "tasks; it is not CPU percentage.",
            "Load history differs from this sampling window. CPU quotas, affinity, "
            "and namespace scope can make host CPU count misleading.");

  auto blocked = get_u(b.values, "process.D_count");
  if (blocked && *blocked > 0)
    finding("process.blocked", "process", "INDICATOR",
            std::to_string(*blocked) + " visible process leaders in D state",
            "Uninterruptible tasks were present at collection time.",
            "D state is not specific to disk I/O; threads are not separately "
            "enumerated and the scan is not atomic.");

  if (load && logical && *load > *logical && cpu != r.rates.end() && cpu->second < 50 &&
      blocked && *blocked > 0)
    finding("load.blocked_context", "load", "INDICATOR",
            "Load exceeds visible logical count, CPU execution <50%, and D-state "
            "leaders present",
            "These observations are consistent with load containing blocked work.",
            "Different time windows and non-atomic reads prevent assigning a "
            "cause; high load does not prove CPU contention.");

  auto available = get_n(b.values, "memory.MemAvailable_bytes"),
       total = get_n(b.values, "memory.MemTotal_bytes");

  if (available && total && *total > 0) {
    double pct = 100.0 * (*available / *total);
    r.rates["memory.available_percent"] = pct;
    finding("memory.available", "memory", pct < 10 ? "INDICATOR" : "CONTEXT",
            "MemAvailable=" + fmt(pct) + "% of visible MemTotal",
            "MemAvailable estimates memory usable without swapping, including "
            "reclaimable cache.",
            "Less than 10% is an attention heuristic, not proof of inadequate "
            "RAM. MemFree alone is not used. Host estimates may not describe "
            "cgroup headroom.");
  }
  auto swap = get_u(b.values, "memory.SwapUsed_bytes");
  if (swap && *swap > 0)
    finding("swap.occupied", "swap", "CONTEXT",
            std::to_string(*swap) + " bytes of swap occupied",
            "Pages residing in swap may be old and inactive.",
            "Occupancy alone does not demonstrate active swapping or "
            "performance degradation.");

  auto si = r.rates.find("vm.pswpin_per_second"),
       so = r.rates.find("vm.pswpout_per_second");

  if (si != r.rates.end() && so != r.rates.end() && (si->second > 0 || so->second > 0))
    finding("swap.activity", "swap", "OBSERVED",
            "swap-in=" + fmt(si->second) + " pages/s; swap-out=" + fmt(so->second) +
                " pages/s",
            "Swap traffic counters increased in this interval.",
            "Traffic alone does not establish application impact; correlate "
            "memory PSI and reclaim. Pages are kernel page units, not bytes.");

  bool pressure = false;

  for (const auto &x : r.rates)
    if (x.first.find(".interval_percent") != std::string::npos &&
        x.first != "psi.cpu.full.interval_percent" && x.second > 0) {
      pressure = true;
      finding("pressure." + x.first, "pressure", "INDICATOR",
              x.first + "=" + fmt(x.second) + "%",
              "PSI recorded time with resource-stalled work during this interval.",
              "Some means at least one task stalled; full means all non-idle tasks "
              "in scope stalled. This does not identify which application was "
              "delayed. CPU full at system scope is not interpreted as "
              "saturation.");
    }
  if (!a)
    for (const auto &x : b.values)
      if (x.first.find(".avg10") != std::string::npos) {
        auto n = number(x.second);
        if (n && *n > 0)
          finding("pressure.history." + x.first, "pressure", "CONTEXT",
                  x.first + "=" + x.second + "%",
                  "PSI rolling history records recent resource stalls.",
                  "The 10-second exponentially weighted average is not a "
                  "measurement of this snapshot interval.");
      }
  if (pressure && si != r.rates.end() && so != r.rates.end() &&
      (si->second > 0 || so->second > 0)) {
    auto mp = r.rates.find("psi.memory.some.interval_percent");
    if (mp != r.rates.end() && mp->second > 0)
      finding("memory.swap_pressure", "memory", "INDICATOR",
              "Active swap traffic and memory PSI stalls in the same interval",
              "The observations are consistent with memory pressure involving swap "
              "activity.",
              "Correlation does not establish that swapping caused the reported "
              "application slowdown; workload attribution is unavailable.");
  }
  for (const auto &x : b.values)
    if (x.first.rfind("filesystem.", 0) == 0 && x.first.size() > 7 &&
        x.first.substr(x.first.size() - 7) == ".blocks") {
      auto p = x.first.substr(0, x.first.size() - 6);
      auto blocks = integer(x.second), free = get_u(b.values, p + "free_blocks"),
           avail = get_u(b.values, p + "available_blocks");
      if (blocks && free && avail && *blocks > 0 && *free <= *blocks &&
          *avail <= *free) {
        const double pct =
            100.0 * static_cast<double>(*avail) / static_cast<double>(*blocks);
        r.rates[p + "available_percent"] = pct;
        if (pct <= 10)
          finding("storage.capacity." + p, "storage", "INDICATOR",
                  p + "available=" + fmt(pct) + "%; type=" + get(b.values, p + "type"),
                  "This filesystem has at most 10% of blocks available to an "
                  "unprivileged caller.",
                  "Capacity is independent of latency and contention. Reserved "
                  "blocks affect availability; no current slowdown is "
                  "attributed to fullness.");
      }
      auto files = get_u(b.values, p + "files"), ff = get_u(b.values, p + "free_files");
      if (files && ff && *files > 0 && *ff <= *files &&
          static_cast<long double>(*ff) / *files <= 0.01L)
        finding("storage.inodes." + p, "storage", "INDICATOR",
                p + "free inodes=" + std::to_string(*ff),
                "At most 1% of reported inodes remain free.",
                "Filesystem inode accounting varies; this does not establish a "
                "cause of slowness.");
    }
  auto hz = get_n(b.values, "system.tick_hz"),
       page = get_n(b.values, "system.page_bytes"),
       uptime = get_n(b.values, "system.uptime_seconds");

  for (const auto &x : b.values)
    if (x.first.rfind("process.", 0) == 0 && x.first.size() > 11 &&
        x.first.substr(x.first.size() - 11) == "start_ticks") {
      auto p = x.first.substr(0, x.first.size() - 11);

      auto rss = get_n(b.values, p + "rss_pages");
      if (rss && page) {
        r.rates[p + "rss_bytes"] = *rss * *page;
        if (total && *total > 0)
          r.rates[p + "rss_percent_visible_memory"] = 100.0 * (*rss * *page / *total);
      }
      auto start = number(x.second);
      if (start && hz && *hz > 0 && uptime && *uptime >= *start / *hz)
        r.rates[p + "age_seconds"] = *uptime - *start / *hz;

      if (!a || get(a->values, x.first, "") != x.second)
        continue;

      auto u = change(p + "user_ticks"), s = change(p + "system_ticks");
      if (u && s && hz && *hz > 0) {
        double pct = static_cast<double>((static_cast<long double>(*u) + *s) * 100.0L /
                                         *hz / elapsed);
        if (std::isfinite(pct) && (!logical || pct <= *logical * 100.0 + 100.0))
          r.rates[p + "cpu_percent_one_core"] = pct;
      }
      for (const auto *field : {"read_bytes", "write_bytes", "rchar", "wchar"}) {
        auto d = change(p + field);
        if (d)
          r.rates[p + field + "_per_second"] = static_cast<double>(*d) / elapsed;
      }
    }
  auto quota = get_n(b.values, "cgroup.visible_quota_cpus");
  if (quota)
    finding("cgroup.cpu_quota", "container/cgroup", "CONTEXT",
            "visible ancestor quota upper bound=" + fmt(*quota) + " CPU equivalents",
            "CPU quota limits runtime per quota period and may be shared by "
            "sibling workloads.",
            "This is not a dedicated CPU allocation. Hidden ancestors, cpusets, "
            "affinity and contention can constrain execution further.");

  auto throttled = r.rates.find("cgroup.cpu.stat.nr_throttled_delta");
  if (throttled != r.rates.end() && throttled->second > 0)
    finding("cgroup.cpu_throttling", "container/cgroup", "OBSERVED",
            "nr_throttled increased by " + fmt(throttled->second),
            "The cgroup CPU controller recorded throttled periods.",
            "Quota enforcement is distinct from thermal throttling. Counters cover "
            "the visible cgroup workload, not necessarily your application.");

  if (get_u(b.values, "cgroup.visible_memory_limit_bytes"))
    finding(
        "cgroup.memory_limit", "container/cgroup", "CONTEXT",
        "visible memory limit=" + get(b.values, "cgroup.visible_memory_limit_bytes") +
            " bytes; leaf current=" + get(b.values, "cgroup.level0.memory.current"),
        "Memory limits may differ from host-visible MemTotal and MemAvailable.",
        "Ancestor limits are shared. Leaf usage subtracted from an ancestor "
        "limit is not reliable available memory.");

  auto pc = get_u(b.values, "process.count");
  if (pc)
    finding("process.population", "process", "CONTEXT",
            "visible leaders=" + std::to_string(*pc) +
                "; zombies=" + get(b.values, "process.Z_count") +
                "; load entities=" + get(b.values, "load.entities"),
            "Process leaders and schedulable entities are different populations.",
            "There is no universal excessive process count. Baseline comparison "
            "and workload context are needed.");

  if (a) {
    auto before = get_u(a->values, "process.count");
    if (before && pc && *pc > *before)
      finding("process.population_growth", "process", "OBSERVED",
              "visible leader count increased by " + std::to_string(*pc - *before),
              "The visible process population grew between reads.",
              "Process churn, visibility and scan timing can change counts; "
              "growth alone does not imply a problem.");
  }
  bool thermal = false;
  for (const auto &x : b.values)
    if (x.first.rfind("thermal.", 0) == 0 || x.first.rfind("frequency.", 0) == 0)
      thermal = true;

  if (thermal)
    finding("thermal.context", "thermal", "CONTEXT",
            "Temperature and/or frequency readings are available in observations",
            "These are context readings from kernel interfaces.",
            "High temperature, frequency changes, or frequency below a maximum do "
            "not establish thermal throttling.");

  for (const auto &x : r.snapshot.unavailable)
    finding("unavailable." + x.first, "availability", "UNAVAILABLE",
            x.first + ": " + x.second, "The metric could not be used.",
            "Missing evidence is not evidence of absence.");

  bool indicator = false;
  for (const auto &f : r.findings)
    if (f.classification == "INDICATOR")
      indicator = true;

  if (!indicator)
    finding("evidence.no_indicator", "summary", "INSUFFICIENT EVIDENCE",
            "No selected attention heuristic matched the available observations",
            "No supported explanation of your reported slowdown was established.",
            "This is not a health verdict. Short windows, missing metrics, "
            "intermittent stalls, application locks and network latency can escape "
            "observation.");

  return r;
}
} // namespace ws
