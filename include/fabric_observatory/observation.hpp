// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_OBSERVATION_HPP
#define FABRIC_OBSERVATORY_OBSERVATION_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fabric_observatory/aspect.hpp"
#include "fabric_observatory/canonical.hpp"
#include "fabric_observatory/digest.hpp"
#include "fabric_observatory/ids.hpp"
#include "fabric_observatory/json.hpp"
#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/status.hpp"
#include "fabric_observatory/time.hpp"
#include "fabric_observatory/value.hpp"
#include "fabric_observatory/version.hpp"

namespace fabric_observatory {

// A bounded, canonically ordered key/value string map used for source-supplied
// annotations. Keys are sorted and unique, so the encoding is canonical.
class Metadata {
 public:
  Metadata() = default;

  [[nodiscard]] static Result<Metadata> make(
      std::vector<std::pair<std::string, std::string>> entries, const Limits& limits);

  [[nodiscard]] const std::vector<std::pair<std::string, std::string>>& entries() const noexcept {
    return entries_;
  }
  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] const std::string* find(std::string_view key) const noexcept;

  friend bool operator==(const Metadata&, const Metadata&) = default;
  friend auto operator<=>(const Metadata&, const Metadata&) = default;

 private:
  std::vector<std::pair<std::string, std::string>> entries_{};
};

// A single assertion about one subject and one aspect, as stated by a source.
struct Claim {
  SubjectIdentity subject{};
  AspectId aspect{};
  Value value{};
  // A source may explicitly declare that it cannot supply this aspect. That is
  // evidence of absence of support, which is different from absence of
  // evidence, and it is preserved as such.
  bool supported{true};
  // Optional per-subject revision declared by the source. Zero means the source
  // does not version this claim.
  ClaimRevision revision{};

  friend bool operator==(const Claim&, const Claim&) = default;
  friend auto operator<=>(const Claim&, const Claim&) = default;
};

// How strongly one observation is related to another. The vocabulary is
// deliberately weaker than causation: this runtime records evidence of
// relationship and never asserts that one event caused another.
enum class CausalStrength : std::uint8_t {
  CorrelatesWith = 0,
  TemporallyPrecedes = 1,
  ContributoryEvidence = 2,
};

inline constexpr std::size_t kCausalStrengthCount = 3;

std::string_view to_string(CausalStrength strength) noexcept;
std::optional<CausalStrength> causal_strength_from_string(std::string_view text) noexcept;
std::string_view causality_disclaimer(CausalStrength strength) noexcept;

struct CausalRef {
  Digest256 antecedent{};
  CausalStrength strength{CausalStrength::CorrelatesWith};
  std::string basis{};

  friend bool operator==(const CausalRef&, const CausalRef&) = default;
  friend auto operator<=>(const CausalRef&, const CausalRef&) = default;
};

// The provenance-bearing unit of input. Everything needed to place a statement
// in time, in a generation and in a source lineage travels with it.
struct Observation {
  std::string schema{};
  FabricId fabric{};
  SourceId source{};
  IncarnationId incarnation{};
  SourceSequence sequence{};
  GenerationId generation{};
  EpochId epoch{};
  TimePoint observed_at{};
  std::vector<Claim> claims{};
  std::vector<CausalRef> causal{};
  Metadata metadata{};

  // Canonical form: claims and causal references are sorted, so two
  // observations carrying the same statements in a different order have the
  // same identity. Receive time is assigned by this runtime and is deliberately
  // not part of the identity.
  void canonicalize();
  void encode(CanonicalEncoder& encoder) const;
  // Exact inverse of encode(). Also verifies that the derived entity
  // identifiers agree with the typed identities carried alongside them.
  [[nodiscard]] static Result<Observation> decode(CanonicalDecoder& decoder, const Limits& limits);
  [[nodiscard]] Digest256 content_digest() const;
  [[nodiscard]] std::string to_json_text(bool pretty = false) const;
  [[nodiscard]] JsonValue to_json() const;

  [[nodiscard]] static Result<Observation> from_json(const JsonValue& json,
                                                     const JsonLimits& json_limits,
                                                     const Limits& limits);
  [[nodiscard]] static Result<Observation> from_json_text(std::string_view text,
                                                          const JsonLimits& json_limits,
                                                          const Limits& limits);

  [[nodiscard]] Result<void> validate(const Limits& limits) const;

  friend bool operator==(const Observation&, const Observation&) = default;
};

// An observation as accepted by this runtime, with the receive time assigned
// here and the recovery lineage made explicit.
struct ObservationRecord {
  Observation observation{};
  TimePoint received_at{};
  bool recovered{false};
  RestartEpoch recovery_epoch{};

  friend bool operator==(const ObservationRecord&, const ObservationRecord&) = default;
};

// An observation that was refused. Refusals are first-class evidence about the
// ingest path and are retained (bounded) so that "why is this aspect unknown"
// has an answer that is not a guess.
struct RejectedObservation {
  Digest256 observation_id{};
  SourceId source{};
  IncarnationId incarnation{};
  SourceSequence sequence{};
  GenerationId generation{};
  EpochId epoch{};
  TimePoint received_at{};
  StatusCode fence{StatusCode::InvalidArgument};
  std::string explanation{};

  friend bool operator==(const RejectedObservation&, const RejectedObservation&) = default;
  friend auto operator<=>(const RejectedObservation&, const RejectedObservation&) = default;
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_OBSERVATION_HPP
