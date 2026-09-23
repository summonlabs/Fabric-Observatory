// Unit tests: bounded resources, bounded history and bounded result sets.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

std::string make_observation_json(std::uint64_t sequence) {
  ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", sequence, 1, 1, 1000);
  builder.claim_text(device_subject("d1"), "link.state", "up");
  return builder.build().to_json_text(false);
}

}  // namespace

FABOBS_TEST(bounds, history_is_a_bounded_ring) {
  History history(3);
  FABOBS_CHECK_EQ(history.capacity(), 3u);
  FABOBS_CHECK(history.empty());
  FABOBS_CHECK(history.latest() == nullptr);

  std::vector<SnapshotId> ids;
  for (int index = 0; index < 5; ++index) {
    SnapshotBuildRequest request;
    request.fabric = test_fabric();
    request.evaluation_time = TimePoint{1000 + index};
    std::shared_ptr<const Snapshot> snapshot = Snapshot::build(request);
    ids.push_back(snapshot->id());
    history.append(snapshot, TimePoint{2000 + index});
  }

  FABOBS_CHECK_EQ(history.size(), std::size_t{3});
  FABOBS_CHECK_EQ(history.total_published(), std::uint64_t{5});
  FABOBS_CHECK_EQ(history.evicted(), std::uint64_t{2});
  FABOBS_CHECK_EQ(history.first_index().value(), std::uint64_t{3});
  FABOBS_CHECK_EQ(history.last_index().value(), std::uint64_t{5});
  FABOBS_CHECK(history.at(HistoryIndex(1)) == nullptr);
  FABOBS_CHECK(history.at(HistoryIndex(2)) == nullptr);
  FABOBS_REQUIRE(history.at(HistoryIndex(3)) != nullptr);
  FABOBS_CHECK_EQ(history.at(HistoryIndex(3))->id(), ids[2]);
  FABOBS_REQUIRE(history.latest() != nullptr);
  FABOBS_CHECK_EQ(history.latest()->id(), ids[4]);
  FABOBS_CHECK(history.find(ids[0]) == nullptr);
  FABOBS_CHECK(history.find(ids[4]) != nullptr);

  const std::vector<HistoryEntry> entries = history.entries(2);
  FABOBS_CHECK_EQ(entries.size(), std::size_t{2});
  FABOBS_CHECK_EQ(entries[0].index.value(), std::uint64_t{5});
  FABOBS_CHECK_EQ(entries[1].index.value(), std::uint64_t{4});
  const std::vector<HistoryEntry> chronological = history.entries_chronological(10);
  FABOBS_CHECK_EQ(chronological.size(), std::size_t{3});
  FABOBS_CHECK_EQ(chronological[0].index.value(), std::uint64_t{3});
}

FABOBS_TEST(bounds, history_reset_clears_state) {
  History history(2);
  for (int index = 0; index < 4; ++index) {
    SnapshotBuildRequest request;
    request.fabric = test_fabric();
    request.evaluation_time = TimePoint{1000 + index};
    history.append(Snapshot::build(request), TimePoint{1});
  }
  FABOBS_CHECK_EQ(history.evicted(), std::uint64_t{2});
  history.reset(8);
  FABOBS_CHECK_EQ(history.capacity(), 8u);
  FABOBS_CHECK_EQ(history.size(), std::size_t{0});
  FABOBS_CHECK_EQ(history.evicted(), std::uint64_t{0});
  FABOBS_CHECK_EQ(history.total_published(), std::uint64_t{0});
  FABOBS_CHECK(history.latest() == nullptr);
}

FABOBS_TEST(bounds, query_result_set_is_bounded_and_reports_truncation) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  for (std::uint64_t index = 0; index < 40; ++index) {
    ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", index + 1, 1, 1, 1000);
    builder.claim_text(device_subject("device-" + std::to_string(index)), "link.state", "up");
    fixture.ingest(builder.build(), IngestOptions::DeferSnapshot);
  }
  std::shared_ptr<const Snapshot> snapshot;
  fixture->publish(snapshot);
  FABOBS_CHECK_EQ(snapshot->stats().subjects, std::uint64_t{40});

  QueryFilter filter;
  filter.limit = 10;
  Result<QueryResult> limited = fixture->query(filter);
  FABOBS_REQUIRE(limited.has_value());
  FABOBS_CHECK_EQ(limited->subjects.size(), std::size_t{10});
  FABOBS_CHECK(limited->truncated);
  FABOBS_CHECK_EQ(limited->scanned_subjects, std::uint64_t{11});

  QueryFilter unlimited;
  Result<QueryResult> all = fixture->query(unlimited);
  FABOBS_REQUIRE(all.has_value());
  FABOBS_CHECK_EQ(all->subjects.size(), std::size_t{40});
  FABOBS_CHECK(!all->truncated);
}

FABOBS_TEST(bounds, observation_limits_are_enforced) {
  Limits limits;
  limits.max_claims_per_observation = 2;
  ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", 1, 1, 1, 1000);
  for (int index = 0; index < 3; ++index) {
    builder.claim_text(device_subject("d" + std::to_string(index)), "link.state", "up");
  }
  Observation observation = builder.build();
  FABOBS_CHECK_EQ(observation.validate(limits).status().code(), StatusCode::TooLarge);

  limits.max_claims_per_observation = 512;
  FABOBS_CHECK(observation.validate(limits).has_value());

  Limits small_metadata;
  small_metadata.max_metadata_entries = 1;
  ObservationBuilder with_metadata(source_id("bounds"), "bounds/boot-1", 2, 1, 1, 1000);
  with_metadata.meta("first", "1").meta("second", "2");
  FABOBS_CHECK_EQ(with_metadata.build().validate(small_metadata).status().code(), StatusCode::TooLarge);

  // The builder silently drops metadata that exceeds the bound, so construct the
  // over-sized case directly.
  Observation direct = with_metadata.build();
  std::vector<std::pair<std::string, std::string>> entries = {{"a", "1"}, {"b", "2"}};
  Result<Metadata> built = Metadata::make(std::move(entries), small_metadata);
  FABOBS_CHECK_EQ(built.status().code(), StatusCode::TooLarge);
}

FABOBS_TEST(bounds, observation_requires_core_provenance) {
  ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", 1, 1, 1, 1000);
  builder.claim_text(device_subject("d"), "link.state", "up");
  Observation observation = builder.build();
  FABOBS_CHECK(observation.validate(kLimits).has_value());

  Observation no_sequence = observation;
  no_sequence.sequence = SourceSequence(0);
  FABOBS_CHECK_EQ(no_sequence.validate(kLimits).status().code(), StatusCode::InvalidArgument);

  Observation no_source = observation;
  no_source.source = SourceId{};
  FABOBS_CHECK_EQ(no_source.validate(kLimits).status().code(), StatusCode::InvalidArgument);

  Observation no_incarnation = observation;
  no_incarnation.incarnation = IncarnationId{};
  FABOBS_CHECK_EQ(no_incarnation.validate(kLimits).status().code(), StatusCode::InvalidArgument);

  Observation no_schema = observation;
  no_schema.schema.clear();
  FABOBS_CHECK_EQ(no_schema.validate(kLimits).status().code(), StatusCode::InvalidArgument);

  Observation duplicate = observation;
  duplicate.claims.push_back(duplicate.claims.front());
  duplicate.canonicalize();
  FABOBS_CHECK_EQ(duplicate.validate(kLimits).status().code(), StatusCode::InvalidArgument);

  Observation zero_causal = observation;
  CausalRef reference;
  zero_causal.causal.push_back(reference);
  FABOBS_CHECK_EQ(zero_causal.validate(kLimits).status().code(), StatusCode::InvalidArgument);
}

FABOBS_TEST(bounds, batch_size_is_bounded) {
  Policy policy;
  policy.limits.max_batch_observations = 2;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());

  std::vector<Observation> observations;
  for (std::uint64_t index = 0; index < 3; ++index) {
    ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", index + 1, 1, 1, 1000);
    builder.claim_text(device_subject("d"), "link.state", "up");
    observations.push_back(builder.build());
  }
  IngestBatchReport report;
  const Status status = fixture->ingest_batch(observations, report);
  FABOBS_CHECK_EQ(status.code(), StatusCode::TooLarge);
  FABOBS_CHECK_EQ(report.outcomes.size(), std::size_t{0});
}

FABOBS_TEST(bounds, evidence_growth_is_bounded_and_visible) {
  Policy policy;
  policy.limits.max_evidence_records = 4;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  for (std::uint64_t index = 0; index < 10; ++index) {
    ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", index + 1, 1, 1,
                               1000 + static_cast<std::int64_t>(index));
    builder.claim_text(device_subject("d" + std::to_string(index)), "link.state", "up");
    fixture.ingest(builder.build(), IngestOptions::DeferSnapshot);
  }
  const ObservatoryStats stats = fixture->stats();
  FABOBS_CHECK_EQ(stats.evidence_records, std::uint64_t{4});
  FABOBS_CHECK_EQ(stats.evidence_evicted, std::uint64_t{6});
  std::shared_ptr<const Snapshot> snapshot;
  fixture->publish(snapshot);
  FABOBS_CHECK(snapshot->evidence_truncated());
  FABOBS_CHECK_EQ(snapshot->evidence_records(), std::uint64_t{4});
}

FABOBS_TEST(bounds, journal_growth_is_bounded_by_rotation) {
  TempFile file("bounds-journal");
  JournalOptions options;
  options.fabric = test_fabric();
  options.max_bytes = 4096;
  options.max_record_bytes = 2048;
  options.max_files = 3;
  options.fsync_on_append = false;

  Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), options);
  FABOBS_REQUIRE(opened.has_value());
  Journal& journal = **opened;
  for (std::uint64_t index = 0; index < 200; ++index) {
    ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", index + 1, 1, 1, 1000);
    builder.claim_text(device_subject("device-" + std::to_string(index)), "link.state", "up");
    ObservationRecord record;
    record.observation = builder.build();
    record.received_at = TimePoint{1000 + static_cast<std::int64_t>(index)};
    FABOBS_REQUIRE(journal.append(record).has_value());
  }
  journal.close();
  FABOBS_CHECK(journal.rotations() > 0);
  FABOBS_CHECK(journal.size_bytes() <= options.max_bytes);

  JournalOptions read_options = options;
  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), read_options);
  FABOBS_REQUIRE(reopened.has_value());
  const RecoveryReport& report = (*reopened)->recovery();
  FABOBS_CHECK(report.chain_verified);
  FABOBS_CHECK(!report.corrupt);
  FABOBS_CHECK(report.parts_recovered <= options.max_files);
  FABOBS_CHECK_MSG(!(*reopened)->recovered_records().empty(), report.to_text());
  FABOBS_CHECK((*reopened)->recovered_records().size() < 200);
}

FABOBS_TEST(bounds, value_bounds_from_limits_are_used_by_the_runtime) {
  Policy policy;
  policy.limits.values.max_text_bytes = 4;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", 1, 1, 1, 1000);
  builder.claim_text(device_subject("d"), "link.state", "a very long value indeed");
  IngestOutcome outcome = fixture.ingest(builder.build());
  FABOBS_CHECK_EQ(outcome.disposition, IngestDisposition::Rejected);
}

FABOBS_TEST(bounds, retained_records_is_bounded_by_the_request) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  for (std::uint64_t index = 0; index < 10; ++index) {
    ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", index + 1, 1, 1, 1000);
    builder.claim_text(device_subject("d"), "link.state", std::to_string(index));
    fixture.ingest(builder.build(), IngestOptions::DeferSnapshot);
  }
  Result<std::vector<ObservationRecord>> records = fixture->retained_records(3);
  FABOBS_REQUIRE(records.has_value());
  FABOBS_CHECK_EQ(records->size(), std::size_t{3});
  FABOBS_CHECK_EQ(records->back().observation.sequence.value(), std::uint64_t{10});
}

FABOBS_TEST(bounds, json_wire_size_is_bounded) {
  Limits limits;
  limits.max_claims_per_observation = 1;
  ObservationBuilder builder(source_id("bounds"), "bounds/boot-1", 1, 1, 1, 1000);
  builder.claim_text(device_subject("d1"), "link.state", "up");
  builder.claim_text(device_subject("d2"), "link.state", "up");
  const std::string text = builder.build().to_json_text(false);
  Result<Observation> parsed = Observation::from_json_text(text, JsonLimits{}, limits);
  FABOBS_CHECK_EQ(parsed.status().code(), StatusCode::TooLarge);
}
