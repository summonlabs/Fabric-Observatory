// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/snapshot.hpp"

#include "fabric_observatory/canonical.hpp"
#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/util.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace fabric_observatory {

namespace {

bool subject_ref_less(const SubjectState& state, const EntityRef& ref) noexcept {
  if (state.identity.kind != ref.kind) {
    return static_cast<std::uint8_t>(state.identity.kind) < static_cast<std::uint8_t>(ref.kind);
  }
  return state.identity.id < ref.id;
}

bool subject_less(const SubjectState& lhs, const SubjectState& rhs) noexcept {
  return subject_ref_less(lhs, rhs.identity.ref());
}

void encode_u128(CanonicalEncoder& encoder, const Uint128& value) {
  encoder.u64(value.hi);
  encoder.u64(value.lo);
}

void encode_identity(CanonicalEncoder& encoder, const SubjectIdentity& identity) {
  encoder.u8(static_cast<std::uint8_t>(identity.kind));
  encode_u128(encoder, identity.id.value());
  encoder.text(identity.typed_text());
}

void encode_claim_record(CanonicalEncoder& encoder, const ClaimRecord& claim) {
  encode_u128(encoder, claim.source.value());
  encode_u128(encoder, claim.incarnation.value());
  encoder.u8(static_cast<std::uint8_t>(claim.authority));
  encoder.u64(claim.sequence.value());
  encoder.u64(claim.revision.value());
  claim.value.encode(encoder);
  encoder.boolean(claim.supported);
  encoder.u8(static_cast<std::uint8_t>(claim.freshness));
  encoder.i64(claim.observed_at.nanos);
  encoder.i64(claim.received_at.nanos);
  encoder.boolean(claim.recovered);
  encoder.u64(claim.recovery_epoch.value());
  encoder.boolean(claim.superseded_generation);
  encoder.boolean(claim.superseded_epoch);
  encoder.boolean(claim.superseded_incarnation);
  encoder.boolean(claim.superseded_by_later_sequence);
  encoder.bytes(as_bytes(claim.observation_id));
}

void encode_aspect_state(CanonicalEncoder& encoder, const AspectState& aspect) {
  encoder.text(aspect.aspect.view());
  encoder.u8(static_cast<std::uint8_t>(aspect.truth));
  encoder.boolean(aspect.agreed_value.has_value());
  if (aspect.agreed_value.has_value()) {
    aspect.agreed_value->encode(encoder);
  }
  encoder.u32(aspect.coverage.distinct_fresh_sources);
  encoder.u32(aspect.coverage.required_distinct_fresh_sources);
  encoder.u8(static_cast<std::uint8_t>(aspect.coverage.best_fresh_authority));
  encoder.u8(static_cast<std::uint8_t>(aspect.coverage.required_authority));
  encoder.boolean(aspect.coverage.satisfied);
  encoder.boolean(aspect.claims_truncated);
  encoder.u64(static_cast<std::uint64_t>(aspect.claims.size()));
  for (const ClaimRecord& claim : aspect.claims) {
    encode_claim_record(encoder, claim);
  }
  encoder.u64(static_cast<std::uint64_t>(aspect.conflicts.size()));
  for (const ConflictGroup& conflict : aspect.conflicts) {
    conflict.value.encode(encoder);
    encoder.u64(static_cast<std::uint64_t>(conflict.sources.size()));
    for (const SourceId& source : conflict.sources) {
      encode_u128(encoder, source.value());
    }
    encoder.u32(conflict.fresh_sources);
  }
}

void encode_subject_state(CanonicalEncoder& encoder, const SubjectState& subject) {
  encode_identity(encoder, subject.identity);
  encoder.boolean(subject.parent.has_value());
  if (subject.parent.has_value()) {
    encode_u128(encoder, subject.parent->value());
  }
  encoder.u8(static_cast<std::uint8_t>(subject.topology_truth));
  encoder.u8(static_cast<std::uint8_t>(subject.worst_truth));
  encoder.u64(static_cast<std::uint64_t>(subject.aspects.size()));
  for (const AspectState& aspect : subject.aspects) {
    encode_aspect_state(encoder, aspect);
  }
}

void encode_source_summary(CanonicalEncoder& encoder, const SourceSummary& source) {
  encode_u128(encoder, source.id.value());
  encoder.text(source.name);
  encoder.text(source.authority_name);
  encoder.u8(static_cast<std::uint8_t>(source.max_authority));
  encode_u128(encoder, source.incarnation.id.value());
  encoder.i64(source.incarnation.boot_time.nanos);
  encoder.u64(source.incarnation.boot_counter);
  encoder.u64(source.last_sequence.value());
  encoder.i64(source.first_receive.nanos);
  encoder.i64(source.last_receive.nanos);
  encoder.i64(source.last_observation.nanos);
  encoder.u64(source.last_generation.value());
  encoder.u64(source.last_epoch.value());
  encoder.u64(source.accepted);
  encoder.u64(source.retained_records);
  encoder.u8(static_cast<std::uint8_t>(source.freshness));
  encoder.boolean(source.recovered);
}

void encode_rejection(CanonicalEncoder& encoder, const RejectedObservation& rejection) {
  encoder.bytes(as_bytes(rejection.observation_id));
  encode_u128(encoder, rejection.source.value());
  encode_u128(encoder, rejection.incarnation.value());
  encoder.u64(rejection.sequence.value());
  encoder.u64(rejection.generation.value());
  encoder.u64(rejection.epoch.value());
  encoder.i64(rejection.received_at.nanos);
  encoder.u16(static_cast<std::uint16_t>(rejection.fence));
  encoder.text(rejection.explanation);
}

void encode_fenced_claim(CanonicalEncoder& encoder, const FencedClaim& claim) {
  encode_identity(encoder, claim.subject);
  encoder.text(claim.aspect.view());
  encode_u128(encoder, claim.source.value());
  encode_u128(encoder, claim.incarnation.value());
  encoder.u64(claim.sequence.value());
  encoder.i64(claim.received_at.nanos);
  encoder.u16(static_cast<std::uint16_t>(claim.fence));
  encoder.text(claim.explanation);
}

// Deterministic accumulation of per-subject and per-aspect evidence.
struct SubjectBucket {
  SubjectIdentity identity{};
  std::map<AspectId, std::vector<ClaimRecord>> aspects{};
};

struct IncarnationCursor {
  TimePoint received{};
  IncarnationId id{};
  bool set{false};
};

// True when the candidate should replace the current cursor: the most recently
// received incarnation of a source is the one whose claims are current. Ties on
// receive time are broken by the incarnation identity, so the rule is total.
bool cursor_newer(const IncarnationCursor& candidate, const IncarnationCursor& current) noexcept {
  if (!current.set) {
    return true;
  }
  if (candidate.received != current.received) {
    return current.received < candidate.received;
  }
  return current.id < candidate.id;
}

std::vector<SourceId> sorted_sources(const std::set<SourceId>& sources) {
  return std::vector<SourceId>(sources.begin(), sources.end());
}

}  // namespace

std::string SnapshotId::to_text() const { return "snap:" + digest_.to_hex(); }

Result<SnapshotId> SnapshotId::from_text(std::string_view text) {
  constexpr std::string_view kPrefix = "snap:";
  if (util::starts_with(text, kPrefix)) {
    text.remove_prefix(kPrefix.size());
  }
  Result<Digest256> digest = Digest256::from_hex(text);
  if (!digest) {
    return Err<SnapshotId>(digest.status().code(), digest.status().message());
  }
  return Ok(SnapshotId(*digest));
}

FreshnessVerdict evaluate_freshness(TimePoint evidence_time, TimePoint evaluation_time,
                                    const FreshnessPolicy& policy, bool recovered) {
  // Evidence after the evaluation instant cannot be fresher than "just
  // received"; the clock skew check at ingest is what reports the anomaly.
  // Saturating arithmetic keeps the function total for extreme timestamps.
  const Duration age = saturating_difference(evaluation_time, evidence_time);
  FreshnessVerdict verdict = FreshnessVerdict::Stale;
  if (age <= policy.fresh_window) {
    verdict = FreshnessVerdict::Fresh;
  } else if (age <= policy.aging_window) {
    verdict = FreshnessVerdict::Aging;
  } else {
    verdict = FreshnessVerdict::Stale;
  }
  if (recovered && !policy.allow_recovered_as_fresh && verdict == FreshnessVerdict::Fresh) {
    // Conservative restart: evidence that came back from persistence is capped
    // at Aging for ever, so a restart can never resurrect a fresh assertion.
    verdict = FreshnessVerdict::Aging;
  }
  return verdict;
}

TruthEvaluation evaluate_truth(const AspectId& aspect, const std::vector<ClaimRecord>& claims,
                               const TruthPolicy& policy) {
  TruthEvaluation evaluation;
  const CoveragePolicy coverage = policy.coverage_for(aspect.view());
  evaluation.coverage.required_distinct_fresh_sources = coverage.min_distinct_fresh_sources;
  evaluation.coverage.required_authority = coverage.required_authority;

  if (claims.empty()) {
    evaluation.truth = TruthState::Unknown;
    return evaluation;
  }

  bool any_supported = false;
  for (const ClaimRecord& claim : claims) {
    if (claim.supported) {
      any_supported = true;
      break;
    }
  }
  if (!any_supported) {
    evaluation.truth = TruthState::Unsupported;
    return evaluation;
  }

  std::vector<const ClaimRecord*> assertive;
  assertive.reserve(claims.size());
  for (const ClaimRecord& claim : claims) {
    if (claim.is_assertive()) {
      assertive.push_back(&claim);
    }
  }
  if (assertive.empty()) {
    evaluation.truth = TruthState::Stale;
    return evaluation;
  }

  std::set<SourceId> distinct_sources;
  SourceAuthority best_authority = SourceAuthority::None;
  std::map<Value, std::vector<const ClaimRecord*>> groups;
  for (const ClaimRecord* claim : assertive) {
    distinct_sources.insert(claim->source);
    if (claim->authority > best_authority) {
      best_authority = claim->authority;
    }
    groups[claim->value].push_back(claim);
  }

  evaluation.coverage.distinct_fresh_sources =
      static_cast<std::uint32_t>(distinct_sources.size());
  evaluation.coverage.best_fresh_authority = best_authority;
  evaluation.coverage.satisfied =
      distinct_sources.size() >= coverage.min_distinct_fresh_sources &&
      best_authority >= coverage.required_authority;

  if (groups.size() > 1) {
    evaluation.truth = TruthState::Conflicting;
    evaluation.conflicts.reserve(groups.size());
    for (const auto& group : groups) {
      ConflictGroup conflict;
      conflict.value = group.first;
      std::set<SourceId> group_sources;
      for (const ClaimRecord* claim : group.second) {
        group_sources.insert(claim->source);
      }
      conflict.fresh_sources = static_cast<std::uint32_t>(group_sources.size());
      conflict.sources = sorted_sources(group_sources);
      evaluation.conflicts.push_back(std::move(conflict));
    }
    return evaluation;
  }

  if (!evaluation.coverage.satisfied) {
    evaluation.truth = TruthState::Incomplete;
    return evaluation;
  }

  evaluation.truth = TruthState::Known;
  evaluation.agreed_value = groups.begin()->first;
  return evaluation;
}

std::shared_ptr<const Snapshot> Snapshot::build(const SnapshotBuildRequest& request) {
  FABRIC_OBSERVATORY_CONTRACT_MSG(!request.fabric.is_nil(),
                                  "a snapshot requires a fabric identity");

  std::shared_ptr<Snapshot> snapshot(new Snapshot());
  snapshot->fabric_ = request.fabric;
  snapshot->policy_digest_ = request.policy.digest();
  snapshot->policy_name_ = request.policy.name;
  snapshot->evaluation_time_ = request.evaluation_time;
  snapshot->restart_epoch_ = request.restart_epoch;
  snapshot->restart_count_ = request.restart_count;
  snapshot->evidence_records_ = static_cast<std::uint64_t>(request.records.size());
  snapshot->evidence_truncated_ = request.evidence_truncated;

  // Canonical record order: independent of arrival order, thread scheduling and
  // container iteration order. An index permutation is sorted rather than the
  // records themselves, so building a snapshot does not copy the whole evidence
  // set.
  std::vector<std::size_t> order(request.records.size());
  for (std::size_t index = 0; index < order.size(); ++index) {
    order[index] = index;
  }
  std::sort(order.begin(), order.end(), [&request](std::size_t lhs, std::size_t rhs) {
    const Observation& left = request.records[lhs].observation;
    const Observation& right = request.records[rhs].observation;
    if (left.source != right.source) {
      return left.source < right.source;
    }
    if (left.incarnation != right.incarnation) {
      return left.incarnation < right.incarnation;
    }
    if (left.sequence != right.sequence) {
      return left.sequence < right.sequence;
    }
    if (left.content_digest() != right.content_digest()) {
      return left.content_digest() < right.content_digest();
    }
    return lhs < rhs;
  });

  std::uint64_t high_generation = 0;
  std::uint64_t current_epoch = 0;
  for (const std::size_t index : order) {
    high_generation =
        std::max(high_generation, request.records[index].observation.generation.value());
  }
  for (const std::size_t index : order) {
    const Observation& observation = request.records[index].observation;
    if (observation.generation.value() == high_generation) {
      current_epoch = std::max(current_epoch, observation.epoch.value());
    }
  }
  snapshot->generation_ = GenerationId(high_generation);
  snapshot->epoch_ = EpochId(current_epoch);

  std::map<SourceId, IncarnationCursor> current_incarnations;
  for (const std::size_t index : order) {
    const ObservationRecord& record = request.records[index];
    const IncarnationCursor candidate{record.received_at, record.observation.incarnation, true};
    auto& slot = current_incarnations[record.observation.source];
    if (cursor_newer(candidate, slot)) {
      slot = candidate;
    }
  }

  bool have_received = false;
  for (const std::size_t index : order) {
    const ObservationRecord& record = request.records[index];
    snapshot->observed_high_water_ =
        std::max(snapshot->observed_high_water_, record.observation.observed_at);
    snapshot->received_high_water_ =
        std::max(snapshot->received_high_water_, record.received_at);
    if (!have_received || record.received_at < snapshot->received_low_water_) {
      snapshot->received_low_water_ = record.received_at;
      have_received = true;
    }
  }

  std::map<SourceId, SourceAuthority> declared_authority;
  for (const SourceSummary& summary : request.sources) {
    declared_authority.emplace(summary.id, summary.max_authority);
  }

  // Claim level fences decided at ingest are applied here: the claim stays in
  // the snapshot as evidence of what was said, and it can never assert.
  std::map<std::tuple<Digest256, EntityId, std::string>, StatusCode> fenced_claims;
  for (const FencedClaim& fenced : request.fenced_claims) {
    fenced_claims.emplace(std::make_tuple(fenced.observation_id, fenced.subject.id,
                                          fenced.aspect.value()),
                          fenced.fence);
  }

  std::map<EntityId, SubjectBucket> buckets;
  bool subjects_truncated = false;
  const std::uint32_t max_subjects = request.policy.limits.max_subjects;
  // The pre-collapse working set per aspect is bounded by the number of
  // registered sources times the retained incarnations; the hard cap below is
  // the configured per-aspect claim bound multiplied by the number of sources
  // this runtime will ever know about, so it cannot be reached by a single
  // hostile observation.
  const std::size_t working_set_cap =
      static_cast<std::size_t>(request.policy.limits.max_sources) *
          static_cast<std::size_t>(request.policy.limits.max_claims_per_subject_aspect) +
      request.policy.limits.max_claims_per_subject_aspect;

  for (const std::size_t index : order) {
    const ObservationRecord& record = request.records[index];
    const Observation& observation = record.observation;
    const bool superseded_generation = observation.generation.value() < high_generation;
    const bool superseded_epoch =
        observation.generation.value() == high_generation && observation.epoch.value() < current_epoch;
    const auto cursor = current_incarnations.find(observation.source);
    const bool superseded_incarnation =
        cursor != current_incarnations.end() && cursor->second.set &&
        !(cursor->second.id == observation.incarnation);
    const Digest256 observation_id = observation.content_digest();

    for (const Claim& claim : observation.claims) {
      auto bucket = buckets.find(claim.subject.id);
      if (bucket == buckets.end()) {
        if (buckets.size() >= max_subjects) {
          subjects_truncated = true;
          continue;
        }
        SubjectBucket fresh;
        fresh.identity = claim.subject;
        bucket = buckets.emplace(claim.subject.id, std::move(fresh)).first;
      }
      auto& aspect_claims = bucket->second.aspects[claim.aspect];
      if (aspect_claims.size() >= working_set_cap) {
        // Hard stop: even the pre-collapse working set is bounded, so a flood of
        // observations cannot make snapshot construction unbounded.
        continue;
      }
      ClaimRecord record_entry;
      record_entry.source = observation.source;
      record_entry.incarnation = observation.incarnation;
      const auto authority = declared_authority.find(observation.source);
      record_entry.authority = authority != declared_authority.end()
                                   ? authority->second
                                   : request.policy.ingest.auto_register_authority;
      record_entry.sequence = observation.sequence;
      record_entry.revision = claim.revision;
      record_entry.value = claim.value;
      record_entry.supported = claim.supported;
      record_entry.freshness = evaluate_freshness(
          evidence_reference_time(observation.observed_at, record.received_at),
          request.evaluation_time, request.policy.freshness, record.recovered);
      record_entry.observed_at = observation.observed_at;
      record_entry.received_at = record.received_at;
      record_entry.recovered = record.recovered;
      record_entry.recovery_epoch = record.recovery_epoch;
      record_entry.superseded_generation = superseded_generation;
      record_entry.superseded_epoch = superseded_epoch;
      record_entry.superseded_incarnation = superseded_incarnation;
      record_entry.observation_id = observation_id;
      const auto fence = fenced_claims.find(
          std::make_tuple(observation_id, claim.subject.id, claim.aspect.value()));
      if (fence != fenced_claims.end()) {
        record_entry.fenced = true;
        record_entry.fence = fence->second;
      }
      aspect_claims.push_back(std::move(record_entry));
    }
  }

  snapshot->subjects_.reserve(buckets.size());
  for (auto& bucket_entry : buckets) {
    SubjectState subject;
    subject.identity = bucket_entry.second.identity;
    subject.aspects.reserve(bucket_entry.second.aspects.size());
    TruthState worst = TruthState::Known;
    for (auto& aspect_entry : bucket_entry.second.aspects) {
      AspectState aspect;
      aspect.aspect = aspect_entry.first;
      aspect.domain = aspect_entry.first.domain();

      std::vector<ClaimRecord>& claims = aspect_entry.second;
      // Per source incarnation, only the latest statement about this aspect is
      // current. The source disagreeing with its own history is an update, not
      // a conflict.
      std::map<std::pair<SourceId, IncarnationId>, std::size_t> latest;
      for (std::size_t index = 0; index < claims.size(); ++index) {
        const auto key = std::make_pair(claims[index].source, claims[index].incarnation);
        const auto found = latest.find(key);
        if (found == latest.end()) {
          latest.emplace(key, index);
          continue;
        }
        const ClaimRecord& incumbent = claims[found->second];
        const ClaimRecord& candidate = claims[index];
        if (candidate.sequence > incumbent.sequence ||
            (candidate.sequence == incumbent.sequence &&
             incumbent.observation_id < candidate.observation_id)) {
          found->second = index;
        }
      }
      for (std::size_t index = 0; index < claims.size(); ++index) {
        const auto key = std::make_pair(claims[index].source, claims[index].incarnation);
        if (latest[key] != index) {
          claims[index].superseded_by_later_sequence = true;
        }
      }
      std::sort(claims.begin(), claims.end());

      const std::size_t claim_bound = request.policy.limits.max_claims_per_subject_aspect;
      if (claims.size() > claim_bound) {
        // Keep the most informative evidence: current evidence before
        // superseded evidence, fresher before staler, higher sequence before
        // lower. The ranking is total, so the retained set is deterministic.
        std::vector<std::size_t> ranking(claims.size());
        for (std::size_t index = 0; index < ranking.size(); ++index) {
          ranking[index] = index;
        }
        std::partial_sort(ranking.begin(),
                          ranking.begin() + static_cast<std::ptrdiff_t>(claim_bound),
                          ranking.end(), [&claims](std::size_t lhs, std::size_t rhs) {
                            const ClaimRecord& left = claims[lhs];
                            const ClaimRecord& right = claims[rhs];
                            if (left.is_current() != right.is_current()) {
                              return left.is_current();
                            }
                            if (left.freshness != right.freshness) {
                              return left.freshness < right.freshness;
                            }
                            if (left.sequence != right.sequence) {
                              return right.sequence < left.sequence;
                            }
                            return lhs < rhs;
                          });
        std::vector<ClaimRecord> retained;
        retained.reserve(claim_bound);
        for (std::size_t index = 0; index < claim_bound; ++index) {
          retained.push_back(claims[ranking[index]]);
        }
        std::sort(retained.begin(), retained.end());
        claims = std::move(retained);
        aspect.claims_truncated = true;
      }

      const TruthEvaluation evaluation = evaluate_truth(aspect.aspect, claims, request.policy.truth);
      aspect.truth = evaluation.truth;
      aspect.agreed_value = evaluation.agreed_value;
      aspect.conflicts = evaluation.conflicts;
      aspect.coverage = evaluation.coverage;
      aspect.claims = claims;

      if (aspect.aspect.view() == "topology.parent" && aspect.truth == TruthState::Known &&
          aspect.agreed_value.has_value() &&
          aspect.agreed_value->kind() == Value::Kind::Text) {
        if (Result<SubjectIdentity> parent = SubjectIdentity::from_text(aspect.agreed_value->as_text());
            parent) {
          subject.parent = parent->id;
        }
      }
      if (aspect.aspect.view() == "topology.parent") {
        subject.topology_truth = aspect.truth;
      }

      if (static_cast<std::uint8_t>(aspect.truth) < static_cast<std::uint8_t>(worst)) {
        worst = aspect.truth;
      }
      subject.aspects.push_back(std::move(aspect));
    }
    subject.worst_truth = subject.aspects.empty() ? TruthState::Unknown : worst;
    snapshot->subjects_.push_back(std::move(subject));
  }

  std::sort(snapshot->subjects_.begin(), snapshot->subjects_.end(), subject_less);

  snapshot->sources_ = request.sources;
  std::sort(snapshot->sources_.begin(), snapshot->sources_.end(),
            [](const SourceSummary& lhs, const SourceSummary& rhs) { return lhs.id < rhs.id; });
  snapshot->rejected_ = request.rejected;
  std::sort(snapshot->rejected_.begin(), snapshot->rejected_.end());
  snapshot->rejected_truncated_ =
      snapshot->rejected_.size() > request.policy.limits.max_rejections_retained;
  if (snapshot->rejected_truncated_) {
    snapshot->rejected_.resize(request.policy.limits.max_rejections_retained);
  }
  snapshot->fenced_claims_ = request.fenced_claims;
  std::sort(snapshot->fenced_claims_.begin(), snapshot->fenced_claims_.end());
  if (snapshot->fenced_claims_.size() > request.policy.limits.max_rejections_retained) {
    snapshot->fenced_claims_.resize(request.policy.limits.max_rejections_retained);
  }

  SnapshotStats stats;
  stats.subjects = static_cast<std::uint64_t>(snapshot->subjects_.size());
  stats.sources = static_cast<std::uint64_t>(snapshot->sources_.size());
  stats.rejected_observations = static_cast<std::uint64_t>(snapshot->rejected_.size());
  stats.fenced_claims = static_cast<std::uint64_t>(snapshot->fenced_claims_.size());
  for (const SubjectState& subject : snapshot->subjects_) {
    ++stats.subjects_by_kind[static_cast<std::size_t>(subject.identity.kind)];
    for (const AspectState& aspect : subject.aspects) {
      ++stats.aspects;
      ++stats.aspects_by_truth[static_cast<std::size_t>(aspect.truth)];
      stats.claims += static_cast<std::uint64_t>(aspect.claims.size());
      stats.conflicts += static_cast<std::uint64_t>(aspect.conflicts.size());
      for (const ClaimRecord& claim : aspect.claims) {
        if (!claim.supported) {
          ++stats.unsupported_claims;
        }
        if (!claim.is_current()) {
          ++stats.superseded_claims;
        }
        if (claim.recovered) {
          ++stats.recovered_claims;
        }
      }
    }
  }
  if (subjects_truncated) {
    snapshot->evidence_truncated_ = true;
  }
  snapshot->stats_ = stats;

  snapshot->compute_digest();
  return std::shared_ptr<const Snapshot>(std::move(snapshot));
}

void Snapshot::encode_content(CanonicalEncoder& encoder) const {
  encoder.tag("snapshot");
  encoder.u8(1);
  encode_u128(encoder, fabric_.value());
  encoder.u64(generation_.value());
  encoder.u64(epoch_.value());
  encoder.i64(evaluation_time_.nanos);
  encoder.i64(observed_high_water_.nanos);
  encoder.i64(received_high_water_.nanos);
  encoder.i64(received_low_water_.nanos);
  encoder.bytes(as_bytes(policy_digest_));
  encoder.text(policy_name_);
  encoder.u64(restart_epoch_.value());
  encoder.u32(restart_count_);
  encoder.boolean(evidence_truncated_);

  encoder.u64(static_cast<std::uint64_t>(sources_.size()));
  for (const SourceSummary& source : sources_) {
    encode_source_summary(encoder, source);
  }
  encoder.u64(static_cast<std::uint64_t>(subjects_.size()));
  for (const SubjectState& subject : subjects_) {
    encode_subject_state(encoder, subject);
  }
  encoder.u64(static_cast<std::uint64_t>(rejected_.size()));
  for (const RejectedObservation& rejection : rejected_) {
    encode_rejection(encoder, rejection);
  }
  encoder.u64(static_cast<std::uint64_t>(fenced_claims_.size()));
  for (const FencedClaim& claim : fenced_claims_) {
    encode_fenced_claim(encoder, claim);
  }
}

void Snapshot::compute_digest() {
  CanonicalEncoder encoder(4096);
  encode_content(encoder);
  id_ = SnapshotId(encoder.digest());
}

Digest256 Snapshot::recompute_digest() const {
  CanonicalEncoder encoder(4096);
  encode_content(encoder);
  return encoder.digest();
}

const SubjectState* Snapshot::find_subject(const EntityRef& ref) const noexcept {
  const auto found = std::lower_bound(subjects_.begin(), subjects_.end(), ref, subject_ref_less);
  if (found == subjects_.end() || found->identity.kind != ref.kind || found->identity.id != ref.id) {
    return nullptr;
  }
  return &*found;
}

const AspectState* Snapshot::find_aspect(const EntityRef& ref, std::string_view aspect) const noexcept {
  const SubjectState* subject = find_subject(ref);
  if (subject == nullptr) {
    return nullptr;
  }
  const auto found = std::lower_bound(
      subject->aspects.begin(), subject->aspects.end(), aspect,
      [](const AspectState& state, std::string_view name) { return state.aspect.view() < name; });
  if (found == subject->aspects.end() || found->aspect.view() != aspect) {
    return nullptr;
  }
  return &*found;
}

const SourceSummary* Snapshot::find_source(const SourceId& id) const noexcept {
  const auto found = std::lower_bound(
      sources_.begin(), sources_.end(), id,
      [](const SourceSummary& summary, const SourceId& key) { return summary.id < key; });
  if (found == sources_.end() || !(found->id == id)) {
    return nullptr;
  }
  return &*found;
}

std::string Snapshot::canonical_summary() const {
  std::ostringstream out;
  out << "snapshot " << id_.to_text() << "\n";
  out << "  fabric " << fabric_.to_text() << "\n";
  out << "  generation " << generation_.value() << " epoch " << epoch_.value() << "\n";
  out << "  evaluation_time " << format_utc(evaluation_time_) << "\n";
  out << "  observed_high_water " << format_utc(observed_high_water_) << "\n";
  out << "  received_window [" << format_utc(received_low_water_) << ", "
      << format_utc(received_high_water_) << "]\n";
  out << "  policy " << policy_name_ << " " << policy_digest_.to_short_hex() << "\n";
  out << "  restart_epoch " << restart_epoch_.value() << " restart_count " << restart_count_
      << "\n";
  out << "  evidence_records " << evidence_records_ << "\n";
  out << "  sources " << stats_.sources << "\n";
  for (const SourceSummary& source : sources_) {
    out << "    " << source.name << " " << source.id.to_text() << " authority="
        << to_string(source.max_authority) << " incarnation=" << source.incarnation.id.to_text()
        << " accepted=" << source.accepted << " retained=" << source.retained_records
        << " freshness=" << to_string(source.freshness) << "\n";
  }
  out << "  subjects " << stats_.subjects << " aspects " << stats_.aspects << " claims "
      << stats_.claims << " conflicts " << stats_.conflicts << "\n";
  for (std::size_t index = 0; index < kEntityKindCount; ++index) {
    out << "    kind " << to_string(static_cast<EntityKind>(index)) << " "
        << stats_.subjects_by_kind[index] << "\n";
  }
  for (std::size_t index = 0; index < kTruthStateCount; ++index) {
    out << "    truth " << to_string(static_cast<TruthState>(index)) << " "
        << stats_.aspects_by_truth[index] << "\n";
  }
  out << "  rejected_observations " << stats_.rejected_observations << " fenced_claims "
      << stats_.fenced_claims << " superseded_claims " << stats_.superseded_claims
      << " recovered_claims " << stats_.recovered_claims << "\n";
  for (const SubjectState& subject : subjects_) {
    out << "  subject " << subject.identity.typed_text() << " worst="
        << to_string(subject.worst_truth) << "\n";
    for (const AspectState& aspect : subject.aspects) {
      out << "    aspect " << aspect.aspect.view() << " " << to_string(aspect.truth);
      if (aspect.agreed_value.has_value()) {
        out << " value=" << aspect.agreed_value->display_text();
      }
      out << " fresh_sources=" << aspect.coverage.distinct_fresh_sources << "/"
          << aspect.coverage.required_distinct_fresh_sources
          << " authority=" << to_string(aspect.coverage.best_fresh_authority) << "/"
          << to_string(aspect.coverage.required_authority) << "\n";
      for (const ConflictGroup& conflict : aspect.conflicts) {
        out << "      conflict " << conflict.value.display_text() << " sources="
            << conflict.fresh_sources << "\n";
      }
      for (const ClaimRecord& claim : aspect.claims) {
        out << "      claim " << claim.source.to_text() << " seq=" << claim.sequence.value()
            << " " << to_string(claim.freshness) << " value=" << claim.value.display_text();
        if (!claim.supported) {
          out << " unsupported";
        }
        if (claim.superseded_generation) {
          out << " superseded-generation";
        }
        if (claim.superseded_epoch) {
          out << " superseded-epoch";
        }
        if (claim.superseded_incarnation) {
          out << " superseded-incarnation";
        }
        if (claim.superseded_by_later_sequence) {
          out << " superseded-sequence";
        }
        if (claim.recovered) {
          out << " recovered";
        }
        out << "\n";
      }
    }
  }
  return out.str();
}

}  // namespace fabric_observatory
