// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_TIME_HPP
#define FABRIC_OBSERVATORY_TIME_HPP

#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#include "fabric_observatory/status.hpp"

namespace fabric_observatory {

// Nanoseconds since the Unix epoch, UTC. Signed because observation times can
// legitimately precede the epoch in synthetic and replay scenarios, and because
// signed arithmetic makes accidental wrap-around visible.
struct TimePoint {
  std::int64_t nanos{0};

  friend constexpr bool operator==(const TimePoint&, const TimePoint&) = default;
  friend constexpr auto operator<=>(const TimePoint&, const TimePoint&) = default;

  [[nodiscard]] constexpr bool is_set() const noexcept { return nanos != 0; }
};

struct Duration {
  std::int64_t nanos{0};

  friend constexpr bool operator==(const Duration&, const Duration&) = default;
  friend constexpr auto operator<=>(const Duration&, const Duration&) = default;

  [[nodiscard]] static constexpr Duration from_nanos(std::int64_t value) noexcept { return Duration{value}; }
  [[nodiscard]] static constexpr Duration from_micros(std::int64_t value) noexcept {
    return Duration{value * 1000};
  }
  [[nodiscard]] static constexpr Duration from_millis(std::int64_t value) noexcept {
    return Duration{value * 1000000};
  }
  [[nodiscard]] static constexpr Duration from_seconds(std::int64_t value) noexcept {
    return Duration{value * 1000000000};
  }
  [[nodiscard]] static constexpr Duration from_minutes(std::int64_t value) noexcept {
    return Duration{value * 60000000000};
  }

  [[nodiscard]] constexpr bool is_negative() const noexcept { return nanos < 0; }
};

[[nodiscard]] constexpr TimePoint operator+(TimePoint point, Duration delta) noexcept {
  return TimePoint{point.nanos + delta.nanos};
}

[[nodiscard]] constexpr TimePoint operator-(TimePoint point, Duration delta) noexcept {
  return TimePoint{point.nanos - delta.nanos};
}

[[nodiscard]] constexpr Duration operator-(TimePoint lhs, TimePoint rhs) noexcept {
  return Duration{lhs.nanos - rhs.nanos};
}

// True when lhs is strictly later than rhs.
[[nodiscard]] constexpr bool time_after(TimePoint lhs, TimePoint rhs) noexcept {
  return lhs.nanos > rhs.nanos;
}

// Difference that saturates instead of overflowing. Observation times are
// externally supplied, so the subtraction is done in unsigned arithmetic and
// clamped: a timestamp at the extreme of the representable range must be
// reported as "very old" or "very far in the future", never wrapped into a
// plausible looking small value.
[[nodiscard]] constexpr Duration saturating_difference(TimePoint lhs, TimePoint rhs) noexcept {
  if (lhs.nanos <= rhs.nanos) {
    return Duration{0};
  }
  const std::uint64_t delta =
      static_cast<std::uint64_t>(lhs.nanos) - static_cast<std::uint64_t>(rhs.nanos);
  constexpr std::uint64_t kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return Duration{delta > kMax ? std::numeric_limits<std::int64_t>::max()
                               : static_cast<std::int64_t>(delta)};
}

// Clock is injected. Tests and benchmarks drive time explicitly so that no test
// ever depends on a timeout or on wall clock progress.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  [[nodiscard]] virtual TimePoint now() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] TimePoint now() const override;
};

class ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(TimePoint start) : current_(start) {}

  [[nodiscard]] TimePoint now() const override { return current_; }
  void set(TimePoint value) noexcept { current_ = value; }
  void advance(Duration delta) noexcept { current_ = current_ + delta; }

 private:
  TimePoint current_{};
};

// Deterministic UTC formatting. No locale, no time zone database, no platform
// formatting routines: the output is a pure function of the input.
std::string format_utc(TimePoint point);
Result<TimePoint> parse_utc(std::string_view text);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_TIME_HPP
