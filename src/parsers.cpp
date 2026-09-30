#include "why_slow/core.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
namespace ws {
std::optional<U> integer(const std::string &s) {
  U n = 0;

  if (s.empty())
    return {};

  auto r = std::from_chars(s.data(), s.data() + s.size(), n);

  if (r.ec != std::errc{} || r.ptr != s.data() + s.size())
    return {};

  return n;
}
std::optional<double> number(const std::string &s) {
  if (s.empty() || s.size() > 64)
    return {};

  double n = 0;
  std::istringstream in(s);
  in.imbue(std::locale::classic());
  in >> n;

  if (!in || !in.eof() || !std::isfinite(n) || n < 0)
    return {};

  return n;
}
std::string safe(const std::string &s) {
  std::string out;

  for (unsigned char c : s) {
    if (out.size() >= 256)
      break;
    out += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
  }
  return out;
}
std::vector<std::string> words(const std::string &s) {
  std::istringstream in(s);
  std::vector<std::string> v;

  std::string x;

  while (in >> x) {
    if (v.size() >= 65536)
      break;
    v.push_back(x);
  }
  return v;
}
std::optional<U> delta(U a, U b) {
  if (b < a)
    return {};

  return b - a;
}
std::optional<U> get_u(const Values &v, const std::string &k) {
  auto i = v.find(k);

  return i == v.end() ? std::nullopt : integer(i->second);
}
std::optional<double> get_n(const Values &v, const std::string &k) {
  auto i = v.find(k);

  return i == v.end() ? std::nullopt : number(i->second);
}
std::string get(const Values &v, const std::string &k, const std::string &fallback) {
  auto i = v.find(k);

  return i == v.end() ? fallback : i->second;
}
static const std::vector<std::string> cpu_fields = {
    "user", "nice",    "system", "idle",  "iowait",
    "irq",  "softirq", "steal",  "guest", "guest_nice"};

void stat_parse(const std::string &s, Snapshot &o) {
  std::istringstream in(s);

  std::string line;

  U count = 0;

  while (std::getline(in, line)) {
    auto v = words(line);

    if (v.empty())
      continue;

    if (v[0] == "cpu" ||
        (v[0].size() > 3 && v[0].substr(0, 3) == "cpu" && integer(v[0].substr(3)))) {
      if (v.size() < 5)
        continue;

      bool valid = true;

      for (std::size_t j = 1; j < v.size(); ++j)
        if (!integer(v[j]))
          valid = false;

      if (!valid)
        continue;

      std::string p = v[0] == "cpu" ? "cpu.total." : "cpu." + v[0].substr(3) + ".";
      if (o.values.count(p + "user")) {
        o.unavailable[p + "duplicate"] = "duplicate CPU record ignored";
        continue;
      }

      for (std::size_t j = 0; j < cpu_fields.size(); ++j)
        o.values[p + cpu_fields[j]] = j + 1 < v.size() ? v[j + 1] : "0";

      if (v[0] != "cpu")
        ++count;

    } else if (v.size() == 2 && (v[0] == "procs_running" || v[0] == "procs_blocked") &&
               integer(v[1]))
      o.values["tasks." + v[0]] = v[1];
  }
  if (count)
    o.values["cpu.logical"] = std::to_string(count);

  if (!o.values.count("cpu.total.user"))
    o.unavailable["cpu"] = "missing or malformed CPU counters";
}
void mem_parse(const std::string &s, Snapshot &o) {
  const std::set<std::string> wanted = {
      "MemTotal:", "MemFree:",      "MemAvailable:", "Buffers:",
      "Cached:",   "SReclaimable:", "Shmem:",        "SwapTotal:",
      "SwapFree:", "Dirty:",        "Writeback:"};

  std::istringstream in(s);

  std::string line;

  while (std::getline(in, line)) {
    auto v = words(line);

    if (v.size() != 3 || !wanted.count(v[0]) || v[2] != "kB")
      continue;

    auto n = integer(v[1]);

    if (n && *n <= std::numeric_limits<U>::max() / 1024)
      o.values["memory." + v[0].substr(0, v[0].size() - 1) + "_bytes"] =
          std::to_string(*n * 1024);
  }
  auto total = get_u(o.values, "memory.MemTotal_bytes"),
       available = get_u(o.values, "memory.MemAvailable_bytes");

  if (!total || *total == 0)
    o.unavailable["memory"] = "missing or invalid MemTotal";

  if (!available || (total && *available > *total)) {
    o.values.erase("memory.MemAvailable_bytes");
    o.unavailable["memory.available"] = "MemAvailable missing or inconsistent";
  }
  auto st = get_u(o.values, "memory.SwapTotal_bytes"),
       sf = get_u(o.values, "memory.SwapFree_bytes");

  if (st && sf && *sf <= *st)
    o.values["memory.SwapUsed_bytes"] = std::to_string(*st - *sf);
}
void vm_parse(const std::string &s, Snapshot &o) {
  std::istringstream in(s);

  std::string line;

  while (std::getline(in, line)) {
    auto v = words(line);

    if (v.size() == 2 && integer(v[1]) &&
        (v[0] == "pswpin" || v[0] == "pswpout" || v[0] == "pgmajfault" ||
         v[0] == "pgscan_kswapd" || v[0] == "pgscan_direct"))
      o.values["vm." + v[0]] = v[1];
  }
  if (!o.values.count("vm.pswpin") || !o.values.count("vm.pswpout"))
    o.unavailable["swap.activity"] = "swap counters missing or malformed";
}
void load_parse(const std::string &s, Snapshot &o) {
  auto v = words(s);

  if (v.size() < 4 || !number(v[0]) || !number(v[1]) || !number(v[2])) {
    o.unavailable["load"] = "missing or malformed load";
    return;
  }
  const auto slash = v[3].find('/');

  if (slash == std::string::npos) {
    o.unavailable["load"] = "invalid task counts";
    return;
  }
  auto a = integer(v[3].substr(0, slash)), b = integer(v[3].substr(slash + 1));

  if (!a || !b || *a > *b) {
    o.unavailable["load"] = "invalid task counts";
    return;
  }
  o.values["load.1"] = v[0];
  o.values["load.5"] = v[1];
  o.values["load.15"] = v[2];
  o.values["load.runnable"] = std::to_string(*a);
  o.values["load.entities"] = std::to_string(*b);
}
void psi_parse(const std::string &s, const std::string &p, Snapshot &o) {
  std::istringstream in(s);

  std::string line;

  bool found = false;

  while (std::getline(in, line)) {
    auto v = words(line);

    if (v.empty() || (v[0] != "some" && v[0] != "full"))
      continue;
    std::set<std::string> seen;
    Values temp;

    bool valid = true;

    for (std::size_t i = 1; i < v.size(); ++i) {
      auto pos = v[i].find('=');

      if (pos == std::string::npos) {
        valid = false;
        break;
      }
      auto k = v[i].substr(0, pos), value = v[i].substr(pos + 1);

      if (!seen.insert(k).second)
        valid = false;

      if (k == "total") {
        if (!integer(value))
          valid = false;
        else
          temp[p + "." + v[0] + ".total_us"] = value;
      } else if (k == "avg10" || k == "avg60" || k == "avg300") {
        auto n = number(value);

        if (!n || *n > 100)
          valid = false;
        else
          temp[p + "." + v[0] + "." + k] = value;
      }
    }
    if (valid && seen.count("total") && seen.count("avg10") && seen.count("avg60") &&
        seen.count("avg300")) {
      o.values.insert(temp.begin(), temp.end());
      found = true;
    } else
      o.unavailable[p + "." + v[0]] = "malformed or partial PSI line";
  }
  if (!found)
    o.unavailable[p] = "PSI missing or malformed";
}
bool process_parse(const std::string &s, U pid, Values &out) {
  auto left = s.find('('), right = s.rfind(')');

  if (left == std::string::npos || right == std::string::npos || right <= left)
    return false;

  auto head = words(s.substr(0, left));

  if (head.size() != 1 || integer(head[0]) != std::optional<U>(pid))
    return false;

  auto v = words(s.substr(right + 1));

  if (v.size() < 22 || v[0].size() != 1 ||
      std::string("RSDZTWtXxKPI").find(v[0]) == std::string::npos)
    return false;

  for (auto i : {11, 12, 17, 19})
    if (!integer(v.at(i)))
      return false;

  auto rss = integer(v[21]);

  if (!rss)
    return false;

  const auto p = "process." + std::to_string(pid) + ".";

  out[p + "name"] = safe(s.substr(left + 1, right - left - 1));
  out[p + "state"] = v[0];
  out[p + "user_ticks"] = v[11];
  out[p + "system_ticks"] = v[12];
  out[p + "threads"] = v[17];
  out[p + "start_ticks"] = v[19];
  out[p + "rss_pages"] = v[21];

  return true;
}
void io_parse(const std::string &s, const std::string &p, Values &out) {
  std::istringstream in(s);

  std::string line;
  const std::set<std::string> keys = {
      "read_bytes:", "write_bytes:", "rchar:", "wchar:"};

  while (std::getline(in, line)) {
    auto v = words(line);

    if (v.size() == 2 && keys.count(v[0]) && integer(v[1]))
      out[p + v[0].substr(0, v[0].size() - 1)] = v[1];
  }
}
void disk_parse(const std::string &s, Snapshot &o) {
  std::istringstream in(s);

  std::string line;

  U count = 0;

  while (std::getline(in, line)) {
    auto v = words(line);

    if (v.size() < 14 || !integer(v[0]) || !integer(v[1]))
      continue;

    bool ok = true;

    for (std::size_t i = 3; i < v.size(); ++i)
      if (!integer(v[i]))
        ok = false;

    if (!ok)
      continue;

    auto p = "disk." + v[0] + ":" + v[1] + ".";
    o.values[p + "name"] = safe(v[2]);
    o.values[p + "read_sectors"] = v[5];
    o.values[p + "write_sectors"] = v[9];
    o.values[p + "io_ms"] = v[12];
    ++count;
  }
  o.values["disk.count"] = std::to_string(count);
}
static std::string unescape(const std::string &s) {
  std::string o;

  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7' &&
        s[i + 2] >= '0' && s[i + 2] <= '7' && s[i + 3] >= '0' && s[i + 3] <= '7') {
      const int n = (s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + s[i + 3] - '0';

      if (n == 0 || n > 255)
        return {};
      o += static_cast<char>(n);
      i += 3;
    } else
      o += s[i];
  }
  return o;
}
std::vector<Mount> mounts_parse(const std::string &s) {
  std::vector<Mount> out;
  std::istringstream in(s);

  std::string line;

  while (std::getline(in, line) && out.size() < 4096) {
    auto v = words(line);

    auto sep = std::find(v.begin(), v.end(), "-");

    if (v.size() < 10 || sep == v.end()) continue;
    const auto separator = static_cast<std::size_t>(sep - v.begin());
    if (separator < 6 || v.size() - separator < 4) continue;

    auto root = unescape(v[3]), path = unescape(v[4]);

    if (root.empty() || path.empty() || root[0] != '/' || path[0] != '/' ||
        root.find('\0') != std::string::npos || path.find('\0') != std::string::npos)
      continue;
    out.push_back({v[2], root, path, v.at(separator + 1)});
  }
  return out;
}
std::optional<U> cpuset_count(const std::string &input) {
  auto tokens = words(input);

  if (tokens.size() != 1)
    return {};
  const auto &s = tokens[0];

  U total = 0, previous = 0;

  bool first = true;
  std::size_t start = 0;

  while (start < s.size()) {
    auto end = s.find(',', start);

    auto item = s.substr(start, end == std::string::npos ? end : end - start);

    auto dash = item.find('-');

    auto low = integer(item.substr(0, dash)),
         high = dash == std::string::npos ? low : integer(item.substr(dash + 1));

    if (!low || !high || *high < *low || *high > 1048575 ||
        (!first && *low <= previous))
      return {};
    total += *high - *low + 1;
    previous = *high;
    first = false;

    if (end == std::string::npos)
      break;
    start = end + 1;

    if (start == s.size())
      return {};
  }
  return total ? std::optional<U>(total) : std::nullopt;
}
void quota_parse(const std::string &s, const std::string &p, Values &v) {
  auto w = words(s);

  if (w.size() != 2)
    return;

  auto period = integer(w[1]);

  if (!period || *period == 0)
    return;

  if (w[0] == "max") {
    v[p + "quota"] = "max";
    v[p + "period_us"] = w[1];
    return;
  }
  auto quota = integer(w[0]);

  if (!quota || *quota == 0)
    return;
  v[p + "quota"] = w[0];
  v[p + "period_us"] = w[1];
  std::ostringstream cpus;
  cpus << std::setprecision(std::numeric_limits<double>::max_digits10)
       << static_cast<double>(*quota) / static_cast<double>(*period);
  v[p + "quota_cpus"] = cpus.str();
}
void temperature_parse(const std::string &s, const std::string &p, Values &v) {
  auto w = words(s);

  if (w.size() != 1)
    return;
  std::int64_t n = 0;

  auto r = std::from_chars(w[0].data(), w[0].data() + w[0].size(), n);

  if (r.ec != std::errc{} || r.ptr != w[0].data() + w[0].size() || n < -40000 ||
      n > 150000)
    return;
  v[p] = std::to_string(static_cast<double>(n) / 1000.0);
}
void frequency_parse(const std::string &s, const std::string &p, Values &v) {
  auto w = words(s);

  if (w.size() != 1)
    return;

  auto n = integer(w[0]);

  if (n && *n >= 1000 && *n <= 10000000)
    v[p] = std::to_string(static_cast<double>(*n) / 1000.0);
}
} // namespace ws
