// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/util.hpp"

#include <array>
#include <cctype>

namespace fabric_observatory::util {

std::string_view trim(std::string_view text) noexcept {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end) {
    const char ch = text[begin];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      break;
    }
    ++begin;
  }
  while (end > begin) {
    const char ch = text[end - 1];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      break;
    }
    --end;
  }
  return text.substr(begin, end - begin);
}

bool starts_with(std::string_view text, std::string_view prefix) noexcept {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string to_lower_ascii(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char ch : text) {
    const unsigned char raw = static_cast<unsigned char>(ch);
    result.push_back(static_cast<char>(std::tolower(raw)));
  }
  return result;
}

namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

constexpr std::array<std::int8_t, 256> build_hex_table() {
  std::array<std::int8_t, 256> table{};
  for (std::size_t index = 0; index < table.size(); ++index) {
    table[index] = -1;
  }
  for (std::int8_t index = 0; index < 10; ++index) {
    table[static_cast<std::size_t>('0' + index)] = index;
  }
  for (std::int8_t index = 0; index < 6; ++index) {
    table[static_cast<std::size_t>('a' + index)] = static_cast<std::int8_t>(10 + index);
    table[static_cast<std::size_t>('A' + index)] = static_cast<std::int8_t>(10 + index);
  }
  return table;
}

constexpr std::array<std::int8_t, 256> kHexTable = build_hex_table();

}  // namespace

std::string hex_encode(ByteSpan data) {
  std::string result;
  result.reserve(data.size() * 2);
  for (const std::byte raw : data) {
    const auto value = static_cast<std::uint8_t>(raw);
    result.push_back(kHexDigits[value >> 4u]);
    result.push_back(kHexDigits[value & 0x0Fu]);
  }
  return result;
}

Result<std::vector<std::byte>> hex_decode(std::string_view text) {
  if (text.size() % 2 != 0) {
    return Err<std::vector<std::byte>>(StatusCode::MalformedInput,
                                       "hex text must have an even number of characters");
  }
  std::vector<std::byte> result;
  result.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const std::int8_t high = kHexTable[static_cast<unsigned char>(text[index])];
    const std::int8_t low = kHexTable[static_cast<unsigned char>(text[index + 1])];
    if (high < 0 || low < 0) {
      return Err<std::vector<std::byte>>(StatusCode::MalformedInput,
                                         "hex text contains a non-hexadecimal character");
    }
    const auto value = static_cast<std::uint8_t>((high << 4) | low);
    result.push_back(static_cast<std::byte>(value));
  }
  return Ok(std::move(result));
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string result;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      result.append(separator);
    }
    result.append(parts[index]);
  }
  return result;
}

bool is_valid_identifier(std::string_view text, std::size_t max_length) noexcept {
  if (text.empty() || text.size() > max_length) {
    return false;
  }
  for (const char ch : text) {
    const bool lower = ch >= 'a' && ch <= 'z';
    const bool digit = ch >= '0' && ch <= '9';
    const bool separator = ch == '.' || ch == '_' || ch == '-';
    if (!lower && !digit && !separator) {
      return false;
    }
  }
  return true;
}

bool is_valid_authority_name(std::string_view text) noexcept {
  if (text.empty() || text.size() > 64) {
    return false;
  }
  for (const char ch : text) {
    const bool lower = ch >= 'a' && ch <= 'z';
    const bool upper = ch >= 'A' && ch <= 'Z';
    const bool digit = ch >= '0' && ch <= '9';
    const bool separator = ch == '.' || ch == '_' || ch == '-' || ch == '@' || ch == ':';
    if (!lower && !upper && !digit && !separator) {
      return false;
    }
  }
  return true;
}

}  // namespace fabric_observatory::util
