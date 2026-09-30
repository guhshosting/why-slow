#include "why_slow/core.hpp"
#include <cmath>
#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
using namespace ws;
namespace {
void check(bool yes) {
  if (!yes)
    throw std::runtime_error("assertion failed");
}
void near(double a, double b) { check(std::abs(a - b) < 0.001); }
bool has(const Report &r, const std::string &id, const std::string &cls = "") {
  return std::any_of(r.findings.begin(), r.findings.end(), [&](const Finding& f) {return f.id == id && (cls.empty() || f.classification == cls);});
}
Snapshot base(double time = 1) {
  Snapshot s;
  s.time = time;
  stat_parse("cpu 100 0 20 880 0 0 0 0 0 0\ncpu0 100 0 20 880 0 0 0 "
             "0\nprocs_running 1\nprocs_blocked 0\n",
             s);
  mem_parse("MemTotal: 1000 kB\nMemFree: 1 kB\nMemAvailable: 500 kB\nCached: 450 "
            "kB\nSwapTotal: 200 kB\nSwapFree: 100 kB\nDirty: 2 kB\nWriteback: 1 kB\n",
            s);
  vm_parse("pswpin 10\npswpout 10\n", s);
  s.values["system.tick_hz"] = "100";
  s.values["system.page_bytes"] = "4096";
  s.values["process.count"] = "1";
  s.values["process.D_count"] = "0";
  return s;
}
std::string proc(const std::string &name = "test", const std::string &state = "R",
                 U start = 10, U user = 100) {
  std::vector<std::string> v(22, "0");
  v[0] = state;
  v[11] = std::to_string(user);
  v[12] = "50";
  v[17] = "2";
  v[19] = std::to_string(start);
  v[21] = "20";
  std::string s = "42 (" + name + ")";
  for (const auto &x : v)
    s += " " + x;
  return s;
}
class Fake final : public Source {
public:
  Values files;
  std::map<std::string, std::vector<std::string>> dirs;
  std::optional<std::string> read(const std::string &p) override {
    auto i = files.find(p);
    return i == files.end() ? std::nullopt : std::optional<std::string>(i->second);
  }
  std::vector<std::string> list(const std::string &p) override { return dirs[p]; }
  bool capacity(const std::string &, Values &out, const std::string &p) override {
    out[p + "blocks"] = "100";
    out[p + "free_blocks"] = "10";
    out[p + "available_blocks"] = "5";
    return true;
  }
};
} // namespace
int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests;
  auto test = [&](const std::string &n, std::function<void()> fn) {
    tests.emplace_back(n, fn);
  };
  test("integer bounds", [] {
    check(integer("18446744073709551615") == std::numeric_limits<U>::max());
    check(!integer("18446744073709551616"));
  });
  test("integer rejects signs junk", [] {
    for (const auto *s : {"-1", "+1", "1x", "", " 1", "1 "})
      check(!integer(s));
  });
  test("number rejects nonfinite", [] {
    for (const auto *s : {"nan", "inf", "-1", "1e999", "2x"})
      check(!number(s));
  });
  test("safe terminal bytes", [] {
    check(safe("a\033\n\xFF") == "a???");
    check(safe(std::string(1000, 'x')).size() == 256);
  });
  test("delta reset", [] {
    check(!delta(100, 99));
    check(delta(100, 100) == 0);
  });
  test("stat fields guest not added", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.user"] = "200";
    b.values["cpu.total.guest"] = "100";
    auto r = analyze(b, &a);
    near(r.rates.at("cpu.total.busy_percent"), 100);
  });
  test("stat multicore", [] {
    Snapshot s;
    stat_parse("cpu 1 2 3 4\ncpu0 1 2 3 4\ncpu1 0 0 0 0\n", s);
    check(get_u(s.values, "cpu.logical") == 2);
  });
  test("stat malformed", [] {
    Snapshot s;
    stat_parse("cpu a 1 2 3\ncpu0 1 2\n", s);
    check(s.unavailable.count("cpu") == 1);
  });
  test("cpu no snapshot rate", [] {
    auto r = analyze(base(), nullptr);
    check(!r.rates.count("cpu.total.busy_percent"));
    check(has(r, "cpu.no_rate", "INSUFFICIENT EVIDENCE"));
  });
  test("cpu zero elapsed", [] {
    auto a = base(), b = a;
    b.values["cpu.total.user"] = "200";
    check(!analyze(b, &a).rates.count("cpu.total.busy_percent"));
  });
  test("cpu zero ticks", [] {
    auto a = base(), b = base(2);
    check(!analyze(b, &a).rates.count("cpu.total.busy_percent"));
  });
  test("cpu counter reset", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.user"] = "99";
    check(!analyze(b, &a).rates.count("cpu.total.busy_percent"));
  });
  test("iowait regression invalidates", [] {
    auto a = base(), b = base(2);
    a.values["cpu.total.iowait"] = "10";
    check(!analyze(b, &a).rates.count("cpu.total.busy_percent"));
  });
  test("cpu huge counters safe sum", [] {
    auto a = base(), b = base(2);
    for (auto &x : b.values)
      if (x.first.rfind("cpu.total.", 0) == 0)
        x.second = "18446744073709551615";
    auto r = analyze(b, &a);
    check(r.rates.at("cpu.total.busy_percent") <= 100);
  });
  test("cpu user system idle percentages", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.user"] = "125";
    b.values["cpu.total.system"] = "45";
    b.values["cpu.total.idle"] = "930";
    auto r = analyze(b, &a);
    near(r.rates.at("cpu.total.busy_percent"), 50);
  });
  test("load valid", [] {
    Snapshot s;
    load_parse("2.5 1.5 0.5 3/100 456", s);
    check(get(s.values, "load.1") == "2.5");
    check(get_u(s.values, "load.entities") == 100);
  });
  test("load malformed", [] {
    for (const auto *text : {"x 2 3 1/2", "1 2 3 3/2", "1 2", "1 2 3 x/y"}) {
      Snapshot s;
      load_parse(text, s);
      check(s.unavailable.count("load"));
    }
  });
  test("load context no cpu blame", [] {
    auto b = base();
    load_parse("100 50 20 1/100", b);
    auto r = analyze(b, nullptr);
    check(has(r, "load.context", "CONTEXT"));
  });
  test("mem available cache", [] {
    auto s = base();
    check(get_u(s.values, "memory.Cached_bytes") == 460800);
    near(analyze(s, nullptr).rates.at("memory.available_percent"), 50);
  });
  test("mem units overflow", [] {
    Snapshot s;
    mem_parse("MemTotal: 18446744073709551615 kB\nMemAvailable: 100 MB\n", s);
    check(s.values.empty());
  });
  test("mem unavailable exceeds total", [] {
    Snapshot s;
    mem_parse("MemTotal: 1 kB\nMemAvailable: 2 kB\n", s);
    check(!s.values.count("memory.MemAvailable_bytes"));
  });
  test("mem absent available", [] {
    Snapshot s;
    mem_parse("MemTotal: 100 kB\nMemFree: 1 kB\n", s);
    check(!analyze(s, nullptr).rates.count("memory.available_percent"));
  });
  test("mem dirty writeback", [] {
    auto s = base();
    check(get_u(s.values, "memory.Dirty_bytes") == 2048);
    check(get_u(s.values, "memory.Writeback_bytes") == 1024);
  });
  test("swap occupancy",
       [] { check(get_u(base().values, "memory.SwapUsed_bytes") == 102400); });
  test("swap inconsistent", [] {
    Snapshot s;
    mem_parse("SwapTotal: 1 kB\nSwapFree: 2 kB\n", s);
    check(!s.values.count("memory.SwapUsed_bytes"));
  });
  test("swap active rate delayed clock", [] {
    auto a = base(), b = base(3);
    b.values["vm.pswpin"] = "30";
    near(analyze(b, &a).rates.at("vm.pswpin_per_second"), 10);
  });
  test("swap reset", [] {
    auto a = base(), b = base(2);
    b.values["vm.pswpin"] = "0";
    check(!analyze(b, &a).rates.count("vm.pswpin_per_second"));
  });
  test("vm malformed", [] {
    Snapshot s;
    vm_parse("pswpin nope\npswpout -1\npgmajfault 7\n", s);
    check(s.values.size() == 1);
  });
  test("psi valid all types", [] {
    for (const auto *kind : {"cpu", "memory", "io"}) {
      Snapshot s;
      psi_parse("some avg10=1.25 avg60=0.2 avg300=0.0 total=500000\nfull "
                "avg10=0 avg60=0 avg300=0 total=0\n",
                kind, s);
      check(s.values.size() == 8);
    }
  });
  test("psi missing", [] {
    Snapshot s;
    psi_parse("", "psi.cpu", s);
    check(s.unavailable.count("psi.cpu"));
  });
  test("psi partial rejected", [] {
    Snapshot s;
    psi_parse("some avg10=1 total=2", "psi.cpu", s);
    check(s.values.empty());
  });
  test("psi malformed percent", [] {
    Snapshot s;
    psi_parse("some avg10=101 avg60=0 avg300=0 total=2", "p", s);
    check(s.values.empty());
  });
  test("psi huge exact", [] {
    Snapshot s;
    psi_parse("some avg10=0 avg60=0 avg300=0 total=18446744073709551615", "p", s);
    check(get_u(s.values, "p.some.total_us") == std::numeric_limits<U>::max());
  });
  test("psi duplicate rejected", [] {
    Snapshot s;
    psi_parse("some avg10=0 avg60=0 avg300=0 total=2 total=3", "p", s);
    check(s.values.empty());
  });
  test("psi sampled percentage", [] {
    auto a = base(), b = base(3);
    a.values["psi.memory.some.total_us"] = "0";
    b.values["psi.memory.some.total_us"] = "1000000";
    near(analyze(b, &a).rates.at("psi.memory.some.interval_percent"), 50);
  });
  test("psi impossible delta unavailable", [] {
    auto a = base(), b = base(2);
    a.values["psi.io.some.total_us"] = "0";
    b.values["psi.io.some.total_us"] = "2000000";
    check(!analyze(b, &a).rates.count("psi.io.some.interval_percent"));
  });
  test("mount escaped path", [] {
    auto m = mounts_parse("1 0 8:1 / /hello\\040world rw - ext4 /dev/x rw\n");
    check(m.size() == 1);
    check(m[0].path == "/hello world");
  });
  test("mount malformed", [] {
    check(mounts_parse("1 2 3 broken\n").empty());
    check(mounts_parse("1 0 8:1 / /a\\000 rw - ext4 x rw").empty());
  });
  test("filesystem duplicate pseudo filtering", [] {
    Fake f;
    f.files["/p/self/mountinfo"] =
        "1 0 8:1 / /a rw - ext4 x rw\n2 0 8:1 /dir /b rw - ext4 x rw\n3 0 0:1 "
        "/ /proc rw - proc proc rw\n";
    auto s = collect(f, {"/p", "/s", false}, 1);
    check(get_u(s.values, "filesystem.count") == 1);
    check(get_u(s.values, "filesystem.excluded_mounts") == 1);
  });
  test("filesystem fixture capacity", [] {
    Fake f;
    f.files["/p/self/mountinfo"] = "1 0 0:2 / / rw - overlay overlay rw\n";
    f.files["/p/fixture-capacity/1"] = "100 10 5 4096 100 1";
    auto s = collect(f, {"/p", "/s", true}, 1);
    check(has(analyze(s, nullptr), "storage.capacity.filesystem.1."));
  });
  test("disk parsing sectors", [] {
    Snapshot s;
    disk_parse("8 0 sda 10 0 20 30 40 0 50 60 0 70 80\n", s);
    check(get_u(s.values, "disk.8:0.read_sectors") == 20);
    check(get_u(s.values, "disk.8:0.write_sectors") == 50);
  });
  test("disk invalid counters", [] {
    Snapshot s;
    disk_parse("8 0 sda 10 0 -1 30 40 0 50 60 0 70 80", s);
    check(get_u(s.values, "disk.count") == 0);
  });
  test("disk stacked no sum", [] {
    auto a = base(), b = base(2);
    for (const auto *k : {"disk.8:0.read_sectors", "disk.253:0.read_sectors"}) {
      a.values[k] = "0";
      b.values[k] = "100";
    }
    auto r = analyze(b, &a);
    check(r.rates.count("disk.8:0.read_sectors_per_second"));
    check(!r.rates.count("disk.total"));
  });
  test("disk reset no rate", [] {
    auto a = base(), b = base(2);
    a.values["disk.8:0.read_sectors"] = "100";
    b.values["disk.8:0.read_sectors"] = "0";
    check(!analyze(b, &a).rates.count("disk.8:0.read_sectors_per_second"));
  });
  test("process normal fields", [] {
    Values v;
    check(process_parse(proc(), 42, v));
    check(get_u(v, "process.42.threads") == 2);
    check(get_u(v, "process.42.rss_pages") == 20);
  });
  test("process parentheses spaces", [] {
    Values v;
    check(process_parse(proc("a (b) ) c"), 42, v));
    check(get(v, "process.42.name") == "a (b) ) c");
  });
  test("process control characters", [] {
    Values v;
    check(process_parse(proc("a\033\nb"), 42, v));
    check(get(v, "process.42.name") == "a??b");
  });
  test("process malformed and mismatch", [] {
    Values v;
    check(!process_parse("42 (foo) R 1", 42, v));
    check(!process_parse(proc(), 43, v));
    check(!process_parse(proc("bad", "?"), 42, v));
  });
  test("process CPU multicore units", [] {
    auto a = base(), b = base(2);
    b.values["cpu.logical"] = "4";
    process_parse(proc(), 42, a.values);
    process_parse(proc("test", "R", 10, 300), 42, b.values);
    near(analyze(b, &a).rates.at("process.42.cpu_percent_one_core"), 200);
  });
  test("process PID reuse no rate", [] {
    auto a = base(), b = base(2);
    process_parse(proc(), 42, a.values);
    process_parse(proc("new", "R", 20, 300), 42, b.values);
    check(!analyze(b, &a).rates.count("process.42.cpu_percent_one_core"));
  });
  test("process new no rate", [] {
    auto a = base(), b = base(2);
    process_parse(proc(), 42, b.values);
    check(!analyze(b, &a).rates.count("process.42.cpu_percent_one_core"));
  });
  test("process exited no rate", [] {
    auto a = base(), b = base(2);
    process_parse(proc(), 42, a.values);
    check(!analyze(b, &a).rates.count("process.42.cpu_percent_one_core"));
  });
  test("process zombie preserved", [] {
    Values v;
    check(process_parse(proc("dead", "Z"), 42, v));
    check(get(v, "process.42.state") == "Z");
  });
  test("process memory and age", [] {
    auto a = base();
    process_parse(proc(), 42, a.values);
    a.values["system.uptime_seconds"] = "10";
    auto r = analyze(a, nullptr);
    near(r.rates.at("process.42.rss_bytes"), 81920);
    near(r.rates.at("process.42.age_seconds"), 9.9);
  });
  test("process IO bytes and requests", [] {
    Values v;
    io_parse("read_bytes: 123\nrchar: 456\nwrite_bytes: -1\n", "p.", v);
    check(v.size() == 2);
    check(get_u(v, "p.rchar") == 456);
  });
  test("process denied via injected source", [] {
    Fake f;
    f.dirs["/p"] = {"42"};
    auto s = collect(f, {"/p", "/s", true}, 1);
    check(get_u(s.values, "process.count") == 0);
    check(s.unavailable.count("process.stat"));
  });
  test("cpuset ranges", [] { check(cpuset_count("0-3,8,10-11\n") == 7); });
  test("cpuset malformed overlap huge", [] {
    for (const auto *s :
         {"", "1-0", "0-3,2", "1,", "999999999", "0-2,2-3", "a", "0,,2"})
      check(!cpuset_count(s));
  });
  test("quota fractional", [] {
    Values v;
    quota_parse("50000 100000", "c.", v);
    near(*get_n(v, "c.quota_cpus"), 0.5);
  });
  test("quota max", [] {
    Values v;
    quota_parse("max 100000", "c.", v);
    check(get(v, "c.quota") == "max");
    check(!v.count("c.quota_cpus"));
  });
  test("quota malformed", [] {
    for (const auto *s : {"1 0", "0 100", "-1 100", "max x", "5 10 extra"}) {
      Values v;
      quota_parse(s, "c.", v);
      check(v.empty());
    }
  });
  test("cgroup visible ancestors", [] {
    Fake f;
    f.files["/p/self/cgroup"] = "0::/leaf\n";
    f.files["/s/fs/cgroup/leaf/cpu.max"] = "100000 100000";
    f.files["/s/fs/cgroup/cpu.max"] = "50000 100000";
    f.files["/s/fs/cgroup/leaf/memory.max"] = "max";
    f.files["/s/fs/cgroup/memory.max"] = "1000";
    Snapshot s;
    cgroup_collect(f, {"/p", "/s", true}, {{"0:1", "/", "/sys/fs/cgroup", "cgroup2"}},
                   s);
    near(*get_n(s.values, "cgroup.visible_quota_cpus"), 0.5);
    check(get_u(s.values, "cgroup.visible_memory_limit_bytes") == 1000);
  });
  test("cgroup unsafe membership", [] {
    Fake f;
    f.files["/p/self/cgroup"] = "0::/../../x";
    Snapshot s;
    cgroup_collect(f, {"/p", "/s", true}, {}, s);
    check(s.unavailable.count("cgroup.v2"));
  });
  test("cgroup v1 explicit", [] {
    Fake f;
    f.files["/p/self/cgroup"] = "2:cpu:/test\n";
    Snapshot s;
    cgroup_collect(f, {"/p", "/s", true}, {}, s);
    check(get(s.values, "cgroup.mode") == "v1 (not interpreted)");
  });
  test("cgroup missing controllers", [] {
    Fake f;
    f.files["/p/self/cgroup"] = "0::/\n";
    Snapshot s;
    cgroup_collect(f, {"/p", "/s", true}, {{"0:1", "/", "/sys/fs/cgroup", "cgroup2"}},
                   s);
    check(s.unavailable.count("cgroup.controllers"));
  });
  test("thermal units signed", [] {
    Values v;
    temperature_parse("65000\n", "t", v);
    check(get(v, "t") == "65.000000");
    temperature_parse("-5000", "t", v);
    check(get(v, "t") == "-5.000000");
  });
  test("thermal rejects implausible", [] {
    for (const auto *s :
         {"151000", "-41000", "nan", "18446744073709551615", "123junk"}) {
      Values v;
      temperature_parse(s, "t", v);
      check(v.empty());
    }
  });
  test("frequency units", [] {
    Values v;
    frequency_parse("2400000", "f", v);
    near(*get_n(v, "f"), 2400);
  });
  test("frequency malformed", [] {
    for (const auto *s : {"0", "10000001", "x", "-1"}) {
      Values v;
      frequency_parse(s, "f", v);
      check(v.empty());
    }
  });
  test("wire roundtrip", [] {
    auto s = base();
    s.values["name"] = "a\"b\\c\n";
    auto d = decode(encode(s));
    check(d && d->values == s.values);
  });
  test("wire malformed", [] {
    check(!decode("bad"));
    check(!decode("1\nQ \"a\" \"b\""));
  });
  test("JSON escapes",
       [] { check(json_string("\"\\\n\001") == "\"\\\"\\\\\\n\\u0001\""); });
  test("CPU saturated scenario", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.user"] = "200";
    auto r = analyze(b, &a);
    check(has(r, "cpu.utilization", "INDICATOR"));
  });
  test("high load blocked low CPU scenario", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.idle"] = "980";
    b.values["process.D_count"] = "5";
    load_parse("20 10 5 1/100", b);
    check(has(analyze(b, &a), "load.blocked_context", "INDICATOR"));
  });
  test("low MemFree healthy availability no pressure", [] {
    auto r = analyze(base(), nullptr);
    check(has(r, "memory.available", "CONTEXT"));
    check(has(r, "evidence.no_indicator"));
  });
  test("occupied inactive swap no active finding", [] {
    auto a = base(), b = base(2);
    auto r = analyze(b, &a);
    check(has(r, "swap.occupied", "CONTEXT"));
    check(!has(r, "swap.activity"));
  });
  test("active swap with memory pressure", [] {
    auto a = base(), b = base(2);
    b.values["vm.pswpin"] = "20";
    a.values["psi.memory.some.total_us"] = "0";
    b.values["psi.memory.some.total_us"] = "500000";
    check(has(analyze(b, &a), "memory.swap_pressure", "INDICATOR"));
  });
  test("high IO without pressure", [] {
    auto a = base(), b = base(2);
    a.values["disk.8:0.read_sectors"] = "0";
    b.values["disk.8:0.read_sectors"] = "10000000";
    check(has(analyze(b, &a), "evidence.no_indicator"));
  });
  test("elevated IO PSI", [] {
    auto a = base(), b = base(2);
    a.values["psi.io.some.total_us"] = "0";
    b.values["psi.io.some.total_us"] = "500000";
    check(has(analyze(b, &a), "pressure.psi.io.some.interval_percent", "INDICATOR"));
  });
  test("full filesystem only attention", [] {
    auto b = base();
    b.values["filesystem.1.blocks"] = "100";
    b.values["filesystem.1.free_blocks"] = "5";
    b.values["filesystem.1.available_blocks"] = "5";
    check(has(analyze(b, nullptr), "storage.capacity.filesystem.1.", "INDICATOR"));
  });
  test("cgroup quota context", [] {
    auto b = base();
    b.values["cgroup.visible_quota_cpus"] = "0.5";
    check(has(analyze(b, nullptr), "cgroup.cpu_quota", "CONTEXT"));
  });
  test("multiple indicators", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.user"] = "200";
    b.values["memory.MemAvailable_bytes"] = "1";
    auto r = analyze(b, &a);
    check(has(r, "cpu.utilization", "INDICATOR"));
    check(has(r, "memory.available", "INDICATOR"));
  });
  test("no score no health verdict", [] {
    auto text = format(analyze(base(), nullptr), "json", true, 10);
    check(text.find("\"score\"") == std::string::npos);
    check(text.find("healthy") == std::string::npos);
  });
  test("thermal throttle requires counter", [] {
    auto b = base();
    b.values["thermal.zone.celsius"] = "100";
    b.values["frequency.cpu0.scaling_cur_freq_mhz"] = "200";
    check(!has(analyze(b, nullptr), "thermal.throttle_counter"));
  });
  test("thermal actual counter increment", [] {
    auto a = base(), b = base(2);
    a.values["throttle.cpu0.core_throttle_count"] = "2";
    b.values["throttle.cpu0.core_throttle_count"] = "3";
    check(has(analyze(b, &a),
              "thermal.throttle_counter.throttle.cpu0.core_throttle_count",
              "OBSERVED"));
  });
  test("causality prohibited positive claims", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.user"] = "200";
    b.values["process.D_count"] = "5";
    b.values["thermal.zone.celsius"] = "100";
    b.values["memory.MemAvailable_bytes"] = "1";
    load_parse("99 88 77 1/100", b);
    process_parse(proc(), 42, b.values);
    auto text = format(analyze(b, &a), "json", true, 10);
    for (const auto *bad :
         {"CPU usage is causing", "slow because it is swapping", "You need more RAM",
          "proves disk bottleneck", "temperature proves throttling",
          "process caused system slowness", "filesystem caused performance degradation",
          "high load means CPU bottleneck"})
      check(text.find(bad) == std::string::npos);
  });
  test("CPU high without pressure no contention claim", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.user"] = "200";
    auto r = analyze(b, &a);
    for (const auto &f : r.findings)
      check(f.id.find("pressure.") != 0);
  });
  test("iowait not busy", [] {
    auto a = base(), b = base(2);
    b.values["cpu.total.iowait"] = "100";
    auto r = analyze(b, &a);
    near(r.rates.at("cpu.total.busy_percent"), 0);
    check(has(r, "cpu.utilization", "OBSERVED"));
  });

  test("cpu huge idle tiny busy nonnegative", [] {
    auto a = base(), b = base(2);
    for (auto &x : a.values)
      if (x.first.rfind("cpu.total.", 0) == 0)
        x.second = "0";
    for (auto &x : b.values)
      if (x.first.rfind("cpu.total.", 0) == 0)
        x.second = "0";
    b.values["cpu.total.user"] = "1";
    for (const auto *key : {"idle", "iowait", "steal"})
      b.values[std::string("cpu.total.") + key] = "18446744073709551615";
    auto r = analyze(b, &a);
    check(r.rates.at("cpu.total.busy_percent") >= 0);
    check(r.rates.at("cpu.total.busy_percent") <= 100);
  });
  test("cpu hotplug aggregate unavailable", [] {
    auto a = base(), b = base(2);
    b.values["cpu.logical"] = "2";
    b.values["cpu.total.user"] = "200";
    check(!analyze(b, &a).rates.count("cpu.total.busy_percent"));
  });
  test("cgroup scope changes invalidate rates", [] {
    auto a = base(), b = base(2);
    a.identity["cgroup"] = "/a";
    b.identity["cgroup"] = "/b";
    a.values["cgroup.cpu.stat.nr_throttled"] = "1";
    b.values["cgroup.cpu.stat.nr_throttled"] = "10";
    check(!analyze(b, &a).rates.count("cgroup.cpu.stat.nr_throttled_delta"));
  });
  test("private scope never formatted", [] {
    auto s = base();
    s.identity["cgroup"] = "/private/person";
    check(decode(encode(s))->identity == s.identity);
    check(format(analyze(s, nullptr), "json", true, 10).find("private/person") ==
          std::string::npos);
  });
  test("cgroup repeated slash remains below root", [] {
    Fake f;
    f.files["/p/self/cgroup"] = "0:://leaf";
    f.files["/s/fs/cgroup/leaf/cpu.max"] = "50000 100000";
    Snapshot s;
    cgroup_collect(f, {"/p", "/s", true}, {{"0:1", "/", "/sys/fs/cgroup", "cgroup2"}},
                   s);
    near(*get_n(s.values, "cgroup.visible_quota_cpus"), 0.5);
  });
  test("cgroup root not read twice", [] {
    Fake f;
    f.files["/p/self/cgroup"] = "0::/";
    f.files["/s/fs/cgroup/cpu.max"] = "50000 100000";
    Snapshot s;
    cgroup_collect(f, {"/p", "/s", true}, {{"0:1", "/", "/sys/fs/cgroup", "cgroup2"}},
                   s);
    check(!s.values.count("cgroup.level1.quota"));
  });
  test("overmounted filesystem last entry wins", [] {
    Fake f;
    f.files["/p/self/mountinfo"] =
        "1 0 8:1 / /a rw - ext4 x rw\n2 0 8:2 / /a rw - xfs x rw\n";
    auto s = collect(f, {"/p", "/s", false}, 1);
    check(get_u(s.values, "filesystem.count") == 1);
    check(get(s.values, "filesystem.1.type") == "xfs");
  });
  test("system CPU full PSI no attention", [] {
    auto a = base(), b = base(2);
    a.values["psi.cpu.full.total_us"] = "0";
    b.values["psi.cpu.full.total_us"] = "500000";
    check(!has(analyze(b, &a), "pressure.psi.cpu.full.interval_percent"));
  });
  test("negative elapsed no rates", [] {
    auto a = base(2), b = base(1);
    b.values["vm.pswpin"] = "100";
    check(!analyze(b, &a).rates.count("vm.pswpin_per_second"));
  });
  test("io active process rate no blame", [] {
    auto a = base(), b = base(2);
    process_parse(proc(), 42, a.values);
    process_parse(proc(), 42, b.values);
    a.values["process.42.read_bytes"] = "0";
    b.values["process.42.read_bytes"] = "1000000";
    auto r = analyze(b, &a);
    near(r.rates.at("process.42.read_bytes_per_second"), 1000000);
    check(has(r, "evidence.no_indicator"));
  });
  test("schedule normal fake clock",
       [] { near(*next_sample_target(10, 10, 10.1, 1, 0, 1, 3), 11); });
  test("schedule delayed fake clock",
       [] { near(*next_sample_target(10, 10, 15, 1, 0, 1, 3), 15); });
  test("schedule count completed",
       [] { check(!next_sample_target(10, 12, 12.1, 1, 0, 3, 3)); });
  test("schedule duration final reading", [] {
    near(*next_sample_target(10, 10, 15, 1, 2, 1, 3), 12);
    check(!next_sample_target(10, 12, 15, 1, 2, 2, 3));
  });
  test("schedule bounds invalid time", [] {
    check(!next_sample_target(0, 0, 0, 0, 0, 1, 3));
    check(!next_sample_target(0, 0, 0, 1, 0, 1000, 1001));
  });
  test("collector hard deadline injected slow source", [] {
    class Slow final : public Source {
    public:
      std::optional<std::string> read(const std::string &) override {
        poll(nullptr, 0, 2000);
        return {};
      }
      std::vector<std::string> list(const std::string &) override { return {}; }
      bool capacity(const std::string &, Values &, const std::string &) override {
        return false;
      }
    } slow;
    double before = monotonic();
    auto s = bounded_collect({"/p", "/s", true}, 0.1, &slow);
    check(s.unavailable.count("collection"));
    check(monotonic() - before < 1.0);
  });
  test("cgroup current rejects max", [] {
    Fake f; f.files["/p/self/cgroup"]="0::/";
    f.files["/s/fs/cgroup/memory.current"]="max";
    f.files["/s/fs/cgroup/pids.current"]="max";
    Snapshot s; cgroup_collect(f,{"/p","/s",true},{{"0:1","/","/sys/fs/cgroup","cgroup2"}},s);
    check(!s.values.count("cgroup.level0.memory.current"));
    check(!s.values.count("cgroup.level0.pids.current"));
  });
  test("repeated collectors release resources", [] {
    Fake f; f.files["/p/stat"]="cpu 1 2 3 4\ncpu0 1 2 3 4\n";
    for (unsigned i=0;i<20;++i) {
      auto s=bounded_collect({"/p","/s",true},1,&f);
      check(!s.unavailable.count("collection"));
      check(s.values.count("cpu.total.user"));
    }
  });
  test("duplicate CPU records do not inflate count", [] {
    Snapshot s;stat_parse("cpu 1 2 3 4\ncpu0 1 2 3 4\ncpu0 9 9 9 9\n",s);
    check(get_u(s.values,"cpu.logical")==1);check(get_u(s.values,"cpu.0.user")==1);
    check(s.unavailable.count("cpu.0.duplicate"));
  });
  test("mount embedded NUL rejected", [] {
    std::string text="1 0 8:1 / /a";text.push_back('\0');text+="b rw - ext4 x rw";
    check(mounts_parse(text).empty());
  });
  test("partial swap counters unavailable", [] {
    Snapshot s;vm_parse("pswpin 12\n",s);check(s.unavailable.count("swap.activity"));
  });
  test("huge quota period does not round capacity to zero", [] {
    Fake f;f.files["/p/self/cgroup"]="0::/";
    f.files["/s/fs/cgroup/cpu.max"]="1 18446744073709551615";
    Snapshot s;cgroup_collect(f,{"/p","/s",true},{{"0:1","/","/sys/fs/cgroup","cgroup2"}},s);
    auto q=get_n(s.values,"cgroup.visible_quota_cpus");check(q && *q>0 && *q<1e-18);
  });
  unsigned failed = 0;
  for (const auto &t : tests) {
    try {
      t.second();
      std::cout << "PASS " << t.first << '\n';
    } catch (const std::exception &e) {
      ++failed;
      std::cerr << "FAIL " << t.first << ": " << e.what() << '\n';
    }
  }
  std::cout << tests.size() << " unit cases; " << failed << " failures\n";
  return failed ? 1 : 0;
}
