// Unit tests: canonical immutable snapshots.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <algorithm>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

// A fixed base time keeps freshness a property of the fixture rather than of
// how long the test process happened to run.
constexpr std::int64_t kBase = 1000000000000;

ObservationRecord make_record(std::uint32_t source, std::uint64_t sequence, std::uint64_t generation,
                              std::uint64_t epoch, const std::string& device, const char* aspect,
                              const std::string& value, std::int64_t received_at_offset,
                              bool recovered = false,
                              SourceAuthority authority = SourceAuthority::Reported) {
  const std::int64_t received_at = kBase + received_at_offset;
  ObservationBuilder builder(source_id("s" + std::to_string(source)),
                             "s" + std::to_string(source) + "/boot-1", sequence, generation, epoch,
                             received_at);
  builder.claim_text(device_subject(device), aspect, value);
  ObservationRecord record;
  record.observation = builder.build();
  record.received_at = TimePoint{received_at};
  record.recovered = recovered;
  if (recovered) {
    record.recovery_epoch = RestartEpoch(1);
  }
  (void)authority;
  return record;
}

// Rebuilds source summaries the same way the runtime does: the current
// incarnation is the most recently received one, with ties broken by identity.
// The helper must not be order dependent, or the determinism assertions below
// would be measuring the helper rather than the runtime.
std::vector<SourceSummary> source_summaries(const std::vector<ObservationRecord>& records) {
  std::map<SourceId, SourceSummary> summaries;
  std::map<SourceId, std::pair<TimePoint, IncarnationId>> cursors;
  for (const ObservationRecord& record : records) {
    SourceSummary& summary = summaries[record.observation.source];
    if (summary.id.is_nil()) {
      summary.id = record.observation.source;
      summary.name = record.observation.source.to_text();
      summary.max_authority = SourceAuthority::Reported;
    }
    const std::pair<TimePoint, IncarnationId> candidate{record.received_at,
                                                        record.observation.incarnation};
    auto& cursor = cursors[record.observation.source];
    if (summary.incarnation.id.is_nil() || cursor < candidate) {
      cursor = candidate;
      summary.incarnation.id = record.observation.incarnation;
      summary.incarnation.boot_time = record.received_at;
    }
    summary.last_sequence = std::max(summary.last_sequence, record.observation.sequence);
    summary.last_receive = std::max(summary.last_receive, record.received_at);
    summary.last_observation = std::max(summary.last_observation, record.observation.observed_at);
    summary.last_generation = std::max(summary.last_generation, record.observation.generation);
    summary.last_epoch = std::max(summary.last_epoch, record.observation.epoch);
    if (summary.first_receive.nanos == 0 || record.received_at < summary.first_receive) {
      summary.first_receive = record.received_at;
    }
    ++summary.accepted;
    ++summary.retained_records;
  }
  std::vector<SourceSummary> ordered;
  ordered.reserve(summaries.size());
  for (auto& entry : summaries) {
    ordered.push_back(std::move(entry.second));
  }
  return ordered;
}

SnapshotBuildRequest make_request(std::vector<ObservationRecord> records,
                                  TimePoint evaluation_time = TimePoint{kBase + 2000},
                                  Policy policy = Policy{}) {
  SnapshotBuildRequest request;
  request.fabric = test_fabric();
  request.policy = std::move(policy);
  request.evaluation_time = evaluation_time;
  request.sources = source_summaries(records);
  request.records = std::move(records);
  return request;
}

std::vector<ObservationRecord> sample_records() {
  std::vector<ObservationRecord> records;
  for (std::uint32_t source = 0; source < 4; ++source) {
    for (std::uint32_t device = 0; device < 6; ++device) {
      records.push_back(make_record(source, device + 1, 5, 2,
                                    "dev" + std::to_string(device), "link.state",
                                    (source % 2 == 0) ? "up" : "down", 1000 + device));
    }
  }
  return records;
}

}  // namespace

FABOBS_TEST(snapshot, identity_is_content_addressed_and_repeatable) {
  const std::shared_ptr<const Snapshot> first = Snapshot::build(make_request(sample_records()));
  const std::shared_ptr<const Snapshot> second = Snapshot::build(make_request(sample_records()));
  FABOBS_REQUIRE(first != nullptr);
  FABOBS_REQUIRE(second != nullptr);
  FABOBS_CHECK_EQ(first->id(), second->id());
  FABOBS_CHECK_EQ(first->digest(), second->digest());
  FABOBS_CHECK_EQ(first->recompute_digest(), first->digest());
  FABOBS_CHECK(!first->id().is_zero());
}

FABOBS_TEST(snapshot, identity_is_independent_of_evidence_order) {
  std::vector<ObservationRecord> records = sample_records();
  const SnapshotId reference = Snapshot::build(make_request(records))->id();

  std::mt19937 generator(20260101u);
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::shuffle(records.begin(), records.end(), generator);
    const std::shared_ptr<const Snapshot> shuffled = Snapshot::build(make_request(records));
    FABOBS_CHECK_EQ(shuffled->id(), reference);
  }

  // Reversing the evidence must not change anything either.
  std::reverse(records.begin(), records.end());
  FABOBS_CHECK_EQ(Snapshot::build(make_request(records))->id(), reference);
}

FABOBS_TEST(snapshot, identity_changes_when_content_changes) {
  const SnapshotId reference = Snapshot::build(make_request(sample_records()))->id();

  std::vector<ObservationRecord> changed = sample_records();
  changed[0].observation.claims[0].value = *Value::text("flapping", kLimits.values);
  changed[0].observation.canonicalize();
  FABOBS_CHECK(Snapshot::build(make_request(changed))->id() != reference);

  Policy policy;
  policy.freshness.fresh_window = Duration::from_seconds(31);
  FABOBS_CHECK(Snapshot::build(make_request(sample_records(), TimePoint{kBase + 2000}, policy))->id() !=
               reference);

  FABOBS_CHECK(Snapshot::build(make_request(sample_records(), TimePoint{kBase + 2001}))->id() !=
               reference);
}

FABOBS_TEST(snapshot, publication_bookkeeping_is_not_part_of_identity) {
  const std::shared_ptr<const Snapshot> first = Snapshot::build(make_request(sample_records()));
  const std::shared_ptr<const Snapshot> second = Snapshot::build(make_request(sample_records()));
  FABOBS_CHECK_EQ(first->history_index().value(), std::uint64_t{0});
  first->set_publication(HistoryIndex(7), TimePoint{42});
  FABOBS_CHECK_EQ(second->history_index().value(), std::uint64_t{0});
  FABOBS_CHECK_EQ(first->history_index().value(), std::uint64_t{7});
  FABOBS_CHECK_EQ(first->id(), second->id());
  FABOBS_CHECK_EQ(first->recompute_digest(), first->digest());
}

FABOBS_TEST(snapshot, generation_and_epoch_are_high_water_marks) {
  std::vector<ObservationRecord> records;
  records.push_back(make_record(0, 1, 3, 1, "a", "link.state", "up", 1000));
  records.push_back(make_record(1, 1, 5, 2, "a", "link.state", "up", 1000));
  records.push_back(make_record(0, 2, 5, 1, "b", "link.state", "up", 1000));
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(records));
  FABOBS_CHECK_EQ(snapshot->generation().value(), std::uint64_t{5});
  FABOBS_CHECK_EQ(snapshot->epoch().value(), std::uint64_t{2});
  // Older generation evidence is retained and marked, never deleted.
  const AspectState* aspect = snapshot->find_aspect(device_subject("a").ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  bool saw_superseded = false;
  for (const ClaimRecord& claim : aspect->claims) {
    if (claim.superseded_generation) {
      saw_superseded = true;
    }
  }
  FABOBS_CHECK(saw_superseded);
  FABOBS_CHECK_EQ(snapshot->stats().superseded_claims >= 1, true);
}

FABOBS_TEST(snapshot, subject_and_aspect_ordering_is_deterministic) {
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(sample_records()));
  FABOBS_REQUIRE(snapshot->subjects().size() > 1);
  for (std::size_t index = 1; index < snapshot->subjects().size(); ++index) {
    const SubjectIdentity& previous = snapshot->subjects()[index - 1].identity;
    const SubjectIdentity& current = snapshot->subjects()[index].identity;
    const bool ordered = previous.kind < current.kind ||
                         (previous.kind == current.kind && previous.id < current.id);
    FABOBS_CHECK(ordered);
  }
  for (const SubjectState& subject : snapshot->subjects()) {
    for (std::size_t index = 1; index < subject.aspects.size(); ++index) {
      FABOBS_CHECK(subject.aspects[index - 1].aspect < subject.aspects[index].aspect);
    }
    for (const AspectState& aspect : subject.aspects) {
      for (std::size_t index = 1; index < aspect.claims.size(); ++index) {
        FABOBS_CHECK(!(aspect.claims[index] < aspect.claims[index - 1]));
      }
    }
  }
}

FABOBS_TEST(snapshot, one_source_that_disagrees_with_itself_is_an_update) {
  std::vector<ObservationRecord> records;
  records.push_back(make_record(0, 1, 1, 1, "a", "link.state", "up", 1000));
  records.push_back(make_record(0, 2, 1, 1, "a", "link.state", "down", 1001));
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(records));
  const AspectState* aspect = snapshot->find_aspect(device_subject("a").ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  // Two records were retained as evidence, but only the later one is current,
  // so this is a single source agreeing with itself rather than a conflict.
  FABOBS_CHECK_EQ(aspect->claims.size(), std::size_t{2});
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Known);
  FABOBS_REQUIRE(aspect->agreed_value.has_value());
  FABOBS_CHECK_EQ(aspect->agreed_value->as_text(), std::string("down"));
  std::size_t superseded = 0;
  for (const ClaimRecord& claim : aspect->claims) {
    if (claim.superseded_by_later_sequence) {
      ++superseded;
    }
  }
  FABOBS_CHECK_EQ(superseded, std::size_t{1});
}

FABOBS_TEST(snapshot, disagreement_between_sources_is_preserved) {
  std::vector<ObservationRecord> records;
  records.push_back(make_record(0, 1, 1, 1, "a", "link.state", "up", 1000));
  records.push_back(make_record(1, 1, 1, 1, "a", "link.state", "down", 1000));
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(records));
  const AspectState* aspect = snapshot->find_aspect(device_subject("a").ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Conflicting);
  FABOBS_CHECK_EQ(aspect->conflicts.size(), std::size_t{2});
  FABOBS_CHECK_EQ(aspect->claims.size(), std::size_t{2});
}

FABOBS_TEST(snapshot, a_new_incarnation_supersedes_the_previous_one) {
  std::vector<ObservationRecord> records;
  records.push_back(make_record(0, 1, 1, 1, "a", "link.state", "up", 1000));
  ObservationBuilder restarted(source_id("s0"), "s0/boot-2", 1, 1, 1, kBase + 2000);
  restarted.claim_text(device_subject("a"), "link.state", "down");
  ObservationRecord restarted_record;
  restarted_record.observation = restarted.build();
  restarted_record.received_at = TimePoint{kBase + 2000};
  records.push_back(restarted_record);

  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(records));
  const AspectState* aspect = snapshot->find_aspect(device_subject("a").ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Known);
  FABOBS_REQUIRE(aspect->agreed_value.has_value());
  FABOBS_CHECK_EQ(aspect->agreed_value->as_text(), std::string("down"));
  std::size_t superseded = 0;
  for (const ClaimRecord& claim : aspect->claims) {
    if (claim.superseded_incarnation) {
      ++superseded;
    }
  }
  FABOBS_CHECK_EQ(superseded, std::size_t{1});
  const SourceSummary* summary = snapshot->find_source(source_id("s0"));
  FABOBS_REQUIRE(summary != nullptr);
  FABOBS_CHECK_EQ(summary->incarnation.id, incarnation("s0/boot-2"));
}

FABOBS_TEST(snapshot, summary_is_stable_and_contains_the_digest) {
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(sample_records()));
  const std::string first = snapshot->canonical_summary();
  const std::string second = snapshot->canonical_summary();
  FABOBS_CHECK_EQ(first, second);
  FABOBS_CHECK(first.find(snapshot->id().to_text()) != std::string::npos);
  FABOBS_CHECK(first.find("generation 5") != std::string::npos);
  FABOBS_CHECK(first.find("truth known") != std::string::npos);

  const std::shared_ptr<const Snapshot> other = Snapshot::build(make_request(sample_records()));
  FABOBS_CHECK_EQ(other->canonical_summary(), first);
}

FABOBS_TEST(snapshot, topology_parent_is_parsed_only_when_known) {
  std::vector<ObservationRecord> records;
  const SubjectIdentity parent = SubjectIdentity::of(DeviceId::derive("device/parent"));
  const SubjectIdentity child = SubjectIdentity::of(DeviceId::derive("device/child"));
  {
    ObservationBuilder builder(source_id("s0"), "s0/boot-1", 1, 1, 1, kBase + 1000);
    builder.claim_text(child, "topology.parent", parent.typed_text());
    ObservationRecord record;
    record.observation = builder.build();
    record.received_at = TimePoint{kBase + 1000};
    records.push_back(record);
  }
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(records));
  const SubjectState* state = snapshot->find_subject(child.ref());
  FABOBS_REQUIRE(state != nullptr);
  FABOBS_REQUIRE(state->parent.has_value());
  FABOBS_CHECK_EQ(*state->parent, parent.id);
  FABOBS_CHECK_EQ(state->topology_truth, TruthState::Known);
}

FABOBS_TEST(snapshot, unparsable_parent_is_not_a_parent) {
  std::vector<ObservationRecord> records;
  const SubjectIdentity child = SubjectIdentity::of(DeviceId::derive("device/child"));
  {
    ObservationBuilder builder(source_id("s0"), "s0/boot-1", 1, 1, 1, kBase + 1000);
    builder.claim_text(child, "topology.parent", "this is not an identity");
    ObservationRecord record;
    record.observation = builder.build();
    record.received_at = TimePoint{kBase + 1000};
    records.push_back(record);
  }
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(records));
  const SubjectState* state = snapshot->find_subject(child.ref());
  FABOBS_REQUIRE(state != nullptr);
  // The aspect is still Known - the source did report something - but the
  // runtime refuses to derive a parent it cannot parse.
  FABOBS_CHECK(!state->parent.has_value());
}

FABOBS_TEST(snapshot, agreement_requires_coverage) {
  std::vector<ObservationRecord> records;
  records.push_back(make_record(0, 1, 1, 1, "a", "reachability.state", "reachable", 1000));
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(records));
  const AspectState* aspect =
      snapshot->find_aspect(device_subject("a").ref(), "reachability.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Incomplete);
  FABOBS_CHECK_EQ(aspect->coverage.required_distinct_fresh_sources, 2u);
}

FABOBS_TEST(snapshot, snapshot_stats_match_their_contents) {
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(sample_records()));
  std::uint64_t subjects = 0;
  std::uint64_t aspects = 0;
  std::uint64_t claims = 0;
  for (const SubjectState& subject : snapshot->subjects()) {
    ++subjects;
    aspects += subject.aspects.size();
    for (const AspectState& aspect : subject.aspects) {
      claims += aspect.claims.size();
    }
  }
  FABOBS_CHECK_EQ(snapshot->stats().subjects, subjects);
  FABOBS_CHECK_EQ(snapshot->stats().aspects, aspects);
  FABOBS_CHECK_EQ(snapshot->stats().claims, claims);
  FABOBS_CHECK_EQ(snapshot->stats().sources, snapshot->sources().size());
  FABOBS_CHECK_EQ(snapshot->evidence_records(), snapshot->subjects().empty() ? 0u : 24u);
}

FABOBS_TEST(snapshot, find_helpers_agree_with_linear_search) {
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request(sample_records()));
  for (const SubjectState& subject : snapshot->subjects()) {
    const SubjectState* found = snapshot->find_subject(subject.identity.ref());
    FABOBS_REQUIRE(found != nullptr);
    FABOBS_CHECK_EQ(found->identity.id, subject.identity.id);
    for (const AspectState& aspect : subject.aspects) {
      const AspectState* found_aspect =
          snapshot->find_aspect(subject.identity.ref(), aspect.aspect.view());
      FABOBS_REQUIRE(found_aspect != nullptr);
      FABOBS_CHECK_EQ(found_aspect->aspect, aspect.aspect);
    }
    FABOBS_CHECK(snapshot->find_aspect(subject.identity.ref(), "link.nonexistent") == nullptr);
  }
  FABOBS_CHECK(snapshot->find_subject(device_subject("absent").ref()) == nullptr);
  FABOBS_CHECK(snapshot->find_source(source_id("absent")) == nullptr);
}

FABOBS_TEST(snapshot, empty_evidence_still_produces_a_valid_snapshot) {
  const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(make_request({}));
  FABOBS_REQUIRE(snapshot != nullptr);
  FABOBS_CHECK_EQ(snapshot->stats().subjects, std::uint64_t{0});
  FABOBS_CHECK_EQ(snapshot->generation().value(), std::uint64_t{0});
  FABOBS_CHECK_EQ(snapshot->recompute_digest(), snapshot->digest());
  FABOBS_CHECK(!snapshot->id().is_zero());
}
