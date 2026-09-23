// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/canonical.hpp"

#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/util.hpp"

#include <optional>

namespace fabric_observatory {

CanonicalEncoder::CanonicalEncoder(std::size_t reserve_bytes) { bytes_.reserve(reserve_bytes); }

void CanonicalEncoder::u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

void CanonicalEncoder::u16(std::uint16_t value) {
  u8(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void CanonicalEncoder::u32(std::uint32_t value) {
  u8(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
  u8(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
  u8(static_cast<std::uint8_t>(value & 0xFFu));
}

void CanonicalEncoder::u64(std::uint64_t value) {
  u32(static_cast<std::uint32_t>((value >> 32u) & 0xFFFFFFFFu));
  u32(static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

void CanonicalEncoder::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void CanonicalEncoder::boolean(bool value) { u8(value ? 1u : 0u); }

void CanonicalEncoder::raw(ByteSpan data) {
  bytes_.insert(bytes_.end(), data.begin(), data.end());
}

void CanonicalEncoder::bytes(ByteSpan data) {
  u64(static_cast<std::uint64_t>(data.size()));
  raw(data);
}

void CanonicalEncoder::text(std::string_view value) {
  bytes(ByteSpan(reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

void CanonicalEncoder::tag(std::string_view value) {
  u8(static_cast<std::uint8_t>(value.size() & 0xFFu));
  raw(ByteSpan(reinterpret_cast<const std::byte*>(value.data()), value.size()));
}

Digest256 CanonicalEncoder::digest() const noexcept {
  return sha256(ByteSpan(bytes_.data(), bytes_.size()));
}

std::string CanonicalEncoder::to_hex() const {
  return util::hex_encode(ByteSpan(bytes_.data(), bytes_.size()));
}

Result<ByteSpan> CanonicalDecoder::take(std::size_t count) noexcept {
  if (!ok_) {
    return Err<ByteSpan>(StatusCode::Corrupt, "decoder is poisoned");
  }
  if (count > remaining()) {
    ok_ = false;
    return Err<ByteSpan>(StatusCode::Corrupt, "canonical input ended inside a field");
  }
  const ByteSpan slice = data_.subspan(position_, count);
  position_ += count;
  return Ok(slice);
}

Result<std::uint8_t> CanonicalDecoder::u8() noexcept {
  Result<ByteSpan> slice = take(1);
  if (!slice) {
    return Err<std::uint8_t>(slice.status().code(), slice.status().message());
  }
  return Ok(std::to_integer<std::uint8_t>(slice->front()));
}

Result<std::uint16_t> CanonicalDecoder::u16() noexcept {
  Result<ByteSpan> slice = take(2);
  if (!slice) {
    return Err<std::uint16_t>(slice.status().code(), slice.status().message());
  }
  std::uint16_t value = 0;
  for (const std::byte raw : *slice) {
    value = static_cast<std::uint16_t>((value << 8u) | std::to_integer<std::uint8_t>(raw));
  }
  return Ok(value);
}

Result<std::uint32_t> CanonicalDecoder::u32() noexcept {
  Result<ByteSpan> slice = take(4);
  if (!slice) {
    return Err<std::uint32_t>(slice.status().code(), slice.status().message());
  }
  std::uint32_t value = 0;
  for (const std::byte raw : *slice) {
    value = (value << 8u) | std::to_integer<std::uint8_t>(raw);
  }
  return Ok(value);
}

Result<std::uint64_t> CanonicalDecoder::u64() noexcept {
  Result<ByteSpan> slice = take(8);
  if (!slice) {
    return Err<std::uint64_t>(slice.status().code(), slice.status().message());
  }
  std::uint64_t value = 0;
  for (const std::byte raw : *slice) {
    value = (value << 8u) | std::to_integer<std::uint8_t>(raw);
  }
  return Ok(value);
}

Result<std::int64_t> CanonicalDecoder::i64() noexcept {
  Result<std::uint64_t> value = u64();
  if (!value) {
    return Err<std::int64_t>(value.status().code(), value.status().message());
  }
  return Ok(static_cast<std::int64_t>(*value));
}

Result<bool> CanonicalDecoder::boolean() noexcept {
  Result<std::uint8_t> value = u8();
  if (!value) {
    return Err<bool>(value.status().code(), value.status().message());
  }
  if (*value > 1u) {
    ok_ = false;
    return Err<bool>(StatusCode::Corrupt, "canonical boolean is not 0 or 1");
  }
  return Ok(*value == 1u);
}

Result<ByteSpan> CanonicalDecoder::bytes() noexcept {
  Result<std::uint64_t> length = u64();
  if (!length) {
    return Err<ByteSpan>(length.status().code(), length.status().message());
  }
  const std::optional<std::size_t> narrowed = checked_size(*length);
  if (!narrowed.has_value()) {
    ok_ = false;
    return Err<ByteSpan>(StatusCode::TooLarge, "canonical length does not fit in size_t");
  }
  return take(*narrowed);
}

Result<std::string_view> CanonicalDecoder::text() noexcept {
  Result<ByteSpan> slice = bytes();
  if (!slice) {
    return Err<std::string_view>(slice.status().code(), slice.status().message());
  }
  return Ok(std::string_view(reinterpret_cast<const char*>(slice->data()), slice->size()));
}

Result<void> CanonicalDecoder::tag(std::string_view expected) noexcept {
  Result<std::uint8_t> length = u8();
  if (!length) {
    return Err<void>(length.status().code(), length.status().message());
  }
  Result<ByteSpan> slice = take(*length);
  if (!slice) {
    return Err<void>(slice.status().code(), slice.status().message());
  }
  const std::string_view actual(reinterpret_cast<const char*>(slice->data()), slice->size());
  if (actual != expected) {
    ok_ = false;
    return Err<void>(StatusCode::Corrupt, "canonical domain tag does not match");
  }
  return VoidResult{};
}

Result<void> CanonicalDecoder::expect_u8(std::uint8_t expected) noexcept {
  Result<std::uint8_t> value = u8();
  if (!value) {
    return Err<void>(value.status().code(), value.status().message());
  }
  if (*value != expected) {
    ok_ = false;
    return Err<void>(StatusCode::VersionMismatch, "unexpected canonical encoding revision");
  }
  return VoidResult{};
}

Result<void> CanonicalDecoder::expect_end() noexcept {
  if (!at_end()) {
    ok_ = false;
    return Err<void>(StatusCode::Corrupt, "trailing bytes after a canonical structure");
  }
  return VoidResult{};
}

}  // namespace fabric_observatory

