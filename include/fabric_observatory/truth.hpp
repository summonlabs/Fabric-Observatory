// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_TRUTH_HPP
#define FABRIC_OBSERVATORY_TRUTH_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fabric_observatory {

// TruthState is the only vocabulary in which this runtime reports fabric state.
// The states are ordered from least to most informative on purpose: the
// evaluation order documented in docs/truth-model.md walks them in this order
// and stops at the first that applies. Only Known is a positive statement about
// the fabric; every other state is an exact description of what is missing.
enum class TruthState : std::uint8_t {
  Unknown = 0,        // no evidence was accepted at all
  Unsupported = 1,    // evidence exists but this runtime cannot interpret it
  Conflicting = 2,    // fresh evidence disagrees and the runtime will not pick a winner
  Stale = 3,          // evidence exists but is not fresh enough to assert
  Incomplete = 4,     // fresh evidence agrees, but coverage requirements are unmet
  Known = 5,          // fresh, agreed, sufficiently covered evidence
};

inline constexpr std::size_t kTruthStateCount = 6;

std::string_view to_string(TruthState state) noexcept;
std::optional<TruthState> truth_state_from_string(std::string_view text) noexcept;

// Absence of evidence is never positive evidence: only Known may be treated as
// an assertion about the fabric by a consumer.
[[nodiscard]] constexpr bool is_positive(TruthState state) noexcept {
  return state == TruthState::Known;
}

// Freshness of a single piece of evidence with respect to the snapshot's
// evaluation time. Freshness is per-record; truth state is per-aspect.
enum class FreshnessVerdict : std::uint8_t {
  Fresh = 0,  // within the fresh window
  Aging = 1,  // past the fresh window, within the aging window
  Stale = 2,  // past the aging window
};

inline constexpr std::size_t kFreshnessVerdictCount = 3;

std::string_view to_string(FreshnessVerdict verdict) noexcept;
std::optional<FreshnessVerdict> freshness_verdict_from_string(std::string_view text) noexcept;

[[nodiscard]] constexpr FreshnessVerdict degrade(FreshnessVerdict verdict, unsigned steps) noexcept {
  const auto value = static_cast<unsigned>(verdict) + steps;
  return value >= kFreshnessVerdictCount ? FreshnessVerdict::Stale
                                         : static_cast<FreshnessVerdict>(value);
}

// Declared strength of a source for a claim. A source that reports beyond its
// declared authority is fenced, never trusted more than it declared.
enum class SourceAuthority : std::uint8_t {
  None = 0,
  Inferred = 1,      // derived by the source, not directly observed
  Reported = 2,      // directly observed by the source's own sensors
  Corroborated = 3,  // independently confirmed by more than one observer
  Authoritative = 4, // the system that owns the fact (for example, link state ownership)
};

inline constexpr std::size_t kSourceAuthorityCount = 5;

std::string_view to_string(SourceAuthority authority) noexcept;
std::optional<SourceAuthority> source_authority_from_string(std::string_view text) noexcept;

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_TRUTH_HPP
