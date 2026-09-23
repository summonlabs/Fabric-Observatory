// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/truth.hpp"

#include <array>

namespace fabric_observatory {

namespace {

constexpr std::array<std::string_view, kTruthStateCount> kTruthStateNames = {
    "unknown", "unsupported", "conflicting", "stale", "incomplete", "known"};

constexpr std::array<std::string_view, kFreshnessVerdictCount> kFreshnessNames = {
    "fresh", "aging", "stale"};

constexpr std::array<std::string_view, kSourceAuthorityCount> kAuthorityNames = {
    "none", "inferred", "reported", "corroborated", "authoritative"};

}  // namespace

std::string_view to_string(TruthState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  return index < kTruthStateCount ? kTruthStateNames[index] : std::string_view("unknown");
}

std::optional<TruthState> truth_state_from_string(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kTruthStateCount; ++index) {
    if (kTruthStateNames[index] == text) {
      return static_cast<TruthState>(index);
    }
  }
  return std::nullopt;
}

std::string_view to_string(FreshnessVerdict verdict) noexcept {
  const auto index = static_cast<std::size_t>(verdict);
  return index < kFreshnessVerdictCount ? kFreshnessNames[index] : std::string_view("stale");
}

std::optional<FreshnessVerdict> freshness_verdict_from_string(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kFreshnessVerdictCount; ++index) {
    if (kFreshnessNames[index] == text) {
      return static_cast<FreshnessVerdict>(index);
    }
  }
  return std::nullopt;
}

std::string_view to_string(SourceAuthority authority) noexcept {
  const auto index = static_cast<std::size_t>(authority);
  return index < kSourceAuthorityCount ? kAuthorityNames[index] : std::string_view("none");
}

std::optional<SourceAuthority> source_authority_from_string(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kSourceAuthorityCount; ++index) {
    if (kAuthorityNames[index] == text) {
      return static_cast<SourceAuthority>(index);
    }
  }
  return std::nullopt;
}

}  // namespace fabric_observatory
