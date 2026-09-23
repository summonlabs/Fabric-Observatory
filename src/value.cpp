// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/value.hpp"

#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fabric_observatory {

namespace {

std::string hex_of_bytes(const std::vector<std::byte>& data) {
  return util::hex_encode(ByteSpan(data.data(), data.size()));
}

void append_escaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default: {
        const unsigned char raw = static_cast<unsigned char>(ch);
        if (raw < 0x20 || raw == 0x7F) {
          constexpr char kDigits[] = "0123456789abcdef";
          out.append("\\x");
          out.push_back(kDigits[raw >> 4u]);
          out.push_back(kDigits[raw & 0x0Fu]);
        } else {
          out.push_back(ch);
        }
        break;
      }
    }
  }
  out.push_back('"');
}

std::string real_to_text(double value) {
  std::array<char, 64> buffer{};
  const std::to_chars_result result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    return "0";
  }
  return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

void verify_limits(std::size_t depth, std::size_t nodes, const ValueLimits& limits) {
  FABRIC_OBSERVATORY_CONTRACT(depth >= 1);
  FABRIC_OBSERVATORY_CONTRACT(nodes >= 1);
  FABRIC_OBSERVATORY_CONTRACT(depth <= limits.max_depth);
  FABRIC_OBSERVATORY_CONTRACT(nodes <= limits.max_total_nodes);
}

}  // namespace

std::string_view to_string(Value::Kind kind) noexcept {
  switch (kind) {
    case Value::Kind::Null:
      return "null";
    case Value::Kind::Bool:
      return "bool";
    case Value::Kind::Int:
      return "int";
    case Value::Kind::Uint:
      return "uint";
    case Value::Kind::Real:
      return "real";
    case Value::Kind::Text:
      return "text";
    case Value::Kind::Blob:
      return "blob";
    case Value::Kind::List:
      return "list";
    case Value::Kind::Map:
      return "map";
  }
  return "null";
}

Value Value::boolean(bool value) noexcept {
  Value result;
  result.kind_ = Kind::Bool;
  result.bool_ = value;
  return result;
}

Value Value::integer(std::int64_t value) noexcept {
  Value result;
  result.kind_ = Kind::Int;
  result.int_ = value;
  return result;
}

Value Value::unsigned_integer(std::uint64_t value) noexcept {
  Value result;
  result.kind_ = Kind::Uint;
  result.uint_ = value;
  return result;
}

Result<Value> Value::real(double value) {
  if (!std::isfinite(value)) {
    return Err<Value>(StatusCode::InvalidArgument,
                      "non-finite real values are not representable and are rejected");
  }
  Value result;
  result.kind_ = Kind::Real;
  result.real_ = value;
  return Ok(std::move(result));
}

Result<Value> Value::text(std::string value, const ValueLimits& limits) {
  if (value.size() > limits.max_text_bytes) {
    return Err<Value>(StatusCode::TooLarge, "text value exceeds the configured maximum length");
  }
  Value result;
  result.kind_ = Kind::Text;
  result.text_ = std::move(value);
  return Ok(std::move(result));
}

Result<Value> Value::blob(std::vector<std::byte> value, const ValueLimits& limits) {
  if (value.size() > limits.max_blob_bytes) {
    return Err<Value>(StatusCode::TooLarge, "blob value exceeds the configured maximum length");
  }
  Value result;
  result.kind_ = Kind::Blob;
  result.blob_ = std::move(value);
  return Ok(std::move(result));
}

Result<Value> Value::list(std::vector<Value> elements, const ValueLimits& limits) {
  if (elements.size() > limits.max_list_elements) {
    return Err<Value>(StatusCode::TooLarge, "list value exceeds the configured maximum element count");
  }
  std::size_t depth = 1;
  std::size_t nodes = 1;
  for (const Value& element : elements) {
    depth = std::max(depth, element.depth() + 1);
    const std::optional<std::size_t> sum = checked_add(nodes, element.node_count());
    if (!sum.has_value()) {
      return Err<Value>(StatusCode::TooLarge, "list value node count overflows");
    }
    nodes = *sum;
  }
  if (depth > limits.max_depth) {
    return Err<Value>(StatusCode::DepthExceeded, "list value exceeds the configured maximum depth");
  }
  if (nodes > limits.max_total_nodes) {
    return Err<Value>(StatusCode::TooLarge, "list value exceeds the configured maximum node count");
  }
  Value result;
  result.kind_ = Kind::List;
  result.list_ = std::move(elements);
  result.depth_ = depth;
  result.node_count_ = nodes;
  verify_limits(result.depth_, result.node_count_, limits);
  return Ok(std::move(result));
}

Result<Value> Value::map(std::vector<std::pair<std::string, Value>> entries, const ValueLimits& limits) {
  if (entries.size() > limits.max_map_entries) {
    return Err<Value>(StatusCode::TooLarge, "map value exceeds the configured maximum entry count");
  }
  for (const auto& entry : entries) {
    if (entry.first.size() > limits.max_text_bytes) {
      return Err<Value>(StatusCode::TooLarge, "map key exceeds the configured maximum length");
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (entries[index - 1].first == entries[index].first) {
      return Err<Value>(StatusCode::InvalidArgument, "map value contains a duplicate key");
    }
  }

  std::size_t depth = 1;
  std::size_t nodes = 1;
  for (const auto& entry : entries) {
    depth = std::max(depth, entry.second.depth() + 1);
    const std::optional<std::size_t> sum = checked_add(nodes, entry.second.node_count());
    if (!sum.has_value()) {
      return Err<Value>(StatusCode::TooLarge, "map value node count overflows");
    }
    nodes = *sum;
  }
  if (depth > limits.max_depth) {
    return Err<Value>(StatusCode::DepthExceeded, "map value exceeds the configured maximum depth");
  }
  if (nodes > limits.max_total_nodes) {
    return Err<Value>(StatusCode::TooLarge, "map value exceeds the configured maximum node count");
  }
  Value result;
  result.kind_ = Kind::Map;
  result.map_ = std::move(entries);
  result.depth_ = depth;
  result.node_count_ = nodes;
  verify_limits(result.depth_, result.node_count_, limits);
  return Ok(std::move(result));
}

Result<void> Value::validate(const ValueLimits& limits) const {
  if (node_count_ > limits.max_total_nodes) {
    return Err(StatusCode::TooLarge, "value exceeds the configured node count");
  }
  if (depth_ > limits.max_depth) {
    return Err(StatusCode::DepthExceeded, "value exceeds the configured depth");
  }
  switch (kind_) {
    case Kind::Text:
      if (text_.size() > limits.max_text_bytes) {
        return Err(StatusCode::TooLarge, "text value exceeds the configured length");
      }
      break;
    case Kind::Blob:
      if (blob_.size() > limits.max_blob_bytes) {
        return Err(StatusCode::TooLarge, "blob value exceeds the configured length");
      }
      break;
    case Kind::List:
      if (list_.size() > limits.max_list_elements) {
        return Err(StatusCode::TooLarge, "list value exceeds the configured element count");
      }
      for (const Value& element : list_) {
        Result<void> valid = element.validate(limits);
        if (!valid) {
          return valid;
        }
      }
      break;
    case Kind::Map:
      if (map_.size() > limits.max_map_entries) {
        return Err(StatusCode::TooLarge, "map value exceeds the configured entry count");
      }
      for (const auto& entry : map_) {
        if (entry.first.size() > limits.max_text_bytes) {
          return Err(StatusCode::TooLarge, "map key exceeds the configured length");
        }
        Result<void> valid = entry.second.validate(limits);
        if (!valid) {
          return valid;
        }
      }
      break;
    default:
      break;
  }
  return VoidResult{};
}

std::string Value::canonical_text() const {
  std::string out;
  switch (kind_) {
    case Kind::Null:
      out = "n";
      break;
    case Kind::Bool:
      out = bool_ ? "b:1" : "b:0";
      break;
    case Kind::Int:
      out = "i:" + std::to_string(int_);
      break;
    case Kind::Uint:
      out = "u:" + std::to_string(uint_);
      break;
    case Kind::Real: {
      const std::uint64_t bits = std::bit_cast<std::uint64_t>(real_);
      std::array<char, 32> buffer{};
      std::snprintf(buffer.data(), buffer.size(), "r:%016llx", static_cast<unsigned long long>(bits));
      out.assign(buffer.data());
      break;
    }
    case Kind::Text:
      out = "s:" + std::to_string(text_.size()) + ":" + text_;
      break;
    case Kind::Blob:
      out = "x:" + hex_of_bytes(blob_);
      break;
    case Kind::List: {
      out = "l[";
      for (std::size_t index = 0; index < list_.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        const std::string child = list_[index].canonical_text();
        out += std::to_string(child.size());
        out.push_back(':');
        out += child;
      }
      out.push_back(']');
      break;
    }
    case Kind::Map: {
      out = "m{";
      for (std::size_t index = 0; index < map_.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        const std::string key = map_[index].first;
        const std::string child = map_[index].second.canonical_text();
        out += std::to_string(key.size());
        out.push_back(':');
        out += key;
        out.push_back('=');
        out += std::to_string(child.size());
        out.push_back(':');
        out += child;
      }
      out.push_back('}');
      break;
    }
  }
  return out;
}

std::string Value::display_text() const {
  std::string out;
  switch (kind_) {
    case Kind::Null:
      out = "null";
      break;
    case Kind::Bool:
      out = bool_ ? "true" : "false";
      break;
    case Kind::Int:
      out = std::to_string(int_);
      break;
    case Kind::Uint:
      out = std::to_string(uint_);
      break;
    case Kind::Real:
      out = real_to_text(real_);
      break;
    case Kind::Text:
      append_escaped(out, text_);
      break;
    case Kind::Blob:
      out = "0x" + hex_of_bytes(blob_);
      break;
    case Kind::List: {
      out.push_back('[');
      for (std::size_t index = 0; index < list_.size(); ++index) {
        if (index != 0) {
          out.append(", ");
        }
        out += list_[index].display_text();
      }
      out.push_back(']');
      break;
    }
    case Kind::Map: {
      out.push_back('{');
      for (std::size_t index = 0; index < map_.size(); ++index) {
        if (index != 0) {
          out.append(", ");
        }
        append_escaped(out, map_[index].first);
        out.append(": ");
        out += map_[index].second.display_text();
      }
      out.push_back('}');
      break;
    }
  }
  return out;
}

void Value::encode(CanonicalEncoder& encoder) const {
  encoder.u8(static_cast<std::uint8_t>(kind_));
  switch (kind_) {
    case Kind::Null:
      break;
    case Kind::Bool:
      encoder.boolean(bool_);
      break;
    case Kind::Int:
      encoder.i64(int_);
      break;
    case Kind::Uint:
      encoder.u64(uint_);
      break;
    case Kind::Real:
      encoder.u64(std::bit_cast<std::uint64_t>(real_));
      break;
    case Kind::Text:
      encoder.text(text_);
      break;
    case Kind::Blob:
      encoder.bytes(ByteSpan(blob_.data(), blob_.size()));
      break;
    case Kind::List:
      encoder.u64(static_cast<std::uint64_t>(list_.size()));
      for (const Value& element : list_) {
        element.encode(encoder);
      }
      break;
    case Kind::Map:
      encoder.u64(static_cast<std::uint64_t>(map_.size()));
      for (const auto& entry : map_) {
        encoder.text(entry.first);
        entry.second.encode(encoder);
      }
      break;
  }
}

Result<Value> Value::decode(CanonicalDecoder& decoder, const ValueLimits& limits) {
  Result<std::uint8_t> raw_kind = decoder.u8();
  if (!raw_kind) {
    return Err<Value>(raw_kind.status().code(), raw_kind.status().message());
  }
  if (*raw_kind > static_cast<std::uint8_t>(Kind::Map)) {
    decoder.poison();
    return Err<Value>(StatusCode::Corrupt, "unknown canonical value kind");
  }
  const auto kind = static_cast<Kind>(*raw_kind);

  switch (kind) {
    case Kind::Null:
      return Ok(Value::null());
    case Kind::Bool: {
      Result<bool> value = decoder.boolean();
      if (!value) {
        return Err<Value>(value.status().code(), value.status().message());
      }
      return Ok(Value::boolean(*value));
    }
    case Kind::Int: {
      Result<std::int64_t> value = decoder.i64();
      if (!value) {
        return Err<Value>(value.status().code(), value.status().message());
      }
      return Ok(Value::integer(*value));
    }
    case Kind::Uint: {
      Result<std::uint64_t> value = decoder.u64();
      if (!value) {
        return Err<Value>(value.status().code(), value.status().message());
      }
      return Ok(Value::unsigned_integer(*value));
    }
    case Kind::Real: {
      Result<std::uint64_t> bits = decoder.u64();
      if (!bits) {
        return Err<Value>(bits.status().code(), bits.status().message());
      }
      return Value::real(std::bit_cast<double>(*bits));
    }
    case Kind::Text: {
      Result<std::string_view> value = decoder.text();
      if (!value) {
        return Err<Value>(value.status().code(), value.status().message());
      }
      return Value::text(std::string(*value), limits);
    }
    case Kind::Blob: {
      Result<ByteSpan> value = decoder.bytes();
      if (!value) {
        return Err<Value>(value.status().code(), value.status().message());
      }
      std::vector<std::byte> blob(value->begin(), value->end());
      return Value::blob(std::move(blob), limits);
    }
    case Kind::List: {
      Result<std::uint64_t> count = decoder.u64();
      if (!count) {
        return Err<Value>(count.status().code(), count.status().message());
      }
      if (*count > static_cast<std::uint64_t>(limits.max_list_elements)) {
        decoder.poison();
        return Err<Value>(StatusCode::TooLarge, "canonical list exceeds the configured bound");
      }
      std::vector<Value> elements;
      elements.reserve(static_cast<std::size_t>(*count));
      for (std::uint64_t index = 0; index < *count; ++index) {
        Result<Value> element = Value::decode(decoder, limits);
        if (!element) {
          return element;
        }
        elements.push_back(std::move(*element));
      }
      return Value::list(std::move(elements), limits);
    }
    case Kind::Map: {
      Result<std::uint64_t> count = decoder.u64();
      if (!count) {
        return Err<Value>(count.status().code(), count.status().message());
      }
      if (*count > static_cast<std::uint64_t>(limits.max_map_entries)) {
        decoder.poison();
        return Err<Value>(StatusCode::TooLarge, "canonical map exceeds the configured bound");
      }
      std::vector<std::pair<std::string, Value>> entries;
      entries.reserve(static_cast<std::size_t>(*count));
      for (std::uint64_t index = 0; index < *count; ++index) {
        Result<std::string_view> key = decoder.text();
        if (!key) {
          return Err<Value>(key.status().code(), key.status().message());
        }
        Result<Value> entry = Value::decode(decoder, limits);
        if (!entry) {
          return entry;
        }
        entries.emplace_back(std::string(*key), std::move(*entry));
      }
      return Value::map(std::move(entries), limits);
    }
  }
  decoder.poison();
  return Err<Value>(StatusCode::Corrupt, "unreachable canonical value kind");
}

std::strong_ordering operator<=>(const Value& lhs, const Value& rhs) {
  const auto lhs_kind = static_cast<std::uint8_t>(lhs.kind_);
  const auto rhs_kind = static_cast<std::uint8_t>(rhs.kind_);
  if (lhs_kind != rhs_kind) {
    return lhs_kind <=> rhs_kind;
  }
  switch (lhs.kind_) {
    case Value::Kind::Null:
      return std::strong_ordering::equal;
    case Value::Kind::Bool:
      return lhs.bool_ <=> rhs.bool_;
    case Value::Kind::Int:
      return lhs.int_ <=> rhs.int_;
    case Value::Kind::Uint:
      return lhs.uint_ <=> rhs.uint_;
    case Value::Kind::Real: {
      const std::uint64_t lhs_bits = std::bit_cast<std::uint64_t>(lhs.real_);
      const std::uint64_t rhs_bits = std::bit_cast<std::uint64_t>(rhs.real_);
      return lhs_bits <=> rhs_bits;
    }
    case Value::Kind::Text:
      return lhs.text_ <=> rhs.text_;
    case Value::Kind::Blob: {
      const std::string lhs_hex = hex_of_bytes(lhs.blob_);
      const std::string rhs_hex = hex_of_bytes(rhs.blob_);
      return lhs_hex <=> rhs_hex;
    }
    case Value::Kind::List: {
      const std::size_t common = std::min(lhs.list_.size(), rhs.list_.size());
      for (std::size_t index = 0; index < common; ++index) {
        if (const std::strong_ordering order = lhs.list_[index] <=> rhs.list_[index];
            order != std::strong_ordering::equal) {
          return order;
        }
      }
      return lhs.list_.size() <=> rhs.list_.size();
    }
    case Value::Kind::Map: {
      const std::size_t common = std::min(lhs.map_.size(), rhs.map_.size());
      for (std::size_t index = 0; index < common; ++index) {
        if (const std::strong_ordering order = lhs.map_[index].first <=> rhs.map_[index].first;
            order != std::strong_ordering::equal) {
          return order;
        }
        if (const std::strong_ordering order = lhs.map_[index].second <=> rhs.map_[index].second;
            order != std::strong_ordering::equal) {
          return order;
        }
      }
      return lhs.map_.size() <=> rhs.map_.size();
    }
  }
  return std::strong_ordering::equal;
}

}  // namespace fabric_observatory
