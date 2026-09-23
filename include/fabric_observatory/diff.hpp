// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_DIFF_HPP
#define FABRIC_OBSERVATORY_DIFF_HPP

#include <cstdint>
#include <optional>
#include <vector>

#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/snapshot.hpp"

namespace fabric_observatory {

struct AspectChange {
  SubjectIdentity subject{};
  AspectId aspect{};
  Domain domain{Domain::Identity};
  TruthState before{TruthState::Unknown};
  TruthState after{TruthState::Unknown};
  std::optional<Value> value_before{};
  std::optional<Value> value_after{};
  std::vector<SourceId> sources_added{};
  std::vector<SourceId> sources_removed{};
  bool freshness_changed{false};

  friend bool operator==(const AspectChange&, const AspectChange&) = default;
  friend auto operator<=>(const AspectChange&, const AspectChange&) = default;
};

struct SourceDelta {
  SourceId id{};
  bool added{false};
  bool removed{false};
  bool incarnation_changed{false};
  IncarnationId before_incarnation{};
  IncarnationId after_incarnation{};
  FreshnessVerdict before_freshness{FreshnessVerdict::Stale};
  FreshnessVerdict after_freshness{FreshnessVerdict::Stale};

  friend bool operator==(const SourceDelta&, const SourceDelta&) = default;
  friend auto operator<=>(const SourceDelta&, const SourceDelta&) = default;
};

struct SnapshotDiff {
  SnapshotId before_id{};
  SnapshotId after_id{};
  GenerationId before_generation{};
  GenerationId after_generation{};
  EpochId before_epoch{};
  EpochId after_epoch{};
  TimePoint before_evaluation{};
  TimePoint after_evaluation{};
  bool generation_changed{false};
  bool epoch_changed{false};
  bool restart_observed{false};
  std::uint32_t restarts_between{0};
  std::vector<SubjectIdentity> subjects_added{};
  std::vector<SubjectIdentity> subjects_removed{};
  std::vector<AspectChange> changes{};
  std::vector<SourceDelta> source_changes{};
  bool truncated{false};
  Digest256 digest{};

  [[nodiscard]] std::string canonical_summary() const;
};

Result<SnapshotDiff> diff_snapshots(const Snapshot& before, const Snapshot& after,
                                    const Limits& limits);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_DIFF_HPP
