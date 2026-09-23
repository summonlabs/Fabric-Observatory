// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_SNAPSHOT_HPP
#define FABRIC_OBSERVATORY_SNAPSHOT_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_observatory/ids.hpp"
#include "fabric_observatory/observation.hpp"
#include "fabric_observatory/policy.hpp"
#include "fabric_observatory/provenance.hpp"
#include "fabric_observatory/truth.hpp"

namespace fabric_observatory {

// Snapshot identity is the SHA-256 of the canonical encoding of everything the
// snapshot asserts. Two snapshots with the same fabric, generation, epoch,
// evaluation time, policy and evidence have the same identity, whatever order
// the evidence arrived in and whichever thread built them. Publication time and
// history position are not part of it.
class SnapshotId {
 public:
  SnapshotId() = default;
  explicit SnapshotId(Digest256 digest) noexcept : digest_(digest) {}

  [[nodiscard]] const Digest256& digest() const noexcept { return digest_; }
  [[nodiscard]] bool is_zero() const noexcept { return digest_.is_zero(); }
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] static Result<SnapshotId> from_text(std::string_view text);

  friend bool operator==(const SnapshotId&, const SnapshotId&) = default;
  friend auto operator<=>(const SnapshotId&, const SnapshotId&) = default;

 private:
  Digest256 digest_{};
};


// One retained statement about a subject and aspect, with its full provenance
// and its evidence classification.
struct ClaimRecord {
  SourceId source{};
  IncarnationId incarnation{};
  SourceAuthority authority{SourceAuthority::None};
  SourceSequence sequence{};
  ClaimRevision revision{};
  Value value{};
  bool supported{true};
  FreshnessVerdict freshness{FreshnessVerdict::Stale};
  TimePoint observed_at{};
  TimePoint received_at{};
  bool recovered{false};
  RestartEpoch recovery_epoch{};
  // Evidence that was accepted but is not current for this aspect.
  bool superseded_generation{false};
  bool superseded_epoch{false};
  bool superseded_incarnation{false};
  bool superseded_by_later_sequence{false};
  // True when this claim was refused by an ingest fence (out of the source's
  // declared scope, or a replayed revision). The claim stays visible as
  // evidence of what the source said, and it can never support an assertion.
  bool fenced{false};
  StatusCode fence{StatusCode::Ok};
  Digest256 observation_id{};

  [[nodiscard]] bool is_current() const noexcept {
    return !superseded_generation && !superseded_epoch && !superseded_incarnation &&
           !superseded_by_later_sequence && !fenced;
  }
  // Evidence that may support a positive statement.
  [[nodiscard]] bool is_assertive() const noexcept {
    return supported && is_current() && freshness == FreshnessVerdict::Fresh;
  }

  friend bool operator==(const ClaimRecord&, const ClaimRecord&) = default;
  friend auto operator<=>(const ClaimRecord&, const ClaimRecord&) = default;
};

struct ConflictGroup {
  Value value{};
  std::vector<SourceId> sources{};
  std::uint32_t fresh_sources{0};

  friend bool operator==(const ConflictGroup&, const ConflictGroup&) = default;
  friend auto operator<=>(const ConflictGroup&, const ConflictGroup&) = default;
};

struct CoverageReport {
  std::uint32_t distinct_fresh_sources{0};
  std::uint32_t required_distinct_fresh_sources{1};
  SourceAuthority best_fresh_authority{SourceAuthority::None};
  SourceAuthority required_authority{SourceAuthority::None};
  bool satisfied{false};

  friend bool operator==(const CoverageReport&, const CoverageReport&) = default;
  friend auto operator<=>(const CoverageReport&, const CoverageReport&) = default;
};

struct AspectState {
  AspectId aspect{};
  Domain domain{Domain::Identity};
  TruthState truth{TruthState::Unknown};
  std::optional<Value> agreed_value{};
  std::vector<ClaimRecord> claims{};
  std::vector<ConflictGroup> conflicts{};
  CoverageReport coverage{};
  // True when the retained claim list was cut short by the configured bound.
  bool claims_truncated{false};

  friend bool operator==(const AspectState&, const AspectState&) = default;
  friend auto operator<=>(const AspectState&, const AspectState&) = default;
};

struct SubjectState {
  SubjectIdentity identity{};
  std::optional<EntityId> parent{};
  TruthState topology_truth{TruthState::Unknown};
  // The least informative truth across the subject's aspects. Consumers that
  // need a single word for a subject get the pessimistic one.
  TruthState worst_truth{TruthState::Known};
  std::vector<AspectState> aspects{};

  friend bool operator==(const SubjectState&, const SubjectState&) = default;
  friend auto operator<=>(const SubjectState&, const SubjectState&) = default;
};

struct SourceSummary {
  SourceId id{};
  std::string name{};
  std::string authority_name{};
  SourceAuthority max_authority{SourceAuthority::None};
  SourceIncarnation incarnation{};
  SourceSequence last_sequence{};
  TimePoint first_receive{};
  TimePoint last_receive{};
  TimePoint last_observation{};
  GenerationId last_generation{};
  EpochId last_epoch{};
  // Accepted observations and how many of them are still retained. Refusal
  // counters deliberately live in ObservatoryStats instead: they count
  // transport events, and a retried stream must not change the snapshot.
  std::uint64_t accepted{0};
  std::uint64_t retained_records{0};
  FreshnessVerdict freshness{FreshnessVerdict::Stale};
  bool recovered{false};

  friend bool operator==(const SourceSummary&, const SourceSummary&) = default;
  friend auto operator<=>(const SourceSummary&, const SourceSummary&) = default;
};

// A claim that was refused before it could become evidence.
struct FencedClaim {
  Digest256 observation_id{};
  SubjectIdentity subject{};
  AspectId aspect{};
  SourceId source{};
  IncarnationId incarnation{};
  SourceSequence sequence{};
  TimePoint received_at{};
  StatusCode fence{StatusCode::FencedAuthority};
  std::string explanation{};

  friend bool operator==(const FencedClaim&, const FencedClaim&) = default;
  friend auto operator<=>(const FencedClaim&, const FencedClaim&) = default;
};

struct SnapshotStats {
  std::uint64_t subjects{0};
  std::uint64_t aspects{0};
  std::uint64_t claims{0};
  std::uint64_t conflicts{0};
  std::uint64_t sources{0};
  std::uint64_t rejected_observations{0};
  std::uint64_t fenced_claims{0};
  std::uint64_t superseded_claims{0};
  std::uint64_t recovered_claims{0};
  std::uint64_t unsupported_claims{0};
  std::uint64_t subjects_by_kind[kEntityKindCount]{};
  std::uint64_t aspects_by_truth[kTruthStateCount]{};

  friend bool operator==(const SnapshotStats&, const SnapshotStats&) = default;
  friend auto operator<=>(const SnapshotStats&, const SnapshotStats&) = default;
};

struct SnapshotBuildRequest {
  FabricId fabric{};
  Policy policy{};
  TimePoint evaluation_time{};
  RestartEpoch restart_epoch{};
  std::uint32_t restart_count{0};
  std::vector<ObservationRecord> records{};
  std::vector<RejectedObservation> rejected{};
  std::vector<FencedClaim> fenced_claims{};
  std::vector<SourceSummary> sources{};
  // Bounds on how much of the evidence set is reflected in the snapshot.
  bool evidence_truncated{false};
};

// An immutable, canonically ordered, generation-bound view of the fabric.
// Snapshot instances are never mutated after construction; readers hold a
// shared_ptr<const Snapshot>, so a query can never observe a partially applied
// update.
class Snapshot {
 public:
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;

  [[nodiscard]] static std::shared_ptr<const Snapshot> build(const SnapshotBuildRequest& request);

  [[nodiscard]] const SnapshotId& id() const noexcept { return id_; }
  [[nodiscard]] const Digest256& digest() const noexcept { return id_.digest(); }
  [[nodiscard]] FabricId fabric() const noexcept { return fabric_; }
  [[nodiscard]] GenerationId generation() const noexcept { return generation_; }
  [[nodiscard]] EpochId epoch() const noexcept { return epoch_; }
  [[nodiscard]] TimePoint evaluation_time() const noexcept { return evaluation_time_; }
  [[nodiscard]] TimePoint observed_high_water() const noexcept { return observed_high_water_; }
  [[nodiscard]] TimePoint received_high_water() const noexcept { return received_high_water_; }
  [[nodiscard]] TimePoint received_low_water() const noexcept { return received_low_water_; }
  [[nodiscard]] const Digest256& policy_digest() const noexcept { return policy_digest_; }
  [[nodiscard]] const std::string& policy_name() const noexcept { return policy_name_; }
  [[nodiscard]] RestartEpoch restart_epoch() const noexcept { return restart_epoch_; }
  [[nodiscard]] std::uint32_t restart_count() const noexcept { return restart_count_; }
  [[nodiscard]] const std::vector<SubjectState>& subjects() const noexcept { return subjects_; }
  [[nodiscard]] const std::vector<SourceSummary>& sources() const noexcept { return sources_; }
  [[nodiscard]] const std::vector<RejectedObservation>& rejected() const noexcept {
    return rejected_;
  }
  [[nodiscard]] const std::vector<FencedClaim>& fenced_claims() const noexcept {
    return fenced_claims_;
  }
  [[nodiscard]] const SnapshotStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::uint64_t evidence_records() const noexcept { return evidence_records_; }
  [[nodiscard]] bool evidence_truncated() const noexcept { return evidence_truncated_; }
  [[nodiscard]] bool rejected_truncated() const noexcept { return rejected_truncated_; }

  // Publication bookkeeping, assigned by the registry and deliberately excluded
  // from the snapshot digest.
  [[nodiscard]] HistoryIndex history_index() const noexcept { return history_index_; }
  [[nodiscard]] TimePoint published_at() const noexcept { return published_at_; }
  void set_publication(HistoryIndex index, TimePoint published_at) const noexcept {
    history_index_ = index;
    published_at_ = published_at;
  }

  [[nodiscard]] const SubjectState* find_subject(const EntityRef& ref) const noexcept;
  [[nodiscard]] const AspectState* find_aspect(const EntityRef& ref,
                                               std::string_view aspect) const noexcept;
  [[nodiscard]] const SourceSummary* find_source(const SourceId& id) const noexcept;
  void encode_content(CanonicalEncoder& encoder) const;

  // Deterministic, stable, human readable rendering of the whole snapshot.
  [[nodiscard]] std::string canonical_summary() const;
  [[nodiscard]] Digest256 recompute_digest() const;

 private:
  Snapshot() = default;
  void compute_digest();

  SnapshotId id_{};
  FabricId fabric_{};
  GenerationId generation_{};
  EpochId epoch_{};
  TimePoint evaluation_time_{};
  TimePoint observed_high_water_{};
  TimePoint received_high_water_{};
  TimePoint received_low_water_{};
  Digest256 policy_digest_{};
  std::string policy_name_{};
  RestartEpoch restart_epoch_{};
  std::uint32_t restart_count_{0};
  std::vector<SubjectState> subjects_{};
  std::vector<SourceSummary> sources_{};
  std::vector<RejectedObservation> rejected_{};
  std::vector<FencedClaim> fenced_claims_{};
  SnapshotStats stats_{};
  std::uint64_t evidence_records_{0};
  bool evidence_truncated_{false};
  bool rejected_truncated_{false};

  mutable HistoryIndex history_index_{};
  mutable TimePoint published_at_{};
};

// The instant from which the age of a piece of evidence is measured: the
// earlier of when it was observed and when it was received. A delayed
// observation therefore can never look fresher than the fact it reports, and an
// observation whose source clock lags is treated conservatively rather than
// generously.
[[nodiscard]] constexpr TimePoint evidence_reference_time(TimePoint observed_at,
                                                          TimePoint received_at) noexcept {
  return observed_at < received_at ? observed_at : received_at;
}

// Freshness of one piece of evidence against an evaluation time. This is a pure
// function: same inputs, same verdict, on every machine and in every run.
[[nodiscard]] FreshnessVerdict evaluate_freshness(TimePoint evidence_time,
                                                 TimePoint evaluation_time,
                                                 const FreshnessPolicy& policy,
                                                 bool recovered);

// Truth state of an aspect given its retained claims. Exposed because it is the
// single most important behaviour of the runtime and is tested directly.
struct TruthEvaluation {
  TruthState truth{TruthState::Unknown};
  std::optional<Value> agreed_value{};
  std::vector<ConflictGroup> conflicts{};
  CoverageReport coverage{};
};

[[nodiscard]] TruthEvaluation evaluate_truth(const AspectId& aspect,
                                             const std::vector<ClaimRecord>& claims,
                                             const TruthPolicy& policy);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_SNAPSHOT_HPP
