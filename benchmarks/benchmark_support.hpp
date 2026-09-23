// Fabric Observatory benchmark support.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Benchmarks measure completed work: every benchmark counts the units of work it
// actually finished and reports the count with the elapsed time, and every
// result is folded into a checksum that is printed so that no measured
// computation can be optimised away.

#ifndef FABRIC_OBSERVATORY_BENCHMARK_SUPPORT_HPP
#define FABRIC_OBSERVATORY_BENCHMARK_SUPPORT_HPP

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace fabric_observatory::bench {

class Timer {
 public:
  Timer() : start_(std::chrono::steady_clock::now()) {}

  [[nodiscard]] std::uint64_t elapsed_nanos() const {
    const auto now = std::chrono::steady_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_).count());
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

inline void report(const char* name, const char* unit, std::uint64_t completed,
                   std::uint64_t elapsed_nanos, std::uint64_t checksum) {
  const double seconds = static_cast<double>(elapsed_nanos) / 1e9;
  const double per_second =
      elapsed_nanos == 0 ? 0.0 : static_cast<double>(completed) / seconds;
  std::printf("%-28s completed=%llu %-10s elapsed_ms=%.3f %s_per_second=%.0f checksum=%llu\n",
              name,
              static_cast<unsigned long long>(completed),
              unit,
              static_cast<double>(elapsed_nanos) / 1e6,
              unit,
              per_second,
              static_cast<unsigned long long>(checksum));
  std::fflush(stdout);
}

// Deterministic generator: the benchmarks are reproducible, and a failing run
// can be replayed exactly.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  [[nodiscard]] std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

 private:
  std::uint64_t state_;
};

}  // namespace fabric_observatory::bench

#endif  // FABRIC_OBSERVATORY_BENCHMARK_SUPPORT_HPP
