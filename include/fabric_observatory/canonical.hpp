// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_CANONICAL_HPP
#define FABRIC_OBSERVATORY_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_observatory/digest.hpp"

namespace fabric_observatory {

// Canonical binary encoding. Every multi-byte integer is big-endian and every
// variable-length field is prefixed with its length, so the encoding is
// unambiguous, architecture independent and stable across runs. Snapshot and
// observation identity are defined as the SHA-256 of this encoding.
class CanonicalEncoder {
 public:
  explicit CanonicalEncoder(std::size_t reserve_bytes = 256);

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void raw(ByteSpan data);
  void bytes(ByteSpan data);
  void text(std::string_view value);

  // Domain separation: a short tag that makes two structurally similar
  // encodings distinct.
  void tag(std::string_view value);

  [[nodiscard]] const std::vector<std::byte>& buffer() const noexcept { return bytes_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] Digest256 digest() const noexcept;
  [[nodiscard]] std::string to_hex() const;

 private:
  std::vector<std::byte> bytes_;
};

// Mirror of CanonicalEncoder. Every read is bounds checked against the input
// span, and a failed read poisons the decoder so that a partially decoded
// structure can never be mistaken for a complete one.
class CanonicalDecoder {
 public:
  explicit CanonicalDecoder(ByteSpan data) noexcept : data_(data) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t position() const noexcept { return position_; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return position_ <= data_.size() ? data_.size() - position_ : 0;
  }
  [[nodiscard]] bool at_end() const noexcept { return remaining() == 0; }

  Result<std::uint8_t> u8() noexcept;
  Result<std::uint16_t> u16() noexcept;
  Result<std::uint32_t> u32() noexcept;
  Result<std::uint64_t> u64() noexcept;
  Result<std::int64_t> i64() noexcept;
  Result<bool> boolean() noexcept;
  Result<ByteSpan> bytes() noexcept;
  Result<std::string_view> text() noexcept;
  Result<void> tag(std::string_view expected) noexcept;
  Result<void> expect_u8(std::uint8_t expected) noexcept;
  Result<void> expect_end() noexcept;
  void poison() noexcept { ok_ = false; }

 private:
  Result<ByteSpan> take(std::size_t count) noexcept;

  ByteSpan data_{};
  std::size_t position_{0};
  bool ok_{true};
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_CANONICAL_HPP
