#include "why_slow/core.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <poll.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
namespace ws {
// Fixture traversal rejects symlinks in every component, including directories.
static int open_input(const std::string &path, int flags, bool fixture) {
  if (!fixture)
    return open(path.c_str(), flags);
  std::filesystem::path p(path);
  int dir = open(p.is_absolute() ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0)
    return -1;
  std::vector<std::string> parts;
  for (const auto &part : p) {
    const auto name = part.string();
    if (name == "/" || name == "." || name.empty())
      continue;
    if (name == "..") {
      close(dir);
      return -1;
    }
    parts.push_back(name);
  }
  for (std::size_t i = 0; i < parts.size(); ++i) {
    int next =
        openat(dir, parts[i].c_str(),
               i + 1 == parts.size() ? flags
                                     : O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(dir);
    if (next < 0)
      return -1;
    dir = next;
  }
  return dir;
}
std::optional<std::string> Files::read(const std::string &path) {
  int fd = open_input(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, fixture_);

  if (fd < 0)
    return {};

  struct stat st{};

  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);

    return {};
  }
  std::string out;
  char b[8192];

  bool ok = true;

  for (;;) {
    auto n = ::read(fd, b, sizeof(b));

    if (n == 0)
      break;

    if (n < 0) {
      if (errno == EINTR)
        continue;
      ok = false;
      break;
    }
    if (out.size() + static_cast<std::size_t>(n) > 1048576) {
      ok = false;
      break;
    }
    out.append(b, static_cast<std::size_t>(n));
  }
  close(fd);

  return ok ? std::optional<std::string>(out) : std::nullopt;
}
std::vector<std::string> Files::list(const std::string &path) {
  std::vector<std::string> v;
  int fd = open_input(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, fixture_);
  if (fd < 0)
    return {};
  DIR *dir = fdopendir(fd);
  if (!dir) {
    close(fd);
    return v;
  }

  // A private DIR is used only by this single-threaded collector.
  // cppcheck-suppress readdirCalled
  while (const auto *e = readdir(dir)) {
    std::string name = e->d_name;

    if (name != "." && name != "..")
      v.push_back(name);

    if (v.size() >= 32768)
      break;
  }
  closedir(dir);
  std::sort(v.begin(), v.end());

  return v;
}
bool Files::capacity(const std::string &path, Values &v, const std::string &prefix) {
  struct statvfs s{};

  if (statvfs(path.c_str(), &s) != 0 || s.f_blocks == 0 || s.f_bfree > s.f_blocks ||
      s.f_bavail > s.f_blocks)
    return false;

  v[prefix + "blocks"] = std::to_string(s.f_blocks);
  v[prefix + "free_blocks"] = std::to_string(s.f_bfree);
  v[prefix + "available_blocks"] = std::to_string(s.f_bavail);
  v[prefix + "block_bytes"] = std::to_string(s.f_frsize);
  v[prefix + "files"] = std::to_string(s.f_files);
  v[prefix + "free_files"] = std::to_string(s.f_ffree);

  return true;
}
Snapshot collect(Source &src, const Roots &roots, double now) {
  Snapshot o;
  o.time = now;

  auto read = [&](const std::string &file, const std::string &key) {
    auto s = src.read(roots.proc + "/" + file);

    if (!s)
      o.unavailable[key] = "missing, inaccessible, non-regular, or over 1 MiB";

    return s.value_or("");
  };

  stat_parse(read("stat", "cpu"), o);
  load_parse(read("loadavg", "load"), o);
  mem_parse(read("meminfo", "memory"), o);
  vm_parse(read("vmstat", "swap.activity"), o);

  for (const auto *type : {"cpu", "memory", "io"})
    psi_parse(read(std::string("pressure/") + type, std::string("psi.") + type),
              std::string("psi.") + type, o);

  disk_parse(read("diskstats", "disk"), o);

  auto mounts = mounts_parse(read("self/mountinfo", "mounts"));

  std::set<std::string> devices;

  U index = 0, failed = 0, excluded = 0;

  const std::set<std::string> local = {"ext2",  "ext3",    "ext4", "xfs",   "btrfs",
                                       "f2fs",  "zfs",     "vfat", "exfat", "ntfs",
                                       "ntfs3", "overlay", "tmpfs"};

  for (const auto &m : mounts) {
    const auto later = std::find_if(mounts.rbegin(), mounts.rend(),
                                    [&](const Mount &x) { return x.path == m.path; });
    if (later != mounts.rend() && &*later != &m)
      continue;
    if (!local.count(m.type)) {
      ++excluded;
      continue;
    }
    if (!devices.insert(m.device).second)
      continue;

    auto p = "filesystem." + std::to_string(++index) + ".";
    o.values[p + "type"] = safe(m.type);

    // Fixture capacities are explicit data, never statvfs against host paths.
    if (roots.fixture) {
      auto s = src.read(roots.proc + "/fixture-capacity/" + std::to_string(index));

      auto w = words(s.value_or(""));

      if (w.size() == 6 && std::all_of(w.begin(), w.end(), [](const auto &x) {
            return integer(x).has_value();
          })) {
        const std::vector<std::string> fields = {
            "blocks",      "free_blocks", "available_blocks",
            "block_bytes", "files",       "free_files"};

        for (std::size_t j = 0; j < w.size(); ++j)
          o.values[p + fields[j]] = w[j];
      } else
        ++failed;
    } else if (!src.capacity(m.path, o.values, p))
      ++failed;
  }
  o.values["filesystem.count"] = std::to_string(index);
  o.values["filesystem.excluded_mounts"] = std::to_string(excluded);

  if (failed)
    o.unavailable["filesystem.capacity"] =
        std::to_string(failed) + " capacity reads unavailable";

  const long hz = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);

  if (hz > 0)
    o.values["system.tick_hz"] = std::to_string(hz);

  if (page > 0)
    o.values["system.page_bytes"] = std::to_string(page);

  auto uptime = words(read("uptime", "uptime"));

  if (!uptime.empty() && number(uptime[0]))
    o.values["system.uptime_seconds"] = uptime[0];

  U seen = 0, parsed = 0, io_missing = 0, unstable = 0, blocked = 0, zombie = 0;

  auto entries = src.list(roots.proc);

  if (entries.size() >= 32768)
    o.unavailable["process.scan"] =
        "directory entry cap reached; process list incomplete";

  for (const auto &name : entries) {
    auto pid = integer(name);

    if (!pid || *pid == 0)
      continue;
    ++seen;

    auto path = roots.proc + "/" + name;

    auto text = src.read(path + "/stat");
    Values pv;

    if (!text || !process_parse(*text, *pid, pv))
      continue;

    const auto p = "process." + name + ".";

    auto io = src.read(path + "/io");

    if (io)
      io_parse(*io, p, pv);

    if (!pv.count(p + "read_bytes"))
      ++io_missing;

    // Re-read identity after io to avoid attributing another lifetime's
    // counters.
    auto check = src.read(path + "/stat");
    Values verify;

    if (!check || !process_parse(*check, *pid, verify) ||
        get(pv, p + "start_ticks") != get(verify, p + "start_ticks")) {
      ++unstable;
      continue;
    }
    if (get(pv, p + "state") == "D")
      ++blocked;

    if (get(pv, p + "state") == "Z")
      ++zombie;

    o.values.insert(pv.begin(), pv.end());
    ++parsed;
  }
  o.values["process.count"] = std::to_string(parsed);
  o.values["process.seen"] = std::to_string(seen);
  o.values["process.D_count"] = std::to_string(blocked);
  o.values["process.Z_count"] = std::to_string(zombie);

  if (seen > parsed)
    o.unavailable["process.stat"] =
        std::to_string(seen - parsed) +
        " processes exited, denied, malformed, or changed identity";

  if (io_missing)
    o.unavailable["process.io"] =
        std::to_string(io_missing) + " process I/O records unavailable";

  if (unstable)
    o.values["process.unstable"] = std::to_string(unstable);

  if (!roots.fixture) {
    cpu_set_t mask;
    CPU_ZERO(&mask);

    if (sched_getaffinity(0, sizeof(mask), &mask) == 0)
      o.values["cpu.affinity_count"] = std::to_string(CPU_COUNT(&mask));
    else
      o.unavailable["cpu.affinity"] =
          "unavailable (including CPU sets exceeding fixed mask)";
  }
  cgroup_collect(src, roots, mounts, o);
  sys_collect(src, roots, o);

  o.values["scope"] = "procfs/namespace-visible; not necessarily physical host";

  return o;
}
std::string encode(const Snapshot &s) {
  std::ostringstream out;
  out << std::setprecision(17) << s.time << '\n';

  for (const auto &x : s.values)
    out << 'V' << ' ' << std::quoted(x.first) << ' ' << std::quoted(x.second) << '\n';

  for (const auto &x : s.identity)
    out << 'K' << ' ' << std::quoted(x.first) << ' ' << std::quoted(x.second) << '\n';
  for (const auto &x : s.unavailable)
    out << 'U' << ' ' << std::quoted(x.first) << ' ' << std::quoted(x.second) << '\n';

  return out.str();
}
std::optional<Snapshot> decode(const std::string &s) {
  Snapshot o;
  std::istringstream in(s);

  if (!(in >> o.time) || !std::isfinite(o.time) || o.time < 0)
    return {};
  char kind;

  std::string k, v;

  while (in >> kind) {
    if (!(in >> std::quoted(k) >> std::quoted(v)) || k.size() > 256 ||
        v.size() > 4096 || (kind != 'V' && kind != 'U' && kind != 'K'))
      return {};
    (kind == 'V' ? o.values : kind == 'U' ? o.unavailable : o.identity)[k] = v;
  }
  if (!in.eof())
    return {};

  return o;
}
Snapshot bounded_collect(const Roots &roots, double timeout_seconds, Source *injected) {
  Snapshot failed;
  failed.time = monotonic();
  failed.unavailable["collection"] = "collector failed or exceeded bounded deadline";

  int pipes[2];

  if (pipe2(pipes, O_CLOEXEC) != 0)
    return failed;

  pid_t child = fork();

  if (child < 0) {
    close(pipes[0]);
    close(pipes[1]);
    return failed;
  }
  if (child == 0) {
    close(pipes[0]);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    try {
      Files f(roots.fixture);

      auto s =
          collect(injected ? *injected : static_cast<Source &>(f), roots, monotonic());
      s.values["collection.seconds"] = std::to_string(monotonic() - s.time);

      auto bytes = encode(s);
      std::size_t at = 0;

      while (at < bytes.size()) {
        auto n = write(pipes[1], bytes.data() + at, bytes.size() - at);

        if (n < 0 && errno == EINTR)
          continue;

        if (n <= 0)
          _exit(1);
        at += static_cast<std::size_t>(n);
      }
      close(pipes[1]);
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  close(pipes[1]);
  int flags = fcntl(pipes[0], F_GETFL);

  if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    close(pipes[0]);
    kill(child, SIGKILL);
    waitpid(child, nullptr, WNOHANG);
    return failed;
  }

  std::string bytes;

  bool ended = false;
  const double deadline = monotonic() + timeout_seconds;

  while (monotonic() < deadline && bytes.size() <= 16777216) {
    pollfd p{pipes[0], POLLIN, 0};
    int ready = poll(&p, 1, 50);

    if (ready < 0 && errno != EINTR)
      break;
    char b[16384];

    auto n = ::read(pipes[0], b, sizeof(b));

    if (n > 0)
      bytes.append(b, static_cast<std::size_t>(n));
    else if (n == 0) {
      ended = true;
      break;
    } else if (errno != EAGAIN && errno != EINTR)
      break;
  }
  close(pipes[0]);
  int status = 0;
  pid_t reaped = waitpid(child, &status, WNOHANG);

  if (reaped == 0) { // Signals are sent ONLY to our own disposable collector,
                     // never analyzed processes.
    if (!ended)
      kill(child, SIGKILL);

    const double until = monotonic() + 0.2;

    while (monotonic() < until && (reaped = waitpid(child, &status, WNOHANG)) == 0)
      poll(nullptr, 0, 5);

    if (reaped == 0)
      kill(child, SIGKILL);
    // Do not wait indefinitely for an uninterruptible syscall.
  }
  if (!ended || bytes.size() > 16777216 || reaped != child || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0)
    return failed;

  auto result = decode(bytes);

  return result.value_or(failed);
}
} // namespace ws
