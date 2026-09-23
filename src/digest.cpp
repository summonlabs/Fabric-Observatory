// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/digest.hpp"

#include "fabric_observatory/util.hpp"

#include <array>
#include <cstring>

namespace fabric_observatory {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t value, unsigned shift) noexcept {
  return (value >> shift) | (value << (32u - shift));
}

constexpr std::uint32_t choose(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (~x & z);
}

constexpr std::uint32_t majority(std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
  return (x & y) ^ (x & z) ^ (y & z);
}

constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
             0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u} {}

void Sha256::compress(const std::uint8_t* block) noexcept {
  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t index = 0; index < 16; ++index) {
    const std::size_t base = index * 4;
    schedule[index] = (static_cast<std::uint32_t>(block[base]) << 24u) |
                      (static_cast<std::uint32_t>(block[base + 1]) << 16u) |
                      (static_cast<std::uint32_t>(block[base + 2]) << 8u) |
                      static_cast<std::uint32_t>(block[base + 3]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    schedule[index] = small_sigma1(schedule[index - 2]) + schedule[index - 7] +
                      small_sigma0(schedule[index - 15]) + schedule[index - 16];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t temp1 =
        h + big_sigma1(e) + choose(e, f, g) + kRoundConstants[index] + schedule[index];
    const std::uint32_t temp2 = big_sigma0(a) + majority(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::append(const std::uint8_t* cursor, std::size_t size) noexcept {
  std::size_t remaining = size;
  total_bytes_ += static_cast<std::uint64_t>(remaining);

  if (buffered_ > 0) {
    const std::size_t needed = 64 - buffered_;
    const std::size_t take = remaining < needed ? remaining : needed;
    std::memcpy(buffer_.data() + buffered_, cursor, take);
    buffered_ += take;
    cursor += take;
    remaining -= take;
    if (buffered_ == 64) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }

  while (remaining >= 64) {
    compress(cursor);
    cursor += 64;
    remaining -= 64;
  }

  if (remaining > 0) {
    std::memcpy(buffer_.data(), cursor, remaining);
    buffered_ = remaining;
  }
}

void Sha256::update(ByteSpan data) noexcept {
  FABRIC_OBSERVATORY_CONTRACT(!finalized_);
  append(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

void Sha256::update(std::string_view data) noexcept {
  update(ByteSpan(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

void Sha256::update_byte(std::uint8_t value) noexcept {
  const std::byte single = static_cast<std::byte>(value);
  update(ByteSpan(&single, 1));
}

std::array<std::uint8_t, Sha256::kDigestBytes> Sha256::finish() noexcept {
  FABRIC_OBSERVATORY_CONTRACT(!finalized_);
  finalized_ = true;

  const std::uint64_t total_bits = total_bytes_ * 8u;
  std::array<std::uint8_t, 8> length_bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    length_bytes[7 - index] = static_cast<std::uint8_t>((total_bits >> (8u * index)) & 0xFFu);
  }

  // Padding is fed through the internal append so that the finalized_ guard is
  // not re-entered; the byte counter captured above is already complete.
  const std::uint8_t pad = 0x80;
  append(&pad, 1);
  const std::uint8_t zero = 0x00;
  while (buffered_ != 56) {
    append(&zero, 1);
  }
  append(length_bytes.data(), length_bytes.size());
  FABRIC_OBSERVATORY_CONTRACT(buffered_ == 0);

  std::array<std::uint8_t, kDigestBytes> digest{};
  for (std::size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<std::uint8_t>((state_[index] >> 24u) & 0xFFu);
    digest[index * 4 + 1] = static_cast<std::uint8_t>((state_[index] >> 16u) & 0xFFu);
    digest[index * 4 + 2] = static_cast<std::uint8_t>((state_[index] >> 8u) & 0xFFu);
    digest[index * 4 + 3] = static_cast<std::uint8_t>(state_[index] & 0xFFu);
  }
  return digest;
}

bool Digest256::is_zero() const noexcept {
  for (const std::uint8_t byte : bytes) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

std::string Digest256::to_hex() const {
  return util::hex_encode(ByteSpan(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
}

std::string Digest256::to_short_hex() const {
  const std::string full = to_hex();
  return full.substr(0, 12);
}

Result<Digest256> Digest256::from_hex(std::string_view text) {
  if (text.size() != 64) {
    return Err<Digest256>(StatusCode::InvalidArgument,
                          "digest hex text must be exactly 64 characters");
  }
  Result<std::vector<std::byte>> decoded = util::hex_decode(text);
  if (!decoded) {
    return Err<Digest256>(decoded.status().code(), decoded.status().message());
  }
  if (decoded->size() != 32) {
    return Err<Digest256>(StatusCode::InvalidArgument,
                          "digest must be exactly 32 bytes (64 hex characters)");
  }
  Digest256 digest;
  std::memcpy(digest.bytes.data(), decoded->data(), 32);
  return Ok(digest);
}

Digest256 sha256(ByteSpan data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return Digest256{hasher.finish()};
}

Digest256 sha256(std::string_view data) noexcept {
  Sha256 hasher;
  hasher.update(data);
  return Digest256{hasher.finish()};
}

namespace {

const std::array<std::uint32_t, 256>& crc32c_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> built{};
    for (std::uint32_t index = 0; index < 256; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? (value >> 1u) ^ 0x82F63B78u : (value >> 1u);
      }
      built[index] = value;
    }
    return built;
  }();
  return table;
}

}  // namespace

void Crc32c::update(ByteSpan data) noexcept {
  const std::array<std::uint32_t, 256>& table = crc32c_table();
  std::uint32_t state = state_;
  for (const std::byte raw : data) {
    const std::uint8_t byte = static_cast<std::uint8_t>(raw);
    state = table[(state ^ byte) & 0xFFu] ^ (state >> 8u);
  }
  state_ = state;
}

void Crc32c::update(std::string_view data) noexcept {
  update(ByteSpan(reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

std::uint32_t crc32c(ByteSpan data) noexcept {
  Crc32c hasher;
  hasher.update(data);
  return hasher.value();
}

std::uint32_t crc32c(std::string_view data) noexcept {
  Crc32c hasher;
  hasher.update(data);
  return hasher.value();
}

}  // namespace fabric_observatory
