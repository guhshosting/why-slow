#include "why_slow/core.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>
namespace {
volatile sig_atomic_t interrupted = 0;

void stop(int s) { interrupted = s; }
struct Options {
  std::string mode = "snapshot", format = "text";
  ws::Roots roots;
  double interval = 1, duration = 0, timeout = 5;
  std::size_t samples = 3, top = 10;
  bool explain = false, count_set = false;
};

void help() {
  std::cout << "why-slow 1.0.0 - read-only Linux performance observations\n"
               "Usage: why-slow [snapshot|sample|processes|summary] [options]\n"
               "  --format text|json|markdown  Output format (default text)\n"
               "  --interval SECONDS          0.05..60, default 1\n"
               "  --samples N                 Total readings, 2..1000 (sample "
               "default 3)\n"
               "  --duration SECONDS          0.05..3600; alternative to --samples\n"
               "  --timeout SECONDS           Per-collection deadline, 0.1..30 "
               "(default 5)\n"
               "  --top N                     Text process rows, 1..100 (default "
               "10)\n"
               "  --explain                   Include raw observations in "
               "text/Markdown\n"
               "  --fixture-root DIR          DIR/proc and DIR/sys; no host "
               "statvfs/affinity\n"
               "  --help | --version\n"
               "Snapshot has no rates. Processes/summary default to two readings.\n"
               "JSON is one document with a samples array; counters are decimal "
               "strings.\n"
               "No report files are created. Exit: 0 report, 1 collection failure/no "
               "core data, 2 "
               "usage,\n"
               "3 output/internal error, 130 interrupt, 143 termination, 141 broken "
               "pipe.\n";
}
Options parse(int argc, char **argv) {
  Options o;
  bool mode_set = false, interval_set = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "snapshot" || a == "sample" || a == "processes" || a == "summary") {
      if (mode_set)
        throw std::invalid_argument("only one mode is allowed");
      o.mode = a;
      mode_set = true;
      continue;
    }
    if (a == "--explain") {
      o.explain = true;
      continue;
    }
    if (i + 1 >= argc)
      throw std::invalid_argument("option needs a value (or unknown option)");

    std::string v = argv[++i];
    auto n = ws::number(v);
    auto u = ws::integer(v);

    if (a == "--format") {
      if (v != "text" && v != "json" && v != "markdown")
        throw std::invalid_argument("unsupported format");
      o.format = v;
    } else if (a == "--interval" && n && *n >= 0.05 && *n <= 60) {
      o.interval = *n;
      interval_set = true;
    } else if (a == "--duration" && n && *n >= 0.05 && *n <= 3600)
      o.duration = *n;

    else if (a == "--timeout" && n && *n >= 0.1 && *n <= 30)
      o.timeout = *n;

    else if (a == "--samples" && u && *u >= 2 && *u <= 1000) {
      o.samples = static_cast<std::size_t>(*u);
      o.count_set = true;
    } else if (a == "--top" && u && *u >= 1 && *u <= 100)
      o.top = static_cast<std::size_t>(*u);

    else if (a == "--fixture-root" && !v.empty()) {
      auto root = std::filesystem::absolute(v).lexically_normal().string();
      o.roots.proc = root + "/proc";
      o.roots.sys = root + "/sys";
      o.roots.fixture = true;
    } else
      throw std::invalid_argument("unknown option or invalid value");
  }
  if (o.duration > 0 && std::ceil(o.duration / o.interval) + 1 > 1000)
    throw std::invalid_argument("duration/interval requires more than 1000 readings");
  if (o.duration > 0 && o.count_set)
    throw std::invalid_argument("--duration and --samples are alternatives");

  if (o.mode == "snapshot" && (o.duration > 0 || o.count_set || interval_set))
    throw std::invalid_argument(
        "sampling options require sample, processes, or summary mode");

  if (o.mode == "snapshot")
    o.samples = 1;
  else if (!o.count_set && o.duration == 0 && o.mode != "sample")
    o.samples = 2;

  if (o.duration == 0 && o.interval * static_cast<double>(o.samples - 1) > 3600)
    throw std::invalid_argument("sampling span exceeds one hour");

  return o;
}
} // namespace
int main(int argc, char **argv) {
  std::locale::global(std::locale::classic());

  if (argc == 2 && std::string(argv[1]) == "--help") {
    help();
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "why-slow 1.0.0\n";
    return 0;
  }
  Options o;
  try {
    o = parse(argc, argv);
  } catch (const std::exception &e) {
    std::cerr << "why-slow: " << e.what() << "; use --help\n";
    return 2;
  }
  struct sigaction action{};
  action.sa_handler = stop;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);

  signal(SIGPIPE, SIG_IGN);

  try {
    bool first = true, any = false, collection_failed = false;
    ws::Snapshot previous;
    std::size_t count = 0;
    const double start = ws::monotonic();
    double target = start;

    if (o.format == "json")
      std::cout << "{\"schema_version\":1,\"version\":\"1.0.0\",\"mode\":"
                << ws::json_string(o.mode) << ",\"samples\":[";

    while (!interrupted && count < 1000) {
      while (!interrupted && ws::monotonic() < target) {
        double remaining = target - ws::monotonic();
        poll(nullptr, 0,
             std::max(1, std::min(100, static_cast<int>(remaining * 1000))));
      }
      if (interrupted)
        break;

      auto snap = ws::bounded_collect(o.roots, o.timeout);
      auto report = ws::analyze(snap, first ? nullptr : &previous);

      if (snap.values.count("cpu.total.user") ||
          ws::get_u(snap.values, "memory.MemTotal_bytes").value_or(0) > 0 ||
          ws::get_u(snap.values, "process.count").value_or(0) > 0)
        any = true;

      if (!first)
        std::cout << (o.format == "json" ? "," : "\n");

      std::cout << ws::format(report, o.format, o.explain, o.top);
      std::cout.flush();
      if (!std::cout)
        return errno == EPIPE ? 141 : 3;

      collection_failed = snap.unavailable.count("collection") != 0;
      previous = std::move(snap);
      first = false;
      ++count;
      if (collection_failed)
        break;

      auto next = ws::next_sample_target(start, target, ws::monotonic(), o.interval,
                                         o.duration, count, o.samples);
      if (!next)
        break;
      target = *next;
    }
    if (o.format == "json")
      std::cout << "],\"interrupted\":" << (interrupted ? "true" : "false") << "}\n";

    std::cout.flush();
    if (!std::cout)
      return errno == EPIPE ? 141 : 3;

    if (interrupted)
      return 128 + interrupted;

    return any && !collection_failed ? 0 : 1;

  } catch (const std::exception &) {
    std::cerr << "why-slow: internal collection/formatting failure\n";
    return 3;
  }
}
