// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_VALUE_HPP
#define FABRIC_OBSERVATORY_VALUE_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fabric_observatory/canonical.hpp"
#include "fabric_observatory/status.hpp"

namespace fabric_observatory {

// Bounds applied to every value that enters the runtime from outside. The
// limits are enforced at construction time, so a Value that exists is already
// bounded: there is no unbounded tree to walk later.
struct ValueLimits {
  std::size_t max_depth{8};
  std::size_t max_list_elements{1024};
  std::size_t max_map_entries{256};
  std::size_t max_text_bytes{4096};
  std::size_t max_blob_bytes{4096};
  std::size_t max_total_nodes{4096};

  friend bool operator==(const ValueLimits&, const ValueLimits&) = default;
};

// A canonical, ordered, bounded value. Comparison is total, so values group
// into conflict sets without depending on map iteration order or pointer
// identity.
class Value {
 public:
  enum class Kind : std::uint8_t {
    Null = 0,
    Bool = 1,
    Int = 2,
    Uint = 3,
    Real = 4,
    Text = 5,
    Blob = 6,
    List = 7,
    Map = 8,
  };

  Value() = default;

  [[nodiscard]] static Value null() noexcept { return Value{}; }
  [[nodiscard]] static Value boolean(bool value) noexcept;
  [[nodiscard]] static Value integer(std::int64_t value) noexcept;
  [[nodiscard]] static Value unsigned_integer(std::uint64_t value) noexcept;
  [[nodiscard]] static Result<Value> real(double value);
  [[nodiscard]] static Result<Value> text(std::string value, const ValueLimits& limits);
  [[nodiscard]] static Result<Value> blob(std::vector<std::byte> value, const ValueLimits& limits);
  [[nodiscard]] static Result<Value> list(std::vector<Value> elements, const ValueLimits& limits);
  [[nodiscard]] static Result<Value> map(std::vector<std::pair<std::string, Value>> entries,
                                         const ValueLimits& limits);

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] std::size_t depth() const noexcept { return depth_; }
  [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }

  [[nodiscard]] bool as_bool() const noexcept { return bool_; }
  [[nodiscard]] std::int64_t as_int() const noexcept { return int_; }
  [[nodiscard]] std::uint64_t as_uint() const noexcept { return uint_; }
  [[nodiscard]] double as_real() const noexcept { return real_; }
  [[nodiscard]] const std::string& as_text() const noexcept { return text_; }
  [[nodiscard]] const std::vector<std::byte>& as_blob() const noexcept { return blob_; }
  [[nodiscard]] const std::vector<Value>& elements() const noexcept { return list_; }
  [[nodiscard]] const std::vector<std::pair<std::string, Value>>& entries() const noexcept {
    return map_;
  }

  // Unambiguous, injective text used for conflict grouping, explanations and
  // diagnostics. Never used as a substitute for the canonical binary encoding.
  [[nodiscard]] std::string canonical_text() const;
  // Human facing rendering; lossy for reals, never used for identity.
  [[nodiscard]] std::string display_text() const;

  void encode(CanonicalEncoder& encoder) const;
  // Re-checks this value against a (possibly tighter) limits set. A value built
  // under one policy must not be accepted unchanged under a stricter one.
  [[nodiscard]] Result<void> validate(const ValueLimits& limits) const;
  // Exact inverse of encode(). Bounds are re-checked on the way in, so a
  // corrupted or hostile encoding cannot produce an over-large value.
  [[nodiscard]] static Result<Value> decode(CanonicalDecoder& decoder, const ValueLimits& limits);

  friend bool operator==(const Value&, const Value&) = default;
  friend std::strong_ordering operator<=>(const Value& lhs, const Value& rhs);

 private:
  Kind kind_{Kind::Null};
  bool bool_{false};
  std::int64_t int_{0};
  std::uint64_t uint_{0};
  double real_{0.0};
  std::string text_{};
  std::vector<std::byte> blob_{};
  std::vector<Value> list_{};
  std::vector<std::pair<std::string, Value>> map_{};
  std::size_t depth_{1};
  std::size_t node_count_{1};
};

// Canonical textual form of a value kind, used by diagnostics and the CLI.
std::string_view to_string(Value::Kind kind) noexcept;

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_VALUE_HPP
