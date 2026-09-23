// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_OBSERVATORY_HPP
#define FABRIC_OBSERVATORY_OBSERVATORY_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "fabric_observatory/diff.hpp"
#include "fabric_observatory/explanation.hpp"
#include "fabric_observatory/history.hpp"
#include "fabric_observatory/lock_audit.hpp"
#include "fabric_observatory/observation.hpp"
#include "fabric_observatory/persistence.hpp"
#include "fabric_observatory/policy.hpp"
#include "fabric_observatory/provenance.hpp"
#include "fabric_observatory/query.hpp"
#include "fabric_observatory/snapshot.hpp"
#include "fabric_observatory/version.hpp"

namespace fabric_observatory {

struct ObservatoryConfig {
  FabricId fabric{};
  Policy policy{};
  // Injected clock. Tests and benchmarks drive time explicitly so that no test
  // depends on wall clock progress.
  std::shared_ptr<Clock> clock{};
  // Empty means the runtime keeps evidence in memory only.
  std::string journal_path{};
  JournalOptions journal{};
  bool recover_on_open{true};

  friend bool operator==(const ObservatoryConfig&, const ObservatoryConfig&) = default;
};

enum class IngestDisposition : std::uint8_t {
  Accepted = 0,
  Duplicate = 1,
  Fenced = 2,
  Rejected = 3,
};

std::string_view to_string(IngestDisposition disposition) noexcept;

enum class IngestOptions : std::uint8_t {
  // Rebuild and publish the snapshot before returning. The published snapshot
  // always reflects the observation that was just accepted.
  PublishSnapshot = 0,
  // Mark the published snapshot stale and let the next current()/refresh()
  // rebuild it. Used by batch and benchmark paths.
  DeferSnapshot = 1,
};

struct IngestOutcome {
  IngestDisposition disposition{IngestDisposition::Rejected};
  StatusCode code{StatusCode::Ok};
  std::string explanation{};
  Digest256 observation_id{};
  SnapshotId snapshot{};
  std::uint32_t claims_accepted{0};
  std::uint32_t claims_fenced{0};

  friend bool operator==(const IngestOutcome&, const IngestOutcome&) = default;
};

struct IngestBatchReport {
  std::uint64_t accepted{0};
  std::uint64_t duplicates{0};
  std::uint64_t fenced{0};
  std::uint64_t rejected{0};
  std::vector<IngestOutcome> outcomes{};
  SnapshotId snapshot{};

  friend bool operator==(const IngestBatchReport&, const IngestBatchReport&) = default;
};

struct ObservatoryStats {
  std::uint64_t observations_accepted{0};
  std::uint64_t observations_duplicate{0};
  std::uint64_t observations_fenced{0};
  std::uint64_t observations_rejected{0};
  std::uint64_t claims_fenced{0};
  std::uint64_t evidence_evicted{0};
  std::uint64_t snapshots_published{0};
  std::uint64_t publications_skipped_unchanged{0};
  std::uint64_t journal_records{0};
  std::uint32_t journal_rotations{0};
  std::uint32_t sources{0};
  std::uint64_t evidence_records{0};
  std::uint32_t restart_count{0};
  bool published{false};
  bool dirty{false};

  friend bool operator==(const ObservatoryStats&, const ObservatoryStats&) = default;
};

// The runtime facade. It owns the authoritative evidence, the published
// immutable snapshot and the bounded history.
//
// Locking: one std::shared_mutex guards the evidence and the published pointer.
// Readers take it shared and return a shared_ptr to an immutable snapshot;
// writers take it exclusively. The journal lock (level 2) is only ever taken
// while the state lock (level 1) is held, which is the documented order.
class Observatory {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Observatory>> open(const ObservatoryConfig& config);
  ~Observatory();

  Observatory(const Observatory&) = delete;
  Observatory& operator=(const Observatory&) = delete;

  // Declares a source before it reports. The declaration is what bounds the
  // source: claims outside the declared aspect scope are fenced, and the
  // declared authority is the strongest authority its claims can carry.
  // Registering an unknown source twice replaces the declaration and keeps the
  // sequence and incarnation bookkeeping.
  Status register_source(const SourceDescriptor& descriptor);

  Status ingest(const Observation& observation, IngestOutcome& outcome,
                IngestOptions options = IngestOptions::PublishSnapshot);
  Status ingest_batch(std::span<const Observation> observations, IngestBatchReport& report,
                      IngestOptions options = IngestOptions::PublishSnapshot);

  // Publishing and reading the published snapshot may rebuild it, so these are
  // not const. Read-only accessors below are.
  [[nodiscard]] std::shared_ptr<const Snapshot> current();
  Status refresh(TimePoint evaluation_time, std::shared_ptr<const Snapshot>& out);
  Status publish(std::shared_ptr<const Snapshot>& out);

  [[nodiscard]] Result<QueryResult> query(const QueryFilter& filter);
  [[nodiscard]] Result<HierarchyResult> hierarchy(const HierarchyQuery& query);
  [[nodiscard]] Result<SnapshotDiff> diff(const SnapshotId& before, const SnapshotId& after);
  [[nodiscard]] Result<SnapshotDiff> diff_latest(std::uint32_t lookback);
  [[nodiscard]] Result<std::vector<HistoryEntry>> history(std::uint32_t max_entries) const;
  [[nodiscard]] Result<std::shared_ptr<const Snapshot>> snapshot_at(HistoryIndex index) const;
  [[nodiscard]] Result<std::shared_ptr<const Snapshot>> snapshot_by_id(const SnapshotId& id) const;
  [[nodiscard]] Result<Explanation> explain(const EntityRef& subject,
                                            std::optional<AspectId> aspect);
  [[nodiscard]] Result<std::vector<SourceSummary>> sources() const;
  [[nodiscard]] Result<std::vector<ObservationRecord>> retained_records(std::uint32_t limit) const;
  [[nodiscard]] Result<RecoveryReport> recovery() const;
  [[nodiscard]] ObservatoryStats stats() const;
  [[nodiscard]] std::string canonical_summary();

  [[nodiscard]] FabricId fabric() const noexcept { return config_.fabric; }
  [[nodiscard]] const Policy& policy() const noexcept { return config_.policy; }
  [[nodiscard]] RestartEpoch restart_epoch() const noexcept { return restart_epoch_; }

  // Flushes and closes the journal. Real: the file is synced and closed before
  // this returns.
  void shutdown();

 private:
  Observatory() = default;

  [[nodiscard]] Status register_source_locked(const SourceDescriptor& descriptor);
  [[nodiscard]] Status ingest_locked(const Observation& observation, IngestOutcome& outcome,
                                     IngestOptions options);
  void record_rejection_locked(const Observation& observation, TimePoint received_at,
                               StatusCode code, const std::string& explanation);
  void record_fenced_claim_locked(const Observation& observation, TimePoint received_at,
                                  const Claim& claim, StatusCode code,
                                  const std::string& explanation);
  void evict_evidence_locked();
  [[nodiscard]] std::vector<SourceSummary> source_summaries_locked(TimePoint evaluation_time) const;
  std::shared_ptr<const Snapshot> publish_locked(TimePoint evaluation_time);
  [[nodiscard]] std::uint64_t high_water_generation_locked() const;

  ObservatoryConfig config_{};
  std::shared_ptr<Clock> clock_{};
  mutable std::shared_mutex state_mutex_{};

  std::map<SourceId, SourceState> sources_{};
  std::map<SourceId, std::deque<IncarnationId>> incarnations_{};
  std::map<std::tuple<SourceId, EntityId, std::string>, ClaimRevision> revisions_{};
  std::vector<ObservationRecord> records_{};
  std::map<Digest256, std::size_t> record_index_{};
  std::vector<RejectedObservation> rejected_{};
  std::vector<FencedClaim> fenced_claims_{};
  History history_;
  std::shared_ptr<const Snapshot> published_{};
  bool dirty_{false};
  bool evidence_truncated_{false};
  bool journal_failed_{false};
  std::string journal_failure_{};
  std::unique_ptr<Journal> journal_{};
  RestartEpoch restart_epoch_{};
  std::uint32_t restart_count_{0};
  std::uint64_t evidence_evicted_{0};
  std::uint64_t snapshots_published_{0};
  std::uint64_t publications_skipped_{0};
  std::uint64_t observations_accepted_{0};
  std::uint64_t observations_duplicate_{0};
  std::uint64_t observations_fenced_{0};
  std::uint64_t observations_rejected_{0};
  std::uint64_t claims_fenced_{0};
  RecoveryReport recovery_{};
  bool shutdown_{false};
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_OBSERVATORY_HPP
