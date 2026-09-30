#include "why_slow/core.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
namespace ws {
double monotonic() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::optional<double> next_sample_target(double start, double previous, double now,
                                         double interval, double duration,
                                         std::size_t collected, std::size_t limit) {
  if (!std::isfinite(start) || !std::isfinite(previous) || !std::isfinite(now) ||
      !std::isfinite(interval) || !std::isfinite(duration) || interval <= 0 ||
      duration < 0 || collected >= 1000)
    return {};
  if (duration > 0) {
    if (previous >= start + duration && collected >= 2)
      return {};
    return std::min(start + duration, std::max(previous + interval, now));
  }
  if (collected >= limit)
    return {};
  return std::max(previous + interval, now);
}
} // namespace ws
