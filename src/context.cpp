#include "why_slow/core.hpp"
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <limits>
namespace ws {
static std::optional<std::string> scalar(Source &src, const std::string &path) {
  auto s = src.read(path);

  if (!s)
    return {};

  auto w = words(*s);

  if (w.size() != 1)
    return {};

  return w[0];
}
void sys_collect(Source &src, const Roots &r, Snapshot &o) {
  U temps = 0, freqs = 0;

  for (const auto &name : src.list(r.sys + "/class/thermal")) {
    if (name.rfind("thermal_zone", 0) != 0 || !integer(name.substr(12)))
      continue;

    auto s = src.read(r.sys + "/class/thermal/" + name + "/temp");

    if (!s)
      continue;

    auto key = "thermal." + name + ".celsius";
    temperature_parse(*s, key, o.values);

    if (o.values.count(key))
      ++temps;
  }
  for (const auto &name : src.list(r.sys + "/devices/system/cpu")) {
    if (name.rfind("cpu", 0) != 0 || !integer(name.substr(3)))
      continue;

    auto path = r.sys + "/devices/system/cpu/" + name;

    for (const auto *field :
         {"scaling_cur_freq", "scaling_max_freq", "cpuinfo_max_freq"}) {
      auto s = src.read(path + "/cpufreq/" + field);

      if (s) {
        auto key = "frequency." + name + "." + field + "_mhz";
        frequency_parse(*s, key, o.values);

        if (o.values.count(key))
          ++freqs;
      }
    }
    for (const auto *field : {"core_throttle_count", "package_throttle_count"}) {
      auto s = scalar(src, path + "/thermal_throttle/" + field);

      if (s && integer(*s))
        o.values["throttle." + name + "." + field] = *s;
    }
  }
  if (!temps)
    o.unavailable["thermal"] = "no plausible readable thermal-zone temperatures";

  if (!freqs)
    o.unavailable["frequency"] = "no plausible readable cpufreq values";

  auto hyper = scalar(src, r.sys + "/hypervisor/type");

  if (hyper)
    o.values["virtualization.hypervisor_interface"] = "present";

  auto cpuinfo = src.read(r.proc + "/cpuinfo");

  if (cpuinfo && cpuinfo->find(" hypervisor") != std::string::npos)
    o.values["virtualization.cpu_hypervisor_flag"] = "present";

  auto status = src.read(r.proc + "/self/status");

  if (status) {
    std::istringstream in(*status);
    std::string line;
    while (std::getline(in, line)) {
      auto v = words(line);
      if (v.size() > 2 && v[0] == "NSpid:")
        o.values["container.nested_pid_namespace"] = "observed";
    }
  }
  o.values["virtualization.detection"] =
      "heuristic; absence does not establish bare metal";
}
void cgroup_collect(Source &src, const Roots &r, const std::vector<Mount> &mounts,
                    Snapshot &o) {
  auto s = src.read(r.proc + "/self/cgroup");

  if (!s) {
    o.unavailable["cgroup"] = "membership unavailable";
    return;
  }
  std::istringstream in(*s);

  std::string line, member;

  bool v1 = false;

  while (std::getline(in, line)) {
    if (line.rfind("0::", 0) == 0)
      member = line.substr(3);
    else if (!line.empty())
      v1 = true;
  }
  if (member.empty()) {
    o.values["cgroup.mode"] = v1 ? "v1 (not interpreted)" : "undetected";
    o.unavailable["cgroup.v2"] = "no unified membership";
    return;
  }
  o.values["cgroup.mode"] = v1 ? "hybrid" : "v2";

  // Never resolve '..' from a cgroup namespace outside the visible hierarchy.
  auto safe_path = [](const std::string &p) {
    if (p.empty() || p.size() > 2048 || p[0] != '/' ||
        p.find('\0') != std::string::npos)
      return false;

    const std::filesystem::path parsed(p);
    return std::none_of(parsed.begin(), parsed.end(), [](const auto& part) {return part == "..";});
  };

  if (!safe_path(member)) {
    o.unavailable["cgroup.v2"] = "membership cannot be mapped safely";
    return;
  }
  const Mount *chosen = nullptr;

  std::string relative;

  for (const auto &m : mounts) {
    if (m.type != "cgroup2" || !safe_path(m.root) || !safe_path(m.path))
      continue;

    if (m.root == "/") {
      chosen = &m;
      relative = member;
      break;
    }
    if (member == m.root || member.rfind(m.root + "/", 0) == 0) {
      chosen = &m;
      relative = member.substr(m.root.size());
      break;
    }
  }
  if (!chosen) {
    o.unavailable["cgroup.v2"] = "no matching visible unified mount";
    return;
  }
  std::string base =
      std::filesystem::path(r.fixture ? r.sys + "/fs/cgroup" : chosen->path)
          .lexically_normal()
          .string();
  while (base.size() > 1 && base.back() == '/')
    base.pop_back();

  std::filesystem::path path = std::filesystem::path(base);
  const auto first_component = relative.find_first_not_of('/');
  if (first_component != std::string::npos)
    path /= relative.substr(first_component);
  path = path.lexically_normal();
  o.identity["cgroup"] = path.string();

  U level = 0;
  std::optional<double> effective;
  std::optional<U> memory_limit;

  for (;;) {
    auto prefix = "cgroup.level" + std::to_string(level) + ".";

    auto cpu = src.read(path.string() + "/cpu.max");

    if (cpu)
      quota_parse(*cpu, prefix, o.values);

    auto q = get_n(o.values, prefix + "quota_cpus");

    if (q)
      effective = effective ? std::min(*q, *effective) : *q;

    for (const auto *file :
         {"memory.max", "memory.current", "memory.high", "pids.current", "pids.max"}) {
      auto value = scalar(src, path.string() + "/" + file);

      if (value && (integer(*value) || (*value == "max" && std::string(file).find("current") == std::string::npos))) {
        o.values[prefix + file] = *value;
        if (std::string(file) == "memory.max" && integer(*value))
          memory_limit = memory_limit ? std::min(*memory_limit, *integer(*value))
                                      : integer(*value);
      }
    }
    if (level == 0) {
      auto cpuset = src.read(path.string() + "/cpuset.cpus.effective");
      if (cpuset) {
        auto n = cpuset_count(*cpuset);
        if (n)
          o.values["cgroup.cpuset_count"] = std::to_string(*n);
      }
      for (const auto *res : {"cpu", "memory", "io"}) {
        auto pressure = src.read(path.string() + "/" + res + ".pressure");
        psi_parse(pressure.value_or(""), std::string("cgroup.psi.") + res, o);
      }
      for (const auto *file : {"cpu.stat", "memory.events"}) {
        auto text = src.read(path.string() + "/" + file);
        if (text) {
          std::istringstream input(*text);
          std::string row;
          while (std::getline(input, row)) {
            auto v = words(row);
            if (v.size() == 2 && integer(v[1]) &&
                (v[0] == "usage_usec" || v[0] == "nr_throttled" ||
                 v[0] == "throttled_usec" || v[0] == "high" || v[0] == "oom" ||
                 v[0] == "oom_kill"))
              o.values[std::string("cgroup.") + file + "." + v[0]] = v[1];
          }
        }
      }
      auto io = src.read(path.string() + "/io.max");
      if (io)
        o.values["cgroup.io_constraints"] =
            words(*io).empty() ? "none at visible leaf"
                               : "configured; per-device limits not interpreted";
    }
    if (path == std::filesystem::path(base) || ++level >= 64)
      break;

    auto parent = path.parent_path();

    if (parent == path || parent.string().size() < base.size())
      break;
    path = parent;
  }
  if (effective) {
    std::ostringstream cpus;
    cpus << std::setprecision(std::numeric_limits<double>::max_digits10) << *effective;
    o.values["cgroup.visible_quota_cpus"] = cpus.str();
  }

  if (memory_limit)
    o.values["cgroup.visible_memory_limit_bytes"] = std::to_string(*memory_limit);

  if (!o.values.count("cgroup.level0.memory.current") &&
      !o.values.count("cgroup.level0.quota"))
    o.unavailable["cgroup.controllers"] = "CPU and memory controls unavailable";

  o.values["cgroup.scope"] = "visible ancestors only; hidden ancestors may "
                             "impose tighter shared limits";
}
} // namespace ws
