// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_DIGEST_HPP
#define FABRIC_OBSERVATORY_DIGEST_HPP

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "fabric_observatory/export.hpp"
#include "fabric_observatory/status.hpp"

namespace fabric_observatory {

using ByteSpan = std::span<const std::byte>;

// SHA-256, used for content addressed identity and for the journal hash chain.
class Sha256 {
 public:
  static constexpr std::size_t kDigestBytes = 32;

  Sha256() noexcept;
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(ByteSpan data) noexcept;
  void update(std::string_view data) noexcept;
  void update_byte(std::uint8_t value) noexcept;

  // Finalizes and returns the digest. The object must not be reused afterwards.
  [[nodiscard]] std::array<std::uint8_t, kDigestBytes> finish() noexcept;

 private:
  void append(const std::uint8_t* data, std::size_t size) noexcept;
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_{0};
  std::uint64_t total_bytes_{0};
  bool finalized_{false};
};

struct Digest256 {
  std::array<std::uint8_t, 32> bytes{};

  friend bool operator==(const Digest256&, const Digest256&) = default;
  friend auto operator<=>(const Digest256&, const Digest256&) = default;

  [[nodiscard]] bool is_zero() const noexcept;
  [[nodiscard]] std::string to_hex() const;
  [[nodiscard]] static Result<Digest256> from_hex(std::string_view text);

  // Short form for human facing output: first 12 hex characters.
  [[nodiscard]] std::string to_short_hex() const;
};

// Byte view of a digest, for encoding and integrity checks.
[[nodiscard]] inline ByteSpan as_bytes(const Digest256& digest) noexcept {
  return ByteSpan(reinterpret_cast<const std::byte*>(digest.bytes.data()), digest.bytes.size());
}

Digest256 sha256(ByteSpan data) noexcept;
Digest256 sha256(std::string_view data) noexcept;

// CRC-32C (Castagnoli) used as a cheap per-record integrity check in the
// journal. It complements, and never replaces, the SHA-256 hash chain.
class Crc32c {
 public:
  void update(ByteSpan data) noexcept;
  void update(std::string_view data) noexcept;
  [[nodiscard]] std::uint32_t value() const noexcept { return ~state_; }

 private:
  std::uint32_t state_{0xFFFFFFFFu};
};

std::uint32_t crc32c(ByteSpan data) noexcept;
std::uint32_t crc32c(std::string_view data) noexcept;

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_DIGEST_HPP
