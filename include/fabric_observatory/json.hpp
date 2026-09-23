// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_JSON_HPP
#define FABRIC_OBSERVATORY_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fabric_observatory/status.hpp"
#include "fabric_observatory/value.hpp"

// A strict, bounded JSON subset used for the observation wire format and for
// command line tooling. It exists so that the runtime has no external
// dependency and so that every bound is enforced where the bytes are read.
//
// Integer encoding rule: 64-bit integers are carried as decimal strings, since
// JSON numbers are IEEE-754 doubles and a nanosecond observation timestamp does
// not round-trip through one. Value objects are tagged envelopes so that the
// canonical value model survives a JSON round trip without ambiguity.

namespace fabric_observatory {

struct JsonLimits {
  std::size_t max_input_bytes{16u * 1024u * 1024u};
  std::size_t max_depth{32};
  std::size_t max_nodes{262144};
  std::size_t max_string_bytes{1u * 1024u * 1024u};
  std::size_t max_object_members{4096};
  std::size_t max_array_elements{65536};
};

class JsonValue {
 public:
  enum class Kind : std::uint8_t { Null = 0, Bool = 1, Number = 2, Text = 3, Array = 4, Object = 5 };

  JsonValue() = default;

  [[nodiscard]] static JsonValue null() noexcept { return JsonValue{}; }
  [[nodiscard]] static JsonValue boolean(bool value) noexcept;
  [[nodiscard]] static JsonValue number(double value) noexcept;
  [[nodiscard]] static JsonValue text(std::string value) noexcept;
  [[nodiscard]] static JsonValue array(std::vector<JsonValue> items) noexcept;
  [[nodiscard]] static JsonValue object(std::vector<std::pair<std::string, JsonValue>> members) noexcept;

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return kind_ == Kind::Bool; }
  [[nodiscard]] bool is_number() const noexcept { return kind_ == Kind::Number; }
  [[nodiscard]] bool is_text() const noexcept { return kind_ == Kind::Text; }
  [[nodiscard]] bool is_array() const noexcept { return kind_ == Kind::Array; }
  [[nodiscard]] bool is_object() const noexcept { return kind_ == Kind::Object; }

  [[nodiscard]] bool as_bool() const noexcept { return bool_; }
  [[nodiscard]] double as_number() const noexcept { return number_; }
  [[nodiscard]] const std::string& as_text() const noexcept { return text_; }
  [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return items_; }
  [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
    return members_;
  }

  // Member lookup. Object members keep the order in which they were supplied, so
  // a value built by this runtime serialises identically every time.
  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

  [[nodiscard]] std::string to_text(bool pretty = false) const;

  friend bool operator==(const JsonValue&, const JsonValue&) = default;

 private:
  Kind kind_{Kind::Null};
  bool bool_{false};
  double number_{0.0};
  std::string text_{};
  std::vector<JsonValue> items_{};
  std::vector<std::pair<std::string, JsonValue>> members_{};
};

Result<JsonValue> parse_json(std::string_view input, const JsonLimits& limits);

// Value <-> JSON. The tagged envelope keeps the canonical value model intact:
// {"k":"uint","v":"18446744073709551615"} round-trips exactly.
Result<Value> value_from_json(const JsonValue& json, const ValueLimits& limits);
JsonValue value_to_json(const Value& value);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_JSON_HPP
