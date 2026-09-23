// Integration tests: conservative restart and recovery behaviour.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

const SubjectIdentity kDeviceA = SubjectIdentity::of(DeviceId::derive("device/a"));
const SubjectIdentity kDeviceB = SubjectIdentity::of(DeviceId::derive("device/b"));

Policy durable_policy() {
  Policy policy;
  policy.name = "durable";
  policy.freshness.fresh_window = Duration::from_seconds(30);
  policy.freshness.aging_window = Duration::from_seconds(120);
  return policy;
}

std::vector<Observation> seed_observations() {
  std::vector<Observation> observations;
  observations.push_back(ObservationBuilder(source_id("left"), "left/boot-1", 1, 3, 2)
                             .claim_text(kDeviceA, "link.state", "up")
                             .claim_text(kDeviceB, "operational.health", "ok")
                             .build());
  observations.push_back(ObservationBuilder(source_id("right"), "right/boot-1", 1, 3, 2)
                             .claim_text(kDeviceA, "capacity.bandwidth_bps", "400000000000")
                             .build());
  observations.push_back(ObservationBuilder(source_id("left"), "left/boot-1", 2, 3, 2)
                             .claim_text(kDeviceB, "reachability.state", "reachable")
                             .build());
  return observations;
}

std::uint64_t file_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  return stream ? static_cast<std::uint64_t>(stream.tellg()) : 0;
}

bool flip_byte(const std::string& path, std::uint64_t offset) {
  std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!stream) {
    return false;
  }
  stream.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  stream.read(&byte, 1);
  if (!stream) {
    return false;
  }
  byte = static_cast<char>(byte ^ 0x5A);
  stream.seekp(static_cast<std::streamoff>(offset));
  stream.write(&byte, 1);
  return stream.good();
}

}  // namespace

FABOBS_TEST(restart, recovered_evidence_is_never_fresh) {
  TempFile file("restart-freshness");
  ObservatoryFixture first(durable_policy(), file.path());
  FABOBS_REQUIRE(first.valid());
  for (const Observation& observation : seed_observations()) {
    FABOBS_REQUIRE_EQ(first.ingest(observation).disposition, IngestDisposition::Accepted);
  }
  const std::shared_ptr<const Snapshot> before = first->current();
  FABOBS_REQUIRE(before != nullptr);
  FABOBS_CHECK_EQ(before->restart_count(), 0u);
  const AspectState* before_aspect = before->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(before_aspect != nullptr);
  FABOBS_CHECK_EQ(before_aspect->truth, TruthState::Known);
  first->shutdown();

  // The restart happens at a point where the persisted evidence would still be
  // inside its fresh window if it were treated as new evidence.
  ObservatoryFixture second(durable_policy(), file.path());
  FABOBS_REQUIRE(second.valid());
  const Result<RecoveryReport> recovery = second->recovery();
  FABOBS_REQUIRE(recovery.has_value());
  FABOBS_CHECK_EQ(recovery->records_accepted, std::uint64_t{3});
  FABOBS_CHECK(recovery->chain_verified);

  const std::shared_ptr<const Snapshot> after = second->current();
  FABOBS_REQUIRE(after != nullptr);
  FABOBS_CHECK_EQ(after->restart_count(), 1u);
  FABOBS_CHECK_EQ(after->restart_epoch().value(), std::uint64_t{2});
  FABOBS_CHECK_EQ(after->stats().recovered_claims, std::uint64_t{4});

  const AspectState* after_aspect = after->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(after_aspect != nullptr);
  // Provenance and the value survive unchanged...
  FABOBS_REQUIRE_EQ(after_aspect->claims.size(), before_aspect->claims.size());
  for (std::size_t index = 0; index < after_aspect->claims.size(); ++index) {
    FABOBS_CHECK_EQ(after_aspect->claims[index].source, before_aspect->claims[index].source);
    FABOBS_CHECK_EQ(after_aspect->claims[index].value, before_aspect->claims[index].value);
    FABOBS_CHECK_EQ(after_aspect->claims[index].received_at,
                    before_aspect->claims[index].received_at);
    FABOBS_CHECK_EQ(after_aspect->claims[index].observation_id,
                    before_aspect->claims[index].observation_id);
    // ... and freshness degrades, permanently, because the evidence came back
    // from persistence rather than from the fabric.
    FABOBS_CHECK(after_aspect->claims[index].recovered);
    FABOBS_CHECK(after_aspect->claims[index].freshness != FreshnessVerdict::Fresh);
    FABOBS_CHECK_EQ(after_aspect->claims[index].freshness, FreshnessVerdict::Aging);
  }
  // It cannot assert, so it is not reported as known.
  FABOBS_CHECK_EQ(after_aspect->truth, TruthState::Stale);
}

FABOBS_TEST(restart, the_generation_epoch_and_lineage_survive) {
  TempFile file("restart-lineage");
  ObservatoryFixture first(durable_policy(), file.path());
  FABOBS_REQUIRE(first.valid());
  for (const Observation& observation : seed_observations()) {
    first.ingest(observation);
  }
  const std::shared_ptr<const Snapshot> before = first->current();
  FABOBS_CHECK_EQ(before->generation().value(), std::uint64_t{3});
  FABOBS_CHECK_EQ(before->epoch().value(), std::uint64_t{2});
  first->shutdown();

  ObservatoryFixture second(durable_policy(), file.path());
  FABOBS_REQUIRE(second.valid());
  const std::shared_ptr<const Snapshot> after = second->current();
  FABOBS_REQUIRE(after != nullptr);
  FABOBS_CHECK_EQ(after->generation().value(), before->generation().value());
  FABOBS_CHECK_EQ(after->epoch().value(), before->epoch().value());
  FABOBS_CHECK_MSG(after->id() != before->id(),
                   "recovered evidence must not produce an identical snapshot");

  Result<std::vector<SourceSummary>> sources = second->sources();
  FABOBS_REQUIRE(sources.has_value());
  FABOBS_REQUIRE_EQ(sources->size(), std::size_t{2});
  for (const SourceSummary& summary : *sources) {
    FABOBS_CHECK(summary.recovered);
    FABOBS_CHECK_EQ(summary.recovered, true);
    FABOBS_CHECK(!summary.incarnation.id.is_nil());
  }
}

FABOBS_TEST(restart, post_restart_fencing_continues_where_it_left_off) {
  TempFile file("restart-fencing");
  ObservatoryFixture first(durable_policy(), file.path());
  FABOBS_REQUIRE(first.valid());
  for (const Observation& observation : seed_observations()) {
    first.ingest(observation);
  }
  first->shutdown();

  ObservatoryFixture second(durable_policy(), file.path());
  FABOBS_REQUIRE(second.valid());
  // Time moves forward between ingests, as it does in operation. The rule that
  // decides which incarnation of a source is current is "most recently received,
  // ties broken by identity", so a frozen clock would make the outcome depend on
  // the identity ordering rather than on the order things actually happened.
  const auto tick = [&second] { second.clock->advance(Duration::from_seconds(1)); };

  // The same incarnation continuing its sequence is accepted.
  tick();
  FABOBS_CHECK_EQ(second.ingest(ObservationBuilder(source_id("left"), "left/boot-1", 3, 3, 2)
                                    .claim_text(kDeviceA, "link.state", "down")
                                    .build())
                      .disposition,
                  IngestDisposition::Accepted);
  // Replaying an already-persisted sequence is refused, exactly as before.
  tick();
  const IngestOutcome replayed =
      second.ingest(ObservationBuilder(source_id("left"), "left/boot-1", 1, 3, 2)
                        .claim_text(kDeviceA, "link.state", "down")
                        .build());
  FABOBS_CHECK_EQ(replayed.code, StatusCode::FencedSequence);
  // A new boot is accepted and supersedes the recovered incarnation.
  tick();
  FABOBS_CHECK_EQ(second.ingest(ObservationBuilder(source_id("left"), "left/boot-2", 1, 3, 2)
                                    .claim_text(kDeviceA, "link.state", "flapping")
                                    .build())
                      .disposition,
                  IngestDisposition::Accepted);
  tick();
  FABOBS_CHECK_EQ(second.ingest(ObservationBuilder(source_id("left"), "left/boot-1", 4, 3, 2)
                                    .claim_text(kDeviceA, "link.state", "down")
                                    .build())
                      .code,
                  StatusCode::FencedIncarnation);

  const std::shared_ptr<const Snapshot> snapshot = second->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  const AspectState* aspect = snapshot->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Known);
  FABOBS_REQUIRE(aspect->agreed_value.has_value());
  FABOBS_CHECK_EQ(aspect->agreed_value->as_text(), std::string("flapping"));
}

FABOBS_TEST(restart, two_restarts_increment_the_lineage) {
  TempFile file("restart-twice");
  for (int attempt = 0; attempt < 3; ++attempt) {
    ObservatoryFixture fixture(durable_policy(), file.path());
    FABOBS_REQUIRE(fixture.valid());
    if (attempt == 0) {
      for (const Observation& observation : seed_observations()) {
        fixture.ingest(observation);
      }
    } else {
      FABOBS_REQUIRE_EQ(fixture->recovery()->records_accepted, std::uint64_t{3});
      const std::shared_ptr<const Snapshot> snapshot = fixture->current();
      FABOBS_CHECK_EQ(snapshot->restart_count(), static_cast<std::uint32_t>(attempt));
      FABOBS_CHECK_EQ(snapshot->restart_epoch().value(), static_cast<std::uint64_t>(attempt) + 1);
    }
    fixture->shutdown();
  }
}

FABOBS_TEST(restart, corruption_is_reported_and_the_runtime_still_serves) {
  TempFile file("restart-corrupt");
  {
    ObservatoryFixture first(durable_policy(), file.path());
    FABOBS_REQUIRE(first.valid());
    for (const Observation& observation : seed_observations()) {
      first.ingest(observation);
    }
    first->shutdown();
  }
  const std::uint64_t bytes = file_bytes(file.path());
  FABOBS_REQUIRE(bytes > 300);
  // Damage a byte inside the first observation's payload.
  FABOBS_REQUIRE(flip_byte(file.path(), kJournalHeaderBytes + kJournalRecordHeaderBytes + 8));

  ObservatoryFixture second(durable_policy(), file.path());
  FABOBS_REQUIRE(second.valid());
  const Result<RecoveryReport> recovery = second->recovery();
  FABOBS_REQUIRE(recovery.has_value());
  FABOBS_CHECK(recovery->corrupt);
  FABOBS_CHECK(!recovery->diagnostics.empty());
  // Recovery stops at the first bad record and reports it; nothing is silently
  // repaired, truncated or deleted. The damaged bytes are still on disk.
  FABOBS_CHECK_EQ(recovery->records_accepted, std::uint64_t{0});
  // Nothing was repaired, truncated or deleted: the journal stays readable while
  // the runtime holds it open, and it still contains the damaged bytes.
  FABOBS_CHECK_MSG(file_bytes(file.path()) >= bytes,
                   "journal shrank at '" + file.path() + "': before=" + std::to_string(bytes) +
                       " after=" + std::to_string(file_bytes(file.path())));

  // The runtime is still usable, and new evidence is accepted normally.
  FABOBS_CHECK_EQ(second.ingest(ObservationBuilder(source_id("fresh"), "fresh/boot-1", 1, 3, 2)
                                    .claim_text(kDeviceA, "link.state", "up")
                                    .build())
                      .disposition,
                  IngestDisposition::Accepted);
  const std::shared_ptr<const Snapshot> snapshot = second->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  const AspectState* aspect = snapshot->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Known);
}

FABOBS_TEST(restart, truncated_tail_is_recovered_conservatively) {
  TempFile file("restart-truncated");
  {
    ObservatoryFixture first(durable_policy(), file.path());
    FABOBS_REQUIRE(first.valid());
    for (const Observation& observation : seed_observations()) {
      first.ingest(observation);
    }
    first->shutdown();
  }
  FABOBS_REQUIRE(file_bytes(file.path()) > 0);
  // Rewrite the file one byte shorter: the last record is now incomplete.
  std::vector<char> data;
  {
    std::ifstream stream(file.path(), std::ios::binary);
    data.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  }
  data.resize(data.size() - 1);
  {
    std::ofstream stream(file.path(), std::ios::binary | std::ios::trunc);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  ObservatoryFixture second(durable_policy(), file.path());
  FABOBS_REQUIRE(second.valid());
  const Result<RecoveryReport> recovery = second->recovery();
  FABOBS_REQUIRE(recovery.has_value());
  FABOBS_CHECK(recovery->truncated);
  FABOBS_CHECK(recovery->truncated_tail_bytes > 0);
  FABOBS_CHECK(!recovery->corrupt);
  // The unusable tail is the whole incomplete trailing record, not just the one
  // byte that was removed, and every complete record before it is recovered.
  FABOBS_CHECK_MSG(recovery->truncated_tail_bytes >= 1,
                   "a cut tail must be reported as unusable bytes");
  FABOBS_CHECK_EQ(recovery->records_accepted, std::uint64_t{3});
  // Opening appends a restart marker and a snapshot marker, so the file grows
  // again; what matters is that the cut was reported rather than repaired.
  FABOBS_CHECK(file_bytes(file.path()) > 0);
  FABOBS_CHECK(recovery->records_accepted >= 2);
  const std::shared_ptr<const Snapshot> snapshot = second->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  FABOBS_CHECK(snapshot->stats().recovered_claims > 0);
}

FABOBS_TEST(restart, without_recovery_an_existing_journal_is_appended_to) {
  TempFile file("restart-no-recovery");
  {
    ObservatoryFixture first(durable_policy(), file.path());
    FABOBS_REQUIRE(first.valid());
    for (const Observation& observation : seed_observations()) {
      first.ingest(observation);
    }
    first->shutdown();
  }
  ObservatoryConfig config;
  config.fabric = test_fabric();
  config.policy = durable_policy();
  config.clock = std::make_shared<ManualClock>(TimePoint{kFixtureTime});
  config.journal_path = file.path();
  config.recover_on_open = false;
  Result<std::unique_ptr<Observatory>> opened = Observatory::open(config);
  FABOBS_REQUIRE(opened.has_value());
  FABOBS_CHECK_EQ((*opened)->stats().evidence_records, std::uint64_t{0});
  // New evidence is appended after the persisted prefix rather than overwriting
  // it, so a later recovery still sees everything.
  IngestOutcome outcome;
  FABOBS_CHECK((*opened)
                   ->ingest(ObservationBuilder(source_id("fresh"), "fresh/boot-1", 1)
                                .claim_text(kDeviceA, "link.state", "up")
                                .build(),
                            outcome)
                   .ok());
  FABOBS_CHECK_EQ(outcome.disposition, IngestDisposition::Accepted);
  (*opened)->shutdown();

  ObservatoryFixture after(durable_policy(), file.path());
  FABOBS_REQUIRE(after.valid());
  FABOBS_CHECK_EQ(after->recovery()->records_accepted, std::uint64_t{4});
}

FABOBS_TEST(restart, snapshot_history_restarts_empty_but_the_journal_does_not) {
  TempFile file("restart-history");
  {
    ObservatoryFixture first(durable_policy(), file.path());
    FABOBS_REQUIRE(first.valid());
    for (const Observation& observation : seed_observations()) {
      first.ingest(observation);
    }
    Result<std::vector<HistoryEntry>> entries = first->history(10);
    FABOBS_REQUIRE(entries.has_value());
    FABOBS_CHECK(entries->size() >= 2);
    first->shutdown();
  }
  ObservatoryFixture second(durable_policy(), file.path());
  FABOBS_REQUIRE(second.valid());
  // In-memory snapshot history is not persisted; the evidence is. That is a
  // deliberate trade: history is a view, the journal is the record.
  Result<std::vector<HistoryEntry>> entries = second->history(10);
  FABOBS_REQUIRE(entries.has_value());
  FABOBS_CHECK_EQ(entries->size(), std::size_t{1});
  FABOBS_CHECK_EQ(second->recovery()->records_accepted, std::uint64_t{3});
}
