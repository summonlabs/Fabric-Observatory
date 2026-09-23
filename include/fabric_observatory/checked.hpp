// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_CHECKED_HPP
#define FABRIC_OBSERVATORY_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

// Every size, offset, count and length that originates outside this runtime
// (wire payloads, journal headers, CLI arguments, persisted counters) passes
// through checked arithmetic before it is used to index, allocate or bound
// anything.

namespace fabric_observatory {

template <class T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr std::optional<T> checked_add(T lhs, T rhs) noexcept {
  if (rhs > static_cast<T>(std::numeric_limits<T>::max() - lhs)) {
    return std::nullopt;
  }
  return static_cast<T>(lhs + rhs);
}

template <class T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr std::optional<T> checked_sub(T lhs, T rhs) noexcept {
  if (rhs > lhs) {
    return std::nullopt;
  }
  return static_cast<T>(lhs - rhs);
}

template <class T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr std::optional<T> checked_mul(T lhs, T rhs) noexcept {
  if (lhs == 0 || rhs == 0) {
    return static_cast<T>(0);
  }
  if (lhs > static_cast<T>(std::numeric_limits<T>::max() / rhs)) {
    return std::nullopt;
  }
  return static_cast<T>(lhs * rhs);
}

// Checked narrowing to std::size_t. Returns nullopt when the value cannot be
// represented, which on 32-bit hosts is a real possibility for wire values.
[[nodiscard]] inline std::optional<std::size_t> checked_size(std::uint64_t value) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] inline std::optional<std::uint32_t> checked_u32(std::uint64_t value) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] inline std::optional<std::uint16_t> checked_u16(std::uint64_t value) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::uint16_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(value);
}

[[nodiscard]] inline std::optional<std::uint8_t> checked_u8(std::uint64_t value) noexcept {
  if (value > static_cast<std::uint64_t>(std::numeric_limits<std::uint8_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::uint8_t>(value);
}

// Checked conversion from a possibly negative signed source.
[[nodiscard]] inline std::optional<std::uint64_t> checked_from_i64(std::int64_t value) noexcept {
  if (value < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(value);
}

// Bounded accumulation used for aggregation windows: adds and saturates at the
// inclusive limit instead of wrapping, and reports whether saturation happened.
template <class T>
  requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool checked_accumulate(T& accumulator, T delta, T limit) noexcept {
  const std::optional<T> sum = checked_add(accumulator, delta);
  if (!sum.has_value() || *sum > limit) {
    accumulator = limit;
    return false;
  }
  accumulator = *sum;
  return true;
}

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_CHECKED_HPP
