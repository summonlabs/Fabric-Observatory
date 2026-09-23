// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/observatory.hpp"

#include "fabric_observatory/canonical.hpp"
#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/util.hpp"
#include "fabric_observatory/version.hpp"

#include <algorithm>
#include <sstream>

namespace fabric_observatory {

namespace {

constexpr std::size_t kMaxExplanationText = 512;

std::string bounded(std::string text) {
  if (text.size() <= kMaxExplanationText) {
    return text;
  }
  text.resize(kMaxExplanationText);
  return text;
}

void push_bounded(std::vector<RejectedObservation>& target, RejectedObservation value,
                  std::size_t bound) {
  target.push_back(std::move(value));
  if (target.size() > bound) {
    target.erase(target.begin());
  }
}

void push_bounded(std::vector<FencedClaim>& target, FencedClaim value, std::size_t bound) {
  target.push_back(std::move(value));
  if (target.size() > bound) {
    target.erase(target.begin());
  }
}

}  // namespace

std::string_view to_string(IngestDisposition disposition) noexcept {
  switch (disposition) {
    case IngestDisposition::Accepted:
      return "accepted";
    case IngestDisposition::Duplicate:
      return "duplicate";
    case IngestDisposition::Fenced:
      return "fenced";
    case IngestDisposition::Rejected:
      return "rejected";
  }
  return "rejected";
}

Result<std::unique_ptr<Observatory>> Observatory::open(const ObservatoryConfig& config) {
  if (config.fabric.is_nil()) {
    return Err<std::unique_ptr<Observatory>>(StatusCode::InvalidArgument,
                                             "an observatory requires a fabric identity");
  }
  Result<void> valid = config.policy.validate();
  if (!valid) {
    return Err<std::unique_ptr<Observatory>>(valid.status().code(), valid.status().message());
  }

  std::unique_ptr<Observatory> observatory(new Observatory());
  observatory->config_ = config;
  observatory->clock_ = config.clock != nullptr ? config.clock : std::make_shared<SystemClock>();
  observatory->history_.reset(config.policy.limits.max_history_snapshots);
  observatory->restart_epoch_ = RestartEpoch(1);
  observatory->restart_count_ = 0;

  if (!config.journal_path.empty()) {
    JournalOptions options = config.journal;
    options.fabric = config.fabric;
    options.limits = config.policy.limits;
    Result<std::unique_ptr<Journal>> journal = Journal::open(config.journal_path, options);
    if (!journal) {
      return Err<std::unique_ptr<Observatory>>(journal.status().code(), journal.status().message());
    }
    observatory->journal_ = std::move(*journal);
    observatory->recovery_ = observatory->journal_->recovery();

    if (config.recover_on_open && !observatory->journal_->recovered_records().empty()) {
      // Conservative restart. Every recovered record is marked as recovered at
      // the new restart epoch before it becomes evidence again, which is what
      // stops persisted dynamic state from silently becoming fresh.
      observatory->restart_count_ = observatory->journal_->recovered_restart_count() + 1;
      observatory->restart_epoch_ =
          RestartEpoch(observatory->journal_->recovered_restart_epoch().value() + 1);
      for (const ObservationRecord& recovered : observatory->journal_->recovered_records()) {
        ObservationRecord record = recovered;
        record.recovered = true;
        record.recovery_epoch = observatory->restart_epoch_;
        const Digest256 digest = record.observation.content_digest();
        observatory->record_index_.emplace(digest, observatory->records_.size());
        observatory->records_.push_back(std::move(record));
      }

      // Rebuild per-source bookkeeping deterministically from the recovered
      // evidence, in the canonical record order, so that post-restart fencing
      // behaves exactly as it did before the restart.
      std::vector<ObservationRecord> ordered = observatory->records_;
      std::sort(ordered.begin(), ordered.end(),
                [](const ObservationRecord& lhs, const ObservationRecord& rhs) {
                  if (lhs.observation.source != rhs.observation.source) {
                    return lhs.observation.source < rhs.observation.source;
                  }
                  if (lhs.observation.incarnation != rhs.observation.incarnation) {
                    return lhs.observation.incarnation < rhs.observation.incarnation;
                  }
                  return lhs.observation.sequence < rhs.observation.sequence;
                });
      for (const ObservationRecord& record : ordered) {
        const Observation& observation = record.observation;
        SourceState& state = observatory->sources_[observation.source];
        if (state.descriptor.id.is_nil()) {
          state.descriptor.id = observation.source;
          state.descriptor.name = observation.source.to_text();
          state.descriptor.schema = observation.schema;
          state.descriptor.max_authority = config.policy.ingest.auto_register_authority;
        }
        if (!(state.incarnation.id == observation.incarnation)) {
          state.incarnation.id = observation.incarnation;
          state.incarnation.boot_time = record.received_at;
          state.incarnation.boot_counter = 0;
          auto& seen = observatory->incarnations_[observation.source];
          if (std::find(seen.begin(), seen.end(), observation.incarnation) == seen.end()) {
            seen.push_back(observation.incarnation);
            while (seen.size() > config.policy.limits.max_incarnations_per_source) {
              seen.pop_front();
            }
          }
        }
        if (observation.sequence > state.last_sequence) {
          state.last_sequence = observation.sequence;
        }
        if (observation.generation > state.last_generation) {
          state.last_generation = observation.generation;
        }
        if (observation.generation == state.last_generation && observation.epoch > state.last_epoch) {
          state.last_epoch = observation.epoch;
        }
        if (state.first_receive.nanos == 0 || record.received_at < state.first_receive) {
          state.first_receive = record.received_at;
        }
        if (record.received_at > state.last_receive) {
          state.last_receive = record.received_at;
        }
        if (observation.observed_at > state.last_observation) {
          state.last_observation = observation.observed_at;
        }
        ++state.accepted;
        state.recovered = true;
        state.recovery_epoch = observatory->restart_epoch_;
      }
    } else if (config.recover_on_open) {
      observatory->restart_count_ = observatory->journal_->recovered_restart_count();
      observatory->restart_epoch_ =
          RestartEpoch(observatory->journal_->recovered_restart_epoch().value() + 1);
    }

    Result<void> marker = observatory->journal_->append_restart_marker(
        observatory->restart_epoch_, observatory->restart_count_, observatory->clock_->now());
    if (!marker) {
      observatory->journal_failed_ = true;
      observatory->journal_failure_ = marker.status().message();
    }
  }

  // Opening publishes the initial view, so current() is total from the moment
  // open() succeeds: a caller never has to handle "there is no snapshot yet".
  observatory->publish_locked(observatory->clock_->now());
  return Ok(std::move(observatory));
}

Observatory::~Observatory() { shutdown(); }

void Observatory::shutdown() {
  std::unique_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  if (journal_ != nullptr) {
    LockOrderGuard persistence(LockLevel::Persistence);
    journal_->close();
  }
}

void Observatory::record_rejection_locked(const Observation& observation, TimePoint received_at,
                                          StatusCode code, const std::string& explanation) {
  RejectedObservation rejection;
  rejection.observation_id = observation.content_digest();
  rejection.source = observation.source;
  rejection.incarnation = observation.incarnation;
  rejection.sequence = observation.sequence;
  rejection.generation = observation.generation;
  rejection.epoch = observation.epoch;
  rejection.received_at = received_at;
  rejection.fence = code;
  rejection.explanation = bounded(explanation);
  if (!config_.policy.ingest.retain_rejections) {
    return;
  }
  // Refusals are idempotent and they only ever describe evidence that was not
  // accepted:
  //   * content that was accepted elsewhere is already visible, so a replay of
  //     it records nothing new;
  //   * the first refusal of an unaccepted observation is the recorded fact, and
  //     refusing the same content again records nothing new either.
  // Together these make retrying a stream unable to move the published view.
  // Retry counts are telemetry and live in the runtime counters.
  if (record_index_.find(rejection.observation_id) != record_index_.end()) {
    return;
  }
  for (const RejectedObservation& existing : rejected_) {
    if (existing.observation_id == rejection.observation_id) {
      return;
    }
  }
  push_bounded(rejected_, std::move(rejection), config_.policy.limits.max_rejections_retained);
}

void Observatory::record_fenced_claim_locked(const Observation& observation, TimePoint received_at,
                                             const Claim& claim, StatusCode code,
                                             const std::string& explanation) {
  FencedClaim fenced;
  fenced.observation_id = observation.content_digest();
  fenced.subject = claim.subject;
  fenced.aspect = claim.aspect;
  fenced.source = observation.source;
  fenced.incarnation = observation.incarnation;
  fenced.sequence = observation.sequence;
  fenced.received_at = received_at;
  fenced.fence = code;
  fenced.explanation = bounded(explanation);
  ++claims_fenced_;
  for (const FencedClaim& existing : fenced_claims_) {
    if (existing.observation_id == fenced.observation_id && existing.subject.id == fenced.subject.id &&
        existing.aspect == fenced.aspect) {
      return;
    }
  }
  push_bounded(fenced_claims_, std::move(fenced), config_.policy.limits.max_rejections_retained);
}

void Observatory::evict_evidence_locked() {
  const std::size_t bound = config_.policy.limits.max_evidence_records;
  if (records_.size() <= bound) {
    return;
  }
  // Deterministic eviction: the oldest received evidence goes first, with a
  // total order on the remaining fields so that the choice never depends on
  // insertion order.
  std::vector<std::size_t> order(records_.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    order[index] = index;
  }
  std::sort(order.begin(), order.end(), [this](std::size_t lhs, std::size_t rhs) {
    const ObservationRecord& left = records_[lhs];
    const ObservationRecord& right = records_[rhs];
    if (left.received_at != right.received_at) {
      return left.received_at < right.received_at;
    }
    if (left.observation.source != right.observation.source) {
      return left.observation.source < right.observation.source;
    }
    if (left.observation.sequence != right.observation.sequence) {
      return left.observation.sequence < right.observation.sequence;
    }
    return left.observation.content_digest() < right.observation.content_digest();
  });
  // Drop in batches. Sorting once per inserted record would make the store
  // quadratic once it is full; dropping a sixteenth of the bound amortizes it.
  // The bound is a maximum, so dropping a little more than the overflow is
  // within contract.
  std::size_t drop = records_.size() - bound;
  const std::size_t batch = bound / 16;
  if (drop < batch && records_.size() > drop) {
    drop = std::min(records_.size(), batch);
  }
  std::vector<bool> dropped(records_.size(), false);
  for (std::size_t index = 0; index < drop; ++index) {
    dropped[order[index]] = true;
  }
  std::vector<ObservationRecord> kept;
  kept.reserve(bound);
  for (std::size_t index = 0; index < records_.size(); ++index) {
    if (!dropped[index]) {
      kept.push_back(std::move(records_[index]));
    }
  }
  records_ = std::move(kept);
  record_index_.clear();
  for (std::size_t index = 0; index < records_.size(); ++index) {
    record_index_.emplace(records_[index].observation.content_digest(), index);
  }
  evidence_evicted_ += drop;
  evidence_truncated_ = true;
}

std::uint64_t Observatory::high_water_generation_locked() const {
  std::uint64_t high = 0;
  for (const ObservationRecord& record : records_) {
    high = std::max(high, record.observation.generation.value());
  }
  return high;
}

std::vector<SourceSummary> Observatory::source_summaries_locked(TimePoint evaluation_time) const {
  std::map<SourceId, std::uint64_t> retained;
  for (const ObservationRecord& record : records_) {
    ++retained[record.observation.source];
  }
  std::vector<SourceSummary> summaries;
  summaries.reserve(sources_.size());
  for (const auto& entry : sources_) {
    const SourceState& state = entry.second;
    SourceSummary summary;
    summary.id = state.descriptor.id;
    summary.name = state.descriptor.name;
    summary.authority_name = state.descriptor.authority_name;
    summary.max_authority = state.descriptor.max_authority;
    summary.incarnation = state.incarnation;
    summary.last_sequence = state.last_sequence;
    summary.first_receive = state.first_receive;
    summary.last_receive = state.last_receive;
    summary.last_observation = state.last_observation;
    summary.last_generation = state.last_generation;
    summary.last_epoch = state.last_epoch;
    summary.accepted = state.accepted;
    const auto count = retained.find(state.descriptor.id);
    summary.retained_records = count == retained.end() ? 0 : count->second;
    summary.freshness = evaluate_freshness(
        evidence_reference_time(state.last_observation, state.last_receive), evaluation_time,
        config_.policy.freshness, state.recovered);
    summary.recovered = state.recovered;
    summaries.push_back(std::move(summary));
  }
  return summaries;
}

std::shared_ptr<const Snapshot> Observatory::publish_locked(TimePoint evaluation_time) {
  SnapshotBuildRequest request;
  request.fabric = config_.fabric;
  request.policy = config_.policy;
  request.evaluation_time = evaluation_time;
  request.restart_epoch = restart_epoch_;
  request.restart_count = restart_count_;
  request.records = records_;
  request.rejected = rejected_;
  request.fenced_claims = fenced_claims_;
  request.sources = source_summaries_locked(evaluation_time);
  request.evidence_truncated = evidence_truncated_;

  std::shared_ptr<const Snapshot> built = Snapshot::build(request);
  if (published_ != nullptr && published_->id() == built->id()) {
    ++publications_skipped_;
    dirty_ = false;
    return published_;
  }

  history_.append(built, clock_->now());
  if (journal_ != nullptr && !journal_failed_) {
    LockOrderGuard persistence(LockLevel::Persistence);
    const SnapshotMarker marker{built->id(), built->generation(), built->epoch(),
                                built->published_at()};
    Result<void> written = journal_->append_snapshot_marker(marker);
    if (!written) {
      journal_failed_ = true;
      journal_failure_ = written.status().message();
    }
  }
  published_ = built;
  dirty_ = false;
  ++snapshots_published_;
  return published_;
}

Status Observatory::register_source_locked(const SourceDescriptor& descriptor) {
  if (descriptor.id.is_nil()) {
    return Status::error(StatusCode::InvalidArgument, "a source declaration requires an identity");
  }
  if (descriptor.name.size() > 128) {
    return Status::error(StatusCode::TooLarge, "source name exceeds the configured length");
  }
  if (!descriptor.authority_name.empty() &&
      !util::is_valid_authority_name(descriptor.authority_name)) {
    return Status::error(StatusCode::InvalidArgument,
                         "source authority name contains unsupported characters");
  }
  if (descriptor.aspects.size() > config_.policy.limits.max_aspects_per_subject) {
    return Status::error(StatusCode::TooLarge,
                         "source declares more aspects than the configured maximum");
  }
  std::vector<AspectId> aspects = descriptor.aspects;
  std::sort(aspects.begin(), aspects.end());
  for (std::size_t index = 1; index < aspects.size(); ++index) {
    if (aspects[index - 1] == aspects[index]) {
      return Status::error(StatusCode::InvalidArgument,
                           "source declares the same aspect more than once");
    }
  }

  auto found = sources_.find(descriptor.id);
  if (found == sources_.end()) {
    if (sources_.size() >= config_.policy.limits.max_sources) {
      return Status::error(StatusCode::ResourceExhausted,
                           "the configured source bound has been reached; no new sources are "
                           "accepted");
    }
    SourceState fresh;
    fresh.descriptor = descriptor;
    fresh.descriptor.aspects = std::move(aspects);
    sources_.emplace(descriptor.id, std::move(fresh));
    return Status{};
  }

  // The declaration is replaced; the lineage is not.
  SourceState& state = found->second;
  const SourceIncarnation incarnation = state.incarnation;
  const SourceSequence sequence = state.last_sequence;
  const GenerationId generation = state.last_generation;
  const EpochId epoch = state.last_epoch;
  state.descriptor = descriptor;
  state.descriptor.aspects = std::move(aspects);
  state.incarnation = incarnation;
  state.last_sequence = sequence;
  state.last_generation = generation;
  state.last_epoch = epoch;
  return Status{};
}

Status Observatory::register_source(const SourceDescriptor& descriptor) {
  std::unique_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  return register_source_locked(descriptor);
}

Status Observatory::ingest_locked(const Observation& observation, IngestOutcome& outcome,
                                  IngestOptions options) {
  const TimePoint received_at = clock_->now();
  outcome = IngestOutcome{};
  outcome.observation_id = observation.content_digest();

  const auto finish = [&](StatusCode code, std::string message) {
    outcome.disposition = IngestDisposition::Rejected;
    outcome.code = code;
    outcome.explanation = bounded(std::move(message));
    ++observations_rejected_;
    record_rejection_locked(observation, received_at, code, outcome.explanation);
    if (options == IngestOptions::PublishSnapshot) {
      outcome.snapshot = publish_locked(received_at)->id();
    } else {
      dirty_ = true;
    }
    return Status::error(code, outcome.explanation);
  };

  Result<void> valid = observation.validate(config_.policy.limits);
  if (!valid) {
    return finish(valid.status().code(), valid.status().message());
  }
  if (observation.schema != std::string(observation_schema())) {
    return finish(StatusCode::Unsupported,
                  "observation schema '" + observation.schema +
                      "' is not supported by this build (expected '" +
                      std::string(observation_schema()) + "')");
  }
  if (!(observation.fabric == config_.fabric)) {
    return finish(StatusCode::FabricMismatch,
                  "observation is for fabric " + observation.fabric.to_text() +
                      ", this runtime observes " + config_.fabric.to_text());
  }
  if (time_after(observation.observed_at, received_at)) {
    const Duration skew = saturating_difference(observation.observed_at, received_at);
    if (skew > config_.policy.freshness.max_clock_skew) {
      return finish(StatusCode::ClockInconsistent,
                    "observation claims to have been observed " + std::to_string(skew.nanos) +
                        " ns in the future, beyond the configured clock skew tolerance");
    }
  }

  auto source_slot = sources_.find(observation.source);
  if (source_slot == sources_.end()) {
    if (!config_.policy.ingest.auto_register_sources) {
      return finish(StatusCode::SourceUnknown,
                    "source " + observation.source.to_text() +
                        " is not registered and automatic registration is disabled");
    }
    SourceDescriptor descriptor;
    descriptor.id = observation.source;
    descriptor.name = observation.source.to_text();
    descriptor.schema = observation.schema;
    descriptor.max_authority = config_.policy.ingest.auto_register_authority;
    const Status registered = register_source_locked(descriptor);
    if (!registered.ok()) {
      return finish(registered.code(), registered.message());
    }
    source_slot = sources_.find(observation.source);
    FABRIC_OBSERVATORY_CONTRACT(source_slot != sources_.end());
  }
  SourceState& source = source_slot->second;

  const bool new_incarnation = !(source.incarnation.id == observation.incarnation);
  if (new_incarnation) {
    auto& seen = incarnations_[observation.source];
    if (std::find(seen.begin(), seen.end(), observation.incarnation) != seen.end()) {
      ++source.fenced;
      return finish(StatusCode::FencedIncarnation,
                    "incarnation " + observation.incarnation.to_text() +
                        " was already superseded for source " + observation.source.to_text() +
                        "; a replayed boot is refused");
    }
  } else if (observation.sequence <= source.last_sequence) {
    const Digest256 digest = observation.content_digest();
    const auto previous = record_index_.find(digest);
    if (observation.sequence == source.last_sequence && previous != record_index_.end()) {
      ++source.duplicates;
      ++observations_duplicate_;
      outcome.disposition = IngestDisposition::Duplicate;
      outcome.code = StatusCode::FencedDuplicateContent;
      outcome.explanation = "observation is byte identical to an already accepted observation";
      outcome.snapshot = published_ != nullptr ? published_->id() : SnapshotId{};
      return Status{};
    }
    ++source.fenced;
    return finish(StatusCode::FencedSequence,
                  "sequence " + std::to_string(observation.sequence.value()) +
                      " does not advance the source sequence " +
                      std::to_string(source.last_sequence.value()) + " for source " +
                      observation.source.to_text());
  }

  if (observation.generation < source.last_generation) {
    ++source.fenced;
    return finish(StatusCode::FencedStaleGeneration,
                  "generation " + std::to_string(observation.generation.value()) +
                      " is older than the last generation reported by this source (" +
                      std::to_string(source.last_generation.value()) + ")");
  }
  if (observation.generation.value() >
      source.last_generation.value() + config_.policy.ingest.max_generation_advance) {
    ++source.fenced;
    return finish(StatusCode::OutOfRange,
                  "generation advance exceeds the configured maximum in one step");
  }
  if (config_.policy.ingest.strict_generation_fence) {
    const std::uint64_t high_water = high_water_generation_locked();
    if (observation.generation.value() < high_water) {
      ++source.fenced;
      return finish(StatusCode::FencedStaleGeneration,
                    "strict generation fencing is enabled and generation " +
                        std::to_string(observation.generation.value()) +
                        " is below the observed high water generation " +
                        std::to_string(high_water));
    }
  }
  if (observation.generation == source.last_generation && observation.epoch < source.last_epoch) {
    ++source.fenced;
    return finish(StatusCode::FencedStaleEpoch,
                  "epoch " + std::to_string(observation.epoch.value()) +
                      " is older than the last epoch of the same generation reported by this "
                      "source (" +
                      std::to_string(source.last_epoch.value()) + ")");
  }

  // Claim level fences. Fenced claims stay visible in the snapshot as evidence
  // of what the source said, but they can never support an assertion.
  std::uint32_t accepted_claims = 0;
  std::uint32_t fenced_claims = 0;
  StatusCode first_fence = StatusCode::FencedAuthority;
  std::string first_fence_reason;
  for (const Claim& claim : observation.claims) {
    const std::string aspect_name = claim.aspect.value();
    if (config_.policy.ingest.reject_unknown_aspects &&
        find_aspect_descriptor(aspect_name) == nullptr) {
      if (fenced_claims == 0) {
        first_fence = StatusCode::Unsupported;
        first_fence_reason = "aspect '" + aspect_name +
                             "' is not a well-known aspect of this build";
      }
      record_fenced_claim_locked(observation, received_at, claim, StatusCode::Unsupported,
                                 "aspect '" + aspect_name +
                                     "' is not a well-known aspect of this build");
      ++fenced_claims;
      continue;
    }
    if (!source.descriptor.aspects.empty() && !source.descriptor.declares(aspect_name)) {
      if (fenced_claims == 0) {
        first_fence = StatusCode::FencedAuthority;
        first_fence_reason =
            "source did not declare aspect '" + aspect_name + "' in its observation scope";
      }
      record_fenced_claim_locked(observation, received_at, claim, StatusCode::FencedAuthority,
                                 "source did not declare aspect '" + aspect_name +
                                     "' in its observation scope");
      ++fenced_claims;
      continue;
    }
    const auto revision_key = std::make_tuple(observation.source, claim.subject.id, aspect_name);
    auto revision_slot = revisions_.find(revision_key);
    if (revision_slot == revisions_.end()) {
      // The revision table is bounded by the subject bound. Beyond it, revision
      // fencing is not applied for new keys; the bound is explicit rather than
      // an unbounded growth path.
      if (revisions_.size() < config_.policy.limits.max_subjects) {
        revision_slot = revisions_.emplace(revision_key, ClaimRevision{}).first;
      }
    }
    if (revision_slot != revisions_.end() && claim.revision.value() != 0 &&
        revision_slot->second.value() != 0 && claim.revision < revision_slot->second) {
      ClaimRevision& last = revision_slot->second;
      if (fenced_claims == 0) {
        first_fence = StatusCode::FencedRevision;
        first_fence_reason =
            "revision " + std::to_string(claim.revision.value()) +
            " is older than the last revision reported for this subject and aspect (" +
            std::to_string(last.value()) + ")";
      }
      record_fenced_claim_locked(observation, received_at, claim, StatusCode::FencedRevision,
                                 "revision " + std::to_string(claim.revision.value()) +
                                     " is older than the last revision reported for this subject "
                                     "and aspect (" +
                                     std::to_string(last.value()) + ")");
      ++fenced_claims;
      continue;
    }
    if (revision_slot != revisions_.end() && claim.revision > revision_slot->second) {
      revision_slot->second = claim.revision;
    }
    ++accepted_claims;
  }

  if (!observation.claims.empty() && accepted_claims == 0) {
    ++source.fenced;
    outcome.disposition = IngestDisposition::Fenced;
    outcome.code = first_fence;
    outcome.explanation = first_fence_reason.empty() ? "every claim in the observation was fenced"
                                                     : first_fence_reason;
    outcome.claims_fenced = fenced_claims;
    ++observations_fenced_;
    record_rejection_locked(observation, received_at, first_fence, outcome.explanation);
    if (options == IngestOptions::PublishSnapshot) {
      outcome.snapshot = publish_locked(received_at)->id();
    } else {
      dirty_ = true;
    }
    return Status{};
  }

  ObservationRecord record;
  record.observation = observation;
  record.received_at = received_at;
  record.recovered = false;
  record.recovery_epoch = restart_epoch_;

  if (journal_ != nullptr && !journal_failed_) {
    LockOrderGuard persistence(LockLevel::Persistence);
    Result<void> written = journal_->append(record);
    if (!written) {
      // A journal failure is reported but never silently ignored: the runtime
      // records the failure and keeps serving from memory.
      journal_failed_ = true;
      journal_failure_ = written.status().message();
    }
  }

  record_index_.emplace(outcome.observation_id, records_.size());
  records_.push_back(std::move(record));
  evict_evidence_locked();

  if (new_incarnation) {
    auto& seen = incarnations_[observation.source];
    seen.push_back(observation.incarnation);
    while (seen.size() > config_.policy.limits.max_incarnations_per_source) {
      seen.pop_front();
    }
    source.incarnation.id = observation.incarnation;
    source.incarnation.boot_time = received_at;
    source.incarnation.boot_counter = source.incarnation.boot_counter + 1;
  }
  source.last_sequence = observation.sequence;
  source.last_generation = observation.generation;
  source.last_epoch = observation.epoch;
  source.last_receive = received_at;
  source.last_observation = observation.observed_at;
  if (source.first_receive.nanos == 0) {
    source.first_receive = received_at;
  }
  ++source.accepted;
  ++observations_accepted_;

  outcome.disposition = IngestDisposition::Accepted;
  outcome.code = StatusCode::Ok;
  outcome.claims_accepted = accepted_claims;
  outcome.claims_fenced = fenced_claims;
  if (options == IngestOptions::PublishSnapshot) {
    outcome.snapshot = publish_locked(received_at)->id();
  } else {
    dirty_ = true;
  }
  return Status{};
}

Status Observatory::ingest(const Observation& observation, IngestOutcome& outcome,
                           IngestOptions options) {
  std::unique_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  return ingest_locked(observation, outcome, options);
}

Status Observatory::ingest_batch(std::span<const Observation> observations,
                                 IngestBatchReport& report, IngestOptions options) {
  if (observations.size() > config_.policy.limits.max_batch_observations) {
    return Status::error(StatusCode::TooLarge,
                         "batch exceeds the configured maximum number of observations");
  }
  std::unique_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  report = IngestBatchReport{};
  report.outcomes.reserve(observations.size());
  for (const Observation& observation : observations) {
    IngestOutcome outcome;
    const Status status = ingest_locked(observation, outcome, IngestOptions::DeferSnapshot);
    switch (outcome.disposition) {
      case IngestDisposition::Accepted:
        ++report.accepted;
        break;
      case IngestDisposition::Duplicate:
        ++report.duplicates;
        break;
      case IngestDisposition::Fenced:
        ++report.fenced;
        break;
      case IngestDisposition::Rejected:
        ++report.rejected;
        break;
    }
    report.outcomes.push_back(std::move(outcome));
    (void)status;
  }
  if (options == IngestOptions::PublishSnapshot) {
    report.snapshot = publish_locked(clock_->now())->id();
  }
  return Status{};
}

std::shared_ptr<const Snapshot> Observatory::current() {
  {
    std::shared_lock lock(state_mutex_);
    LockOrderGuard guard(LockLevel::State);
    if (published_ != nullptr && !dirty_) {
      return published_;
    }
  }
  std::unique_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  if (published_ != nullptr && !dirty_) {
    return published_;
  }
  return publish_locked(clock_->now());
}

Status Observatory::refresh(TimePoint evaluation_time, std::shared_ptr<const Snapshot>& out) {
  std::unique_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  out = publish_locked(evaluation_time);
  return Status{};
}

Status Observatory::publish(std::shared_ptr<const Snapshot>& out) {
  return refresh(clock_->now(), out);
}

Result<QueryResult> Observatory::query(const QueryFilter& filter) {
  std::shared_ptr<const Snapshot> snapshot = current();
  if (snapshot == nullptr) {
    return Err<QueryResult>(StatusCode::NotOpen, "no snapshot has been published");
  }
  return execute_query(*snapshot, filter, config_.policy.limits);
}

Result<HierarchyResult> Observatory::hierarchy(const HierarchyQuery& hierarchy_query) {
  std::shared_ptr<const Snapshot> snapshot = current();
  if (snapshot == nullptr) {
    return Err<HierarchyResult>(StatusCode::NotOpen, "no snapshot has been published");
  }
  return execute_hierarchy(*snapshot, hierarchy_query, config_.policy.limits);
}

Result<SnapshotDiff> Observatory::diff(const SnapshotId& before, const SnapshotId& after) {
  std::shared_ptr<const Snapshot> first;
  std::shared_ptr<const Snapshot> second;
  {
    std::shared_lock lock(state_mutex_);
    LockOrderGuard guard(LockLevel::State);
    first = history_.find(before);
    second = history_.find(after);
  }
  if (first == nullptr || second == nullptr) {
    return Err<SnapshotDiff>(
        StatusCode::NotFound,
        "snapshot history is per process: a snapshot published by an earlier run of this "
        "runtime is recorded in the journal but cannot be rebuilt, because recovered "
        "evidence is deliberately never presented as fresh");
  }
  return diff_snapshots(*first, *second, config_.policy.limits);
}

Result<SnapshotDiff> Observatory::diff_latest(std::uint32_t lookback) {
  std::shared_ptr<const Snapshot> before;
  std::shared_ptr<const Snapshot> after;
  {
    std::shared_lock lock(state_mutex_);
    LockOrderGuard guard(LockLevel::State);
    after = history_.latest();
    if (after == nullptr) {
      return Err<SnapshotDiff>(StatusCode::NotFound, "no snapshot has been published");
    }
    if (after->history_index().value() <= lookback) {
      return Err<SnapshotDiff>(
          StatusCode::NotFound,
          "snapshot history is per process: this run has published " +
              std::to_string(after->history_index().value()) +
              " snapshot(s) so far, and " + std::to_string(lookback) +
              " step(s) back is not available. The persisted journal records every published " +
              "snapshot identity, but a snapshot cannot be rebuilt from a later restart: " +
              "recovered evidence is deliberately never presented as fresh.");
    }
    before = history_.at(HistoryIndex(after->history_index().value() - lookback));
  }
  if (before == nullptr) {
    return Err<SnapshotDiff>(StatusCode::NotFound, "the earlier snapshot has been evicted");
  }
  return diff_snapshots(*before, *after, config_.policy.limits);
}

Result<std::vector<HistoryEntry>> Observatory::history(std::uint32_t max_entries) const {
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  return Ok(history_.entries(max_entries));
}

Result<std::shared_ptr<const Snapshot>> Observatory::snapshot_at(HistoryIndex index) const {
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  std::shared_ptr<const Snapshot> snapshot = history_.at(index);
  if (snapshot == nullptr) {
    return Err<std::shared_ptr<const Snapshot>>(StatusCode::NotFound,
                                                "no snapshot at that history index");
  }
  return Ok(std::move(snapshot));
}

Result<std::shared_ptr<const Snapshot>> Observatory::snapshot_by_id(const SnapshotId& id) const {
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  std::shared_ptr<const Snapshot> snapshot = history_.find(id);
  if (snapshot == nullptr) {
    return Err<std::shared_ptr<const Snapshot>>(StatusCode::NotFound,
                                                "that snapshot is not retained in history");
  }
  return Ok(std::move(snapshot));
}

Result<Explanation> Observatory::explain(const EntityRef& subject, std::optional<AspectId> aspect) {
  std::shared_ptr<const Snapshot> snapshot = current();
  if (snapshot == nullptr) {
    return Err<Explanation>(StatusCode::NotOpen, "no snapshot has been published");
  }
  const SubjectState* state = snapshot->find_subject(subject);
  if (state == nullptr) {
    return explain_absent(*snapshot, subject, aspect);
  }
  Result<Explanation> explanation =
      explain_subject(*snapshot, state->identity, aspect, config_.policy.limits);
  if (!explanation) {
    return explanation;
  }

  // Causal references are part of an observation, not of the snapshot, so they
  // are attached here from the retained evidence. The wording never asserts
  // causation; it names the recorded relationship and its weakness.
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  for (const AspectState& aspect_state : state->aspects) {
    if (aspect.has_value() && !(aspect_state.aspect == *aspect)) {
      continue;
    }
    for (const ClaimRecord& claim : aspect_state.claims) {
      const auto found = record_index_.find(claim.observation_id);
      if (found == record_index_.end() || found->second >= records_.size()) {
        continue;
      }
      const Observation& observation = records_[found->second].observation;
      for (const CausalRef& reference : observation.causal) {
        if (explanation->causality.size() >= config_.policy.limits.max_explanation_lines) {
          break;
        }
        explanation->causality.push_back(causality_line(reference, claim.observation_id));
      }
    }
  }
  return explanation;
}

Result<std::vector<SourceSummary>> Observatory::sources() const {
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  return Ok(source_summaries_locked(clock_->now()));
}

Result<std::vector<ObservationRecord>> Observatory::retained_records(std::uint32_t limit) const {
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  std::vector<ObservationRecord> result;
  const std::size_t bound = std::min<std::size_t>(limit, records_.size());
  result.reserve(bound);
  for (std::size_t index = 0; index < bound; ++index) {
    result.push_back(records_[records_.size() - bound + index]);
  }
  return Ok(std::move(result));
}

Result<RecoveryReport> Observatory::recovery() const {
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  return Ok(recovery_);
}

ObservatoryStats Observatory::stats() const {
  std::shared_lock lock(state_mutex_);
  LockOrderGuard guard(LockLevel::State);
  ObservatoryStats result;
  result.observations_accepted = observations_accepted_;
  result.observations_duplicate = observations_duplicate_;
  result.observations_fenced = observations_fenced_;
  result.observations_rejected = observations_rejected_;
  result.claims_fenced = claims_fenced_;
  result.evidence_evicted = evidence_evicted_;
  result.snapshots_published = snapshots_published_;
  result.publications_skipped_unchanged = publications_skipped_;
  result.journal_records = journal_ != nullptr ? journal_->records_written() : 0;
  result.journal_rotations = journal_ != nullptr ? journal_->rotations() : 0;
  result.sources = static_cast<std::uint32_t>(sources_.size());
  result.evidence_records = static_cast<std::uint64_t>(records_.size());
  result.restart_count = restart_count_;
  result.published = published_ != nullptr;
  result.dirty = dirty_;
  return result;
}

std::string Observatory::canonical_summary() {
  std::shared_ptr<const Snapshot> snapshot =
      published_ != nullptr && !dirty_ ? published_ : current();
  std::ostringstream out;
  out << "observatory fabric " << config_.fabric.to_text() << "\n";
  out << "  " << config_.policy.canonical_text() << "\n";
  out << "  restart_epoch " << restart_epoch_.value() << " restart_count " << restart_count_
      << "\n";
  if (journal_ != nullptr) {
    out << "  journal " << journal_->path() << " bytes " << journal_->size_bytes() << " rotations "
        << journal_->rotations() << " records " << journal_->records_written() << "\n";
    out << recovery_.to_text();
  } else {
    out << "  journal none (in-memory only)\n";
  }
  if (journal_failed_) {
    out << "  journal_failure " << journal_failure_ << "\n";
  }
  if (snapshot != nullptr) {
    out << snapshot->canonical_summary();
  } else {
    out << "  no snapshot has been published\n";
  }
  return out.str();
}

}  // namespace fabric_observatory
