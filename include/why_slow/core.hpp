#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace ws {
using U = std::uint64_t;
using Values = std::map<std::string, std::string>;
struct Snapshot {
  double time = 0;
  Values values;
  Values unavailable;
  Values identity; // Private scope comparison; never formatted in reports.
};
struct Finding {
  std::string id, category, classification, evidence, explanation, limitations;
};
struct Report {
  Snapshot snapshot;
  std::map<std::string, double> rates;
  std::vector<Finding> findings;
};
struct Roots {
  std::string proc = "/proc", sys = "/sys";
  bool fixture = false;
};
std::optional<U> integer(const std::string &);
std::optional<double> number(const std::string &);
std::string safe(const std::string &);
std::vector<std::string> words(const std::string &);
std::optional<U> delta(U, U);
std::optional<U> get_u(const Values &, const std::string &);
std::optional<double> get_n(const Values &, const std::string &);
std::string get(const Values &, const std::string &,
                const std::string &fallback = "unavailable");
void stat_parse(const std::string &, Snapshot &);
void mem_parse(const std::string &, Snapshot &);
void vm_parse(const std::string &, Snapshot &);
void load_parse(const std::string &, Snapshot &);
void psi_parse(const std::string &, const std::string &, Snapshot &);
bool process_parse(const std::string &, U, Values &);
void io_parse(const std::string &, const std::string &, Values &);
void disk_parse(const std::string &, Snapshot &);
struct Mount {
  std::string device, root, path, type;
};
std::vector<Mount> mounts_parse(const std::string &);
std::optional<U> cpuset_count(const std::string &);
void quota_parse(const std::string &, const std::string &, Values &);
void temperature_parse(const std::string &, const std::string &, Values &);
void frequency_parse(const std::string &, const std::string &, Values &);
class Source {
public:
  virtual ~Source() = default;
  virtual std::optional<std::string> read(const std::string &) = 0;
  virtual std::vector<std::string> list(const std::string &) = 0;
  virtual bool capacity(const std::string &, Values &, const std::string &) = 0;
};
class Files final : public Source {
  bool fixture_;

public:
  explicit Files(bool fixture = false) : fixture_(fixture) {}
  std::optional<std::string> read(const std::string &) override;
  std::vector<std::string> list(const std::string &) override;
  bool capacity(const std::string &, Values &, const std::string &) override;
};
void cgroup_collect(Source &, const Roots &, const std::vector<Mount> &, Snapshot &);
void sys_collect(Source &, const Roots &, Snapshot &);
Snapshot collect(Source &, const Roots &, double);
Snapshot bounded_collect(const Roots &, double timeout_seconds,
                         Source *injected = nullptr);
double monotonic();
std::optional<double> next_sample_target(double start, double previous, double now,
                                         double interval, double duration,
                                         std::size_t collected, std::size_t limit);
std::string encode(const Snapshot &);
std::optional<Snapshot> decode(const std::string &);
Report analyze(const Snapshot &, const Snapshot *);
std::string json_string(const std::string &);
std::string format(const Report &, const std::string &, bool, std::size_t);
} // namespace ws
