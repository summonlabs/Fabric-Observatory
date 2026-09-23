// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/json.hpp"

#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/util.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace fabric_observatory {

namespace {

JsonValue make_tagged(std::string_view kind, JsonValue payload) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("k", JsonValue::text(std::string(kind)));
  members.emplace_back("v", std::move(payload));
  return JsonValue::object(std::move(members));
}

std::string number_to_text(double value) {
  std::array<char, 64> buffer{};
  const std::to_chars_result result =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    return "0";
  }
  return std::string(buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data()));
}

void append_json_string(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char ch : text) {
    switch (ch) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
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
        if (raw < 0x20) {
          std::array<char, 8> escape{};
          std::snprintf(escape.data(), escape.size(), "\\u%04x", static_cast<unsigned>(raw));
          out.append(escape.data());
        } else {
          out.push_back(ch);
        }
        break;
      }
    }
  }
  out.push_back('"');
}

void write_json(std::string& out, const JsonValue& value, bool pretty, std::size_t indent) {
  const auto pad = [&](std::size_t level) {
    if (pretty) {
      out.push_back('\n');
      out.append(level * 2, ' ');
    }
  };
  switch (value.kind()) {
    case JsonValue::Kind::Null:
      out.append("null");
      break;
    case JsonValue::Kind::Bool:
      out.append(value.as_bool() ? "true" : "false");
      break;
    case JsonValue::Kind::Number:
      out.append(number_to_text(value.as_number()));
      break;
    case JsonValue::Kind::Text:
      append_json_string(out, value.as_text());
      break;
    case JsonValue::Kind::Array: {
      if (value.items().empty()) {
        out.append("[]");
        break;
      }
      out.push_back('[');
      for (std::size_t index = 0; index < value.items().size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        pad(indent + 1);
        write_json(out, value.items()[index], pretty, indent + 1);
      }
      pad(indent);
      out.push_back(']');
      break;
    }
    case JsonValue::Kind::Object: {
      if (value.members().empty()) {
        out.append("{}");
        break;
      }
      out.push_back('{');
      for (std::size_t index = 0; index < value.members().size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        pad(indent + 1);
        append_json_string(out, value.members()[index].first);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        write_json(out, value.members()[index].second, pretty, indent + 1);
      }
      pad(indent);
      out.push_back('}');
      break;
    }
  }
}

class Parser {
 public:
  Parser(std::string_view input, const JsonLimits& limits) : input_(input), limits_(limits) {}

  Result<JsonValue> parse() {
    if (input_.size() > limits_.max_input_bytes) {
      return Err<JsonValue>(StatusCode::TooLarge, "JSON input exceeds the configured maximum size");
    }
    skip_whitespace();
    Result<JsonValue> value = parse_value(0);
    if (!value) {
      return value;
    }
    skip_whitespace();
    if (position_ != input_.size()) {
      return Err<JsonValue>(StatusCode::MalformedInput, "trailing content after the JSON value");
    }
    return value;
  }

 private:
  Result<JsonValue> parse_value(std::size_t depth) {
    if (depth > limits_.max_depth) {
      return Err<JsonValue>(StatusCode::DepthExceeded, "JSON nesting exceeds the configured depth");
    }
    if (++nodes_ > limits_.max_nodes) {
      return Err<JsonValue>(StatusCode::TooLarge, "JSON document exceeds the configured node count");
    }
    if (position_ >= input_.size()) {
      return Err<JsonValue>(StatusCode::MalformedInput, "unexpected end of JSON input");
    }
    switch (input_[position_]) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        Result<std::string> text = parse_string();
        if (!text) {
          return Err<JsonValue>(text.status().code(), text.status().message());
        }
        return Ok(JsonValue::text(std::move(*text)));
      }
      case 't':
        return parse_literal("true", JsonValue::boolean(true));
      case 'f':
        return parse_literal("false", JsonValue::boolean(false));
      case 'n':
        return parse_literal("null", JsonValue::null());
      default:
        return parse_number();
    }
  }

  Result<JsonValue> parse_literal(std::string_view literal, JsonValue value) {
    if (input_.substr(position_, literal.size()) != literal) {
      return Err<JsonValue>(StatusCode::MalformedInput, "invalid JSON literal");
    }
    position_ += literal.size();
    return Ok(std::move(value));
  }

  Result<JsonValue> parse_object(std::size_t depth) {
    ++position_;  // '{'
    std::vector<std::pair<std::string, JsonValue>> members;
    skip_whitespace();
    if (position_ < input_.size() && input_[position_] == '}') {
      ++position_;
      return Ok(JsonValue::object(std::move(members)));
    }
    while (true) {
      skip_whitespace();
      if (position_ >= input_.size() || input_[position_] != '"') {
        return Err<JsonValue>(StatusCode::MalformedInput, "expected a JSON object key");
      }
      Result<std::string> key = parse_string();
      if (!key) {
        return Err<JsonValue>(key.status().code(), key.status().message());
      }
      skip_whitespace();
      if (position_ >= input_.size() || input_[position_] != ':') {
        return Err<JsonValue>(StatusCode::MalformedInput, "expected ':' after a JSON object key");
      }
      ++position_;
      skip_whitespace();
      Result<JsonValue> value = parse_value(depth + 1);
      if (!value) {
        return value;
      }
      members.emplace_back(std::move(*key), std::move(*value));
      if (members.size() > limits_.max_object_members) {
        return Err<JsonValue>(StatusCode::TooLarge,
                              "JSON object exceeds the configured member count");
      }
      skip_whitespace();
      if (position_ >= input_.size()) {
        return Err<JsonValue>(StatusCode::MalformedInput, "unterminated JSON object");
      }
      if (input_[position_] == ',') {
        ++position_;
        continue;
      }
      if (input_[position_] == '}') {
        ++position_;
        return Ok(JsonValue::object(std::move(members)));
      }
      return Err<JsonValue>(StatusCode::MalformedInput, "expected ',' or '}' in JSON object");
    }
  }

  Result<JsonValue> parse_array(std::size_t depth) {
    ++position_;  // '['
    std::vector<JsonValue> items;
    skip_whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return Ok(JsonValue::array(std::move(items)));
    }
    while (true) {
      skip_whitespace();
      Result<JsonValue> value = parse_value(depth + 1);
      if (!value) {
        return value;
      }
      items.push_back(std::move(*value));
      if (items.size() > limits_.max_array_elements) {
        return Err<JsonValue>(StatusCode::TooLarge,
                              "JSON array exceeds the configured element count");
      }
      skip_whitespace();
      if (position_ >= input_.size()) {
        return Err<JsonValue>(StatusCode::MalformedInput, "unterminated JSON array");
      }
      if (input_[position_] == ',') {
        ++position_;
        continue;
      }
      if (input_[position_] == ']') {
        ++position_;
        return Ok(JsonValue::array(std::move(items)));
      }
      return Err<JsonValue>(StatusCode::MalformedInput, "expected ',' or ']' in JSON array");
    }
  }

  Result<std::string> parse_string() {
    ++position_;  // opening quote
    std::string out;
    while (true) {
      if (position_ >= input_.size()) {
        return Err<std::string>(StatusCode::MalformedInput, "unterminated JSON string");
      }
      const unsigned char raw = static_cast<unsigned char>(input_[position_]);
      if (raw == static_cast<unsigned char>('"')) {
        ++position_;
        return Ok(std::move(out));
      }
      if (raw == static_cast<unsigned char>('\\')) {
        ++position_;
        if (position_ >= input_.size()) {
          return Err<std::string>(StatusCode::MalformedInput, "unterminated JSON escape");
        }
        const char escape = input_[position_++];
        switch (escape) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            Result<std::uint32_t> code = parse_hex4();
            if (!code) {
              return Err<std::string>(code.status().code(), code.status().message());
            }
            std::uint32_t codepoint = *code;
            if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
              if (position_ + 1 >= input_.size() || input_[position_] != '\\' ||
                  input_[position_ + 1] != 'u') {
                return Err<std::string>(StatusCode::MalformedInput,
                                        "high surrogate without a following low surrogate");
              }
              position_ += 2;
              Result<std::uint32_t> low = parse_hex4();
              if (!low) {
                return Err<std::string>(low.status().code(), low.status().message());
              }
              if (*low < 0xDC00u || *low > 0xDFFFu) {
                return Err<std::string>(StatusCode::MalformedInput,
                                        "high surrogate not followed by a low surrogate");
              }
              codepoint = 0x10000u + ((codepoint - 0xD800u) << 10u) + (*low - 0xDC00u);
            } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
              return Err<std::string>(StatusCode::MalformedInput,
                                      "unexpected low surrogate in JSON string");
            }
            append_utf8(out, codepoint);
            break;
          }
          default:
            return Err<std::string>(StatusCode::MalformedInput, "unknown JSON escape sequence");
        }
      } else if (raw < 0x20) {
        return Err<std::string>(StatusCode::MalformedInput,
                                "unescaped control character in JSON string");
      } else {
        out.push_back(static_cast<char>(raw));
        ++position_;
      }
      if (out.size() > limits_.max_string_bytes) {
        return Err<std::string>(StatusCode::TooLarge,
                                "JSON string exceeds the configured maximum length");
      }
    }
  }

  Result<std::uint32_t> parse_hex4() {
    if (position_ + 4 > input_.size()) {
      return Err<std::uint32_t>(StatusCode::MalformedInput, "truncated \\u escape");
    }
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
      const char ch = input_[position_ + static_cast<std::size_t>(index)];
      std::uint32_t digit = 0;
      if (ch >= '0' && ch <= '9') {
        digit = static_cast<std::uint32_t>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        digit = static_cast<std::uint32_t>(ch - 'a' + 10);
      } else if (ch >= 'A' && ch <= 'F') {
        digit = static_cast<std::uint32_t>(ch - 'A' + 10);
      } else {
        return Err<std::uint32_t>(StatusCode::MalformedInput, "non-hexadecimal digit in \\u escape");
      }
      value = (value << 4u) | digit;
    }
    position_ += 4;
    return Ok(value);
  }

  static void append_utf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint < 0x80u) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800u) {
      out.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else if (codepoint < 0x10000u) {
      out.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
    }
  }

  Result<JsonValue> parse_number() {
    const std::size_t start = position_;
    if (position_ < input_.size() && input_[position_] == '-') {
      ++position_;
    }
    if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
      return Err<JsonValue>(StatusCode::MalformedInput, "expected a JSON value");
    }
    if (input_[position_] == '0') {
      ++position_;
    } else {
      while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < input_.size() && input_[position_] == '.') {
      ++position_;
      if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
        return Err<JsonValue>(StatusCode::MalformedInput, "JSON fraction requires a digit");
      }
      while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        ++position_;
      }
    }
    if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
      ++position_;
      if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
        ++position_;
      }
      if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
        return Err<JsonValue>(StatusCode::MalformedInput, "JSON exponent requires a digit");
      }
      while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
        ++position_;
      }
    }
    const std::string_view token = input_.substr(start, position_ - start);
    double value = 0.0;
    const std::from_chars_result result =
        std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec == std::errc::result_out_of_range) {
      return Err<JsonValue>(StatusCode::OutOfRange, "JSON number is outside the double range");
    }
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
      return Err<JsonValue>(StatusCode::MalformedInput, "malformed JSON number");
    }
    return Ok(JsonValue::number(value));
  }

  void skip_whitespace() {
    while (position_ < input_.size()) {
      const char ch = input_[position_];
      if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
        ++position_;
      } else {
        break;
      }
    }
  }

  std::string_view input_;
  const JsonLimits& limits_;
  std::size_t position_{0};
  std::size_t nodes_{0};
};

}  // namespace

JsonValue JsonValue::boolean(bool value) noexcept {
  JsonValue result;
  result.kind_ = Kind::Bool;
  result.bool_ = value;
  return result;
}

JsonValue JsonValue::number(double value) noexcept {
  JsonValue result;
  result.kind_ = Kind::Number;
  result.number_ = value;
  return result;
}

JsonValue JsonValue::text(std::string value) noexcept {
  JsonValue result;
  result.kind_ = Kind::Text;
  result.text_ = std::move(value);
  return result;
}

JsonValue JsonValue::array(std::vector<JsonValue> items) noexcept {
  JsonValue result;
  result.kind_ = Kind::Array;
  result.items_ = std::move(items);
  return result;
}

JsonValue JsonValue::object(std::vector<std::pair<std::string, JsonValue>> members) noexcept {
  JsonValue result;
  result.kind_ = Kind::Object;
  result.members_ = std::move(members);
  return result;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  for (const auto& member : members_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

std::string JsonValue::to_text(bool pretty) const {
  std::string out;
  write_json(out, *this, pretty, 0);
  return out;
}

Result<JsonValue> parse_json(std::string_view input, const JsonLimits& limits) {
  Parser parser(input, limits);
  return parser.parse();
}

Result<Value> value_from_json(const JsonValue& json, const ValueLimits& limits) {
  if (!json.is_object()) {
    return Err<Value>(StatusCode::MalformedInput, "a value envelope must be a JSON object");
  }
  const JsonValue* kind = json.find("k");
  const JsonValue* payload = json.find("v");
  if (kind == nullptr || !kind->is_text()) {
    return Err<Value>(StatusCode::MalformedInput, "a value envelope requires a text 'k' member");
  }
  const std::string& tag = kind->as_text();

  if (tag == "null") {
    return Ok(Value::null());
  }
  if (tag == "bool") {
    if (payload == nullptr || !payload->is_bool()) {
      return Err<Value>(StatusCode::MalformedInput, "bool value requires a boolean 'v'");
    }
    return Ok(Value::boolean(payload->as_bool()));
  }
  if (tag == "int" || tag == "uint") {
    if (payload == nullptr || !payload->is_text()) {
      return Err<Value>(StatusCode::MalformedInput,
                        "integer values are carried as decimal strings");
    }
    const std::string& digits = payload->as_text();
    if (tag == "int") {
      std::int64_t parsed = 0;
      const std::from_chars_result result =
          std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
      if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
        return Err<Value>(StatusCode::MalformedInput, "malformed signed integer value");
      }
      return Ok(Value::integer(parsed));
    }
    std::uint64_t parsed = 0;
    const std::from_chars_result result =
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
      return Err<Value>(StatusCode::MalformedInput, "malformed unsigned integer value");
    }
    return Ok(Value::unsigned_integer(parsed));
  }
  if (tag == "real") {
    if (payload == nullptr || !payload->is_number()) {
      return Err<Value>(StatusCode::MalformedInput, "real value requires a numeric 'v'");
    }
    return Value::real(payload->as_number());
  }
  if (tag == "text") {
    if (payload == nullptr || !payload->is_text()) {
      return Err<Value>(StatusCode::MalformedInput, "text value requires a text 'v'");
    }
    return Value::text(payload->as_text(), limits);
  }
  if (tag == "blob") {
    if (payload == nullptr || !payload->is_text()) {
      return Err<Value>(StatusCode::MalformedInput, "blob value requires a hex text 'v'");
    }
    Result<std::vector<std::byte>> decoded = util::hex_decode(payload->as_text());
    if (!decoded) {
      return Err<Value>(decoded.status().code(), decoded.status().message());
    }
    return Value::blob(std::move(*decoded), limits);
  }
  if (tag == "list") {
    if (payload == nullptr || !payload->is_array()) {
      return Err<Value>(StatusCode::MalformedInput, "list value requires an array 'v'");
    }
    std::vector<Value> elements;
    elements.reserve(payload->items().size());
    for (const JsonValue& item : payload->items()) {
      Result<Value> element = value_from_json(item, limits);
      if (!element) {
        return element;
      }
      elements.push_back(std::move(*element));
    }
    return Value::list(std::move(elements), limits);
  }
  if (tag == "map") {
    if (payload == nullptr || !payload->is_array()) {
      return Err<Value>(StatusCode::MalformedInput, "map value requires an array 'v'");
    }
    std::vector<std::pair<std::string, Value>> entries;
    entries.reserve(payload->items().size());
    for (const JsonValue& item : payload->items()) {
      if (!item.is_object()) {
        return Err<Value>(StatusCode::MalformedInput, "map entries must be JSON objects");
      }
      const JsonValue* key = item.find("k");
      const JsonValue* entry_value = item.find("v");
      if (key == nullptr || !key->is_text() || entry_value == nullptr) {
        return Err<Value>(StatusCode::MalformedInput,
                          "map entries require text 'k' and a value 'v'");
      }
      Result<Value> converted = value_from_json(*entry_value, limits);
      if (!converted) {
        return converted;
      }
      entries.emplace_back(key->as_text(), std::move(*converted));
    }
    return Value::map(std::move(entries), limits);
  }
  return Err<Value>(StatusCode::Unsupported, "unknown value envelope tag");
}

JsonValue value_to_json(const Value& value) {
  switch (value.kind()) {
    case Value::Kind::Null:
      return make_tagged("null", JsonValue::null());
    case Value::Kind::Bool:
      return make_tagged("bool", JsonValue::boolean(value.as_bool()));
    case Value::Kind::Int:
      return make_tagged("int", JsonValue::text(std::to_string(value.as_int())));
    case Value::Kind::Uint:
      return make_tagged("uint", JsonValue::text(std::to_string(value.as_uint())));
    case Value::Kind::Real:
      return make_tagged("real", JsonValue::number(value.as_real()));
    case Value::Kind::Text:
      return make_tagged("text", JsonValue::text(value.as_text()));
    case Value::Kind::Blob:
      return make_tagged("blob",
                         JsonValue::text(util::hex_encode(
                             ByteSpan(value.as_blob().data(), value.as_blob().size()))));
    case Value::Kind::List: {
      std::vector<JsonValue> items;
      items.reserve(value.elements().size());
      for (const Value& element : value.elements()) {
        items.push_back(value_to_json(element));
      }
      return make_tagged("list", JsonValue::array(std::move(items)));
    }
    case Value::Kind::Map: {
      std::vector<JsonValue> items;
      items.reserve(value.entries().size());
      for (const auto& entry : value.entries()) {
        std::vector<std::pair<std::string, JsonValue>> members;
        members.emplace_back("k", JsonValue::text(entry.first));
        members.emplace_back("v", value_to_json(entry.second));
        items.push_back(JsonValue::object(std::move(members)));
      }
      return make_tagged("map", JsonValue::array(std::move(items)));
    }
  }
  return make_tagged("null", JsonValue::null());
}

}  // namespace fabric_observatory
