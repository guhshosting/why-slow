#include "why_slow/core.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
namespace ws {
std::string json_string(const std::string &s) {
  std::ostringstream o;
  o << '"';
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      o << "\\\"";
      break;
    case '\\':
      o << "\\\\";
      break;
    case '\n':
      o << "\\n";
      break;
    case '\r':
      o << "\\r";
      break;
    case '\t':
      o << "\\t";
      break;
    default:
      if (c < 32 || c >= 127)
        o << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
          << static_cast<unsigned>(c) << std::dec;
      else
        o << static_cast<char>(c);
    }
  }
  o << '"';
  return o.str();
}
static std::string markdown(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (std::string("\\`*_{}[]<>()#+-.!|").find(c) != std::string::npos)
      out += '\\';
    out += c;
  }
  return out;
}
std::string format(const Report &r, const std::string &type, bool explain,
                   std::size_t top) {
  std::ostringstream o;
  o.imbue(std::locale::classic());
  o << std::setprecision(12);

  if (type == "json") {
    o << "{\"monotonic_seconds\":" << r.snapshot.time << ",\"observations\":{";
    bool first = true;
    for (const auto &x : r.snapshot.values) {
      if (!first)
        o << ',';
      first = false;
      o << json_string(x.first) << ':' << json_string(x.second);
    }
    o << "},\"derived\":{";
    first = true;
    for (const auto &x : r.rates) {
      if (!first)
        o << ',';
      first = false;
      o << json_string(x.first) << ':' << x.second;
    }
    o << "},\"unavailable\":{";
    first = true;
    for (const auto &x : r.snapshot.unavailable) {
      if (!first)
        o << ',';
      first = false;
      o << json_string(x.first) << ':' << json_string(x.second);
    }
    o << "},\"findings\":[";
    first = true;

    for (const auto &f : r.findings) {
      if (!first)
        o << ',';
      first = false;
      o << "{\"id\":" << json_string(f.id)
        << ",\"category\":" << json_string(f.category)
        << ",\"classification\":" << json_string(f.classification)
        << ",\"evidence\":" << json_string(f.evidence)
        << ",\"explanation\":" << json_string(f.explanation)
        << ",\"limitations\":" << json_string(f.limitations) << '}';
    }
    o << "]}";
    return o.str();
  }
  bool md = type == "markdown";
  o << (md ? "## why-slow observation\n\n" : "why-slow observation\n");

  o << "Scope: procfs/namespace-visible. Correlation does not establish "
       "causation.\n\n";

  for (const auto &f : r.findings) {
    auto text = "[" + f.classification + "] " + f.id + ": " + f.evidence;
    o << (md ? "- " : "") << (md ? markdown(text) : safe(text)) << '\n';
    o << "  " << f.explanation << '\n';
    if (explain || f.classification != "UNAVAILABLE")
      o << "  Limit: " << f.limitations << '\n';
  }
  struct Row {
    std::string p;
    double rank;
  };
  std::vector<Row> rows;
  bool cpu_sort = false;
  for (const auto &x : r.rates)
    if (x.first.find("cpu_percent_one_core") != std::string::npos)
      cpu_sort = true;

  for (const auto &x : r.snapshot.values)
    if (x.first.rfind("process.", 0) == 0 && x.first.size() > 5 &&
        x.first.substr(x.first.size() - 5) == ".name") {
      auto p = x.first.substr(0, x.first.size() - 4);
      auto i = r.rates.find(p + (cpu_sort ? "cpu_percent_one_core" : "rss_bytes"));
      rows.push_back({p, i == r.rates.end() ? -1 : i->second});
    }
  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    return a.rank != b.rank ? a.rank > b.rank : a.p < b.p;
  });

  o << "\n"
    << (md ? "### " : "") << "Processes ranked by "
    << (cpu_sort ? "sampled CPU (100% = one logical CPU)" : "resident memory")
    << "\n\n";

  auto display = [&](const std::string &k) {
    auto i = r.rates.find(k);
    if (i == r.rates.end())
      return std::string("unavailable");
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2) << i->second;
    return ss.str();
  };

  for (std::size_t i = 0; i < std::min(top, rows.size()); ++i) {
    const auto &p = rows[i].p;
    std::string line = p + " " + get(r.snapshot.values, p + "name") +
                       " state=" + get(r.snapshot.values, p + "state") +
                       " CPU%=" + display(p + "cpu_percent_one_core") +
                       " RSS bytes=" + display(p + "rss_bytes");
    o << (md ? "- " : "") << (md ? markdown(line) : safe(line)) << '\n';
  }
  for (std::size_t i = 0; i < std::min(top, rows.size()); ++i) {
    const auto &p = rows[i].p;
    o << (md ? "- " : "") << p << "threads=" << get(r.snapshot.values, p + "threads")
      << " read bytes/s=" << display(p + "read_bytes_per_second")
      << " write bytes/s=" << display(p + "write_bytes_per_second") << '\n';
  }
  o << "Ranking identifies observed activity, not responsibility for system "
       "slowness.\n";

  if (explain) {
    o << "\n" << (md ? "### " : "") << "Raw observations and derived values\n\n";
    for (const auto &x : r.snapshot.values) {
      auto line = x.first + " = " + x.second;
      o << (md ? "- " : "") << (md ? markdown(line) : safe(line)) << '\n';
    }
    for (const auto &x : r.rates)
      o << (md ? "- " : "") << x.first << " = " << x.second << '\n';
  }
  return o.str();
}
} // namespace ws
