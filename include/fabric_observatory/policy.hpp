// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_POLICY_HPP
#define FABRIC_OBSERVATORY_POLICY_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "fabric_observatory/aspect.hpp"
#include "fabric_observatory/digest.hpp"
#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/time.hpp"
#include "fabric_observatory/truth.hpp"

namespace fabric_observatory {

// Freshness is a pure function of the evaluation time carried by the snapshot
// and the receive time carried by the evidence. Nothing here reads a clock.
struct FreshnessPolicy {
  Duration fresh_window{Duration::from_seconds(30)};
  Duration aging_window{Duration::from_seconds(120)};
  Duration max_clock_skew{Duration::from_seconds(5)};

  // Persisted dynamic evidence must not silently become fresh after restart.
  // With the default, recovered evidence is capped at Aging for ever, no matter
  // how recently it was received before the restart. An operator who wants the
  // other behaviour must say so explicitly, and the policy digest recorded in
  // every snapshot makes the choice visible.
  bool allow_recovered_as_fresh{false};

  friend bool operator==(const FreshnessPolicy&, const FreshnessPolicy&) = default;
};

// How much independent, sufficiently authoritative, fresh evidence an aspect
// needs before the runtime will call it Known.
struct CoveragePolicy {
  std::uint32_t min_distinct_fresh_sources{1};
  SourceAuthority required_authority{SourceAuthority::None};

  friend bool operator==(const CoveragePolicy&, const CoveragePolicy&) = default;
};

struct TruthPolicy {
  CoveragePolicy default_coverage{};
  std::vector<std::pair<AspectId, CoveragePolicy>> overrides{};

  // Resolution order, highest priority first:
  //   1. an explicit override in this policy
  //   2. the well-known aspect descriptor
  //   3. default_coverage
  [[nodiscard]] CoveragePolicy coverage_for(std::string_view aspect) const;
  [[nodiscard]] Result<void> validate() const;

  friend bool operator==(const TruthPolicy&, const TruthPolicy&) = default;
};

struct IngestPolicy {
  // When set, an observation whose generation is below the fabric high-water
  // generation is refused at ingest. The default keeps such evidence and lets
  // composition mark it superseded, which is order independent; strict mode
  // trades order independence for immediate refusal and is recorded in the
  // policy digest.
  bool strict_generation_fence{false};
  // Refuse an observation that would advance the fabric generation by more than
  // this in one step. Bounds the effect of a hostile or broken source.
  std::uint64_t max_generation_advance{1000000};
  // Accept observations from sources that have not been registered explicitly.
  // The declared authority is then applied.
  bool auto_register_sources{true};
  SourceAuthority auto_register_authority{SourceAuthority::Reported};
  // Refuse claims for well-known aspects whose name is unknown to this build.
  // When set, only the well-known aspects of this build are accepted at all.
  // The default accepts any "<known domain>.<name>" aspect, which is what makes
  // the runtime vendor neutral; the flag exists for locked-down deployments and
  // is exercised by the tests.
  bool reject_unknown_aspects{false};
  // Retain refusals in the snapshot so that explanations can name them.
  bool retain_rejections{true};

  friend bool operator==(const IngestPolicy&, const IngestPolicy&) = default;
};

// The complete deterministic policy. Its digest is a component of snapshot
// identity, so two snapshots with the same evidence but different policies can
// never collide.
struct Policy {
  std::string name{"default"};
  std::uint32_t revision{1};
  FreshnessPolicy freshness{};
  TruthPolicy truth{};
  IngestPolicy ingest{};
  Limits limits{};

  [[nodiscard]] Result<void> validate() const;
  void encode(CanonicalEncoder& encoder) const;
  [[nodiscard]] Digest256 digest() const;
  [[nodiscard]] std::string canonical_text() const;

  friend bool operator==(const Policy&, const Policy&) = default;
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_POLICY_HPP
