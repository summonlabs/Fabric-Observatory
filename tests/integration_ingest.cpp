// Integration tests: the complete ingest path, its fences and its composition.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

const SubjectIdentity kDeviceA = SubjectIdentity::of(DeviceId::derive("device/a"));
const SubjectIdentity kDeviceB = SubjectIdentity::of(DeviceId::derive("device/b"));

SourceDescriptor scoped_source(const std::string& name, std::vector<AspectId> aspects,
                               SourceAuthority authority) {
  SourceDescriptor descriptor;
  descriptor.id = source_id(name);
  descriptor.name = name;
  descriptor.authority_name = "unit-test";
  descriptor.aspects = std::move(aspects);
  descriptor.max_authority = authority;
  descriptor.schema = std::string(observation_schema());
  return descriptor;
}

}  // namespace

FABOBS_TEST(ingest, accepts_a_well_formed_observation) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const IngestOutcome outcome =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                         .claim_text(kDeviceA, "link.state", "up")
                         .build());
  FABOBS_CHECK_EQ(outcome.disposition, IngestDisposition::Accepted);
  FABOBS_CHECK_EQ(outcome.code, StatusCode::Ok);
  FABOBS_CHECK_EQ(outcome.claims_accepted, 1u);
  FABOBS_CHECK_EQ(outcome.claims_fenced, 0u);

  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  const AspectState* aspect = snapshot->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Known);
  FABOBS_REQUIRE(aspect->agreed_value.has_value());
  FABOBS_CHECK_EQ(aspect->agreed_value->as_text(), std::string("up"));
}

FABOBS_TEST(ingest, fences_stale_generation_epoch_and_sequence) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1, 3, 2)
                                     .claim_text(kDeviceA, "link.state", "up")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);

  // The same source may not go backwards in generation or epoch.
  const IngestOutcome older_generation =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 2, 2, 2)
                         .claim_text(kDeviceA, "link.state", "up")
                         .build());
  FABOBS_CHECK_EQ(older_generation.code, StatusCode::FencedStaleGeneration);
  const IngestOutcome older_epoch =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 3, 3, 1)
                         .claim_text(kDeviceA, "link.state", "up")
                         .build());
  FABOBS_CHECK_EQ(older_epoch.code, StatusCode::FencedStaleEpoch);
  const IngestOutcome replays_sequence =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1, 3, 2)
                         .claim_text(kDeviceA, "link.state", "down")
                         .build());
  FABOBS_CHECK_EQ(replays_sequence.code, StatusCode::FencedSequence);

  const ObservatoryStats stats = fixture->stats();
  FABOBS_CHECK_EQ(stats.observations_accepted, std::uint64_t{1});
  FABOBS_CHECK_EQ(stats.observations_rejected, std::uint64_t{3});
}

FABOBS_TEST(ingest, fences_a_replayed_boot_incarnation) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                     .claim_text(kDeviceA, "link.state", "up")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
  // A restart is a new incarnation and is accepted.
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-2", 1)
                                     .claim_text(kDeviceA, "link.state", "down")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
  // The old boot coming back is a replay and is refused.
  const IngestOutcome replayed =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 2)
                         .claim_text(kDeviceA, "link.state", "up")
                         .build());
  FABOBS_CHECK_EQ(replayed.disposition, IngestDisposition::Rejected);
  FABOBS_CHECK_EQ(replayed.code, StatusCode::FencedIncarnation);
}

FABOBS_TEST(ingest, fences_fabric_schema_and_clock_anomalies) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());

  Observation other_fabric = ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                 .claim_text(kDeviceA, "link.state", "up")
                                 .build();
  other_fabric.fabric = FabricId::derive("fabric/other");
  other_fabric.canonicalize();
  FABOBS_CHECK_EQ(fixture.ingest(other_fabric).code, StatusCode::FabricMismatch);

  Observation other_schema = ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                 .claim_text(kDeviceA, "link.state", "up")
                                 .build();
  other_schema.schema = "fabric-observatory/observation/99";
  FABOBS_CHECK_EQ(fixture.ingest(other_schema).code, StatusCode::Unsupported);

  Observation future = ObservationBuilder(source_id("s"), "s/boot-1", 1, 1, 1,
                                          fixture.clock->now().nanos + 60000000000)
                           .claim_text(kDeviceA, "link.state", "up")
                           .build();
  FABOBS_CHECK_EQ(fixture.ingest(future).code, StatusCode::ClockInconsistent);

  // An observation that arrives long after it was observed is accepted and is
  // simply stale; the runtime does not invent evidence about why it was late.
  Observation old = ObservationBuilder(source_id("s"), "s/boot-1", 1, 1, 1, 1)
                        .claim_text(kDeviceA, "link.state", "up")
                        .build();
  FABOBS_CHECK_EQ(fixture.ingest(old).disposition, IngestDisposition::Accepted);
  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  const AspectState* aspect = snapshot->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Stale);
}

FABOBS_TEST(ingest, refuses_unregistered_sources_when_asked_to) {
  Policy policy;
  policy.ingest.auto_register_sources = false;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  const IngestOutcome outcome =
      fixture.ingest(ObservationBuilder(source_id("stranger"), "stranger/boot-1", 1)
                         .claim_text(kDeviceA, "link.state", "up")
                         .build());
  FABOBS_CHECK_EQ(outcome.code, StatusCode::SourceUnknown);

  SourceDescriptor descriptor;
  descriptor.id = source_id("stranger");
  descriptor.name = "stranger";
  descriptor.max_authority = SourceAuthority::Reported;
  FABOBS_CHECK(fixture->register_source(descriptor).ok());
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("stranger"), "stranger/boot-1", 1)
                                     .claim_text(kDeviceA, "link.state", "up")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
}

FABOBS_TEST(ingest, declares_and_enforces_source_scope) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  std::vector<AspectId> scope = {*well_known_aspect("link.state")};
  FABOBS_CHECK(fixture->register_source(scoped_source("scoped", scope, SourceAuthority::Reported))
                   .ok());

  const IngestOutcome accepted =
      fixture.ingest(ObservationBuilder(source_id("scoped"), "scoped/boot-1", 1)
                         .claim_text(kDeviceA, "link.state", "up")
                         .build());
  FABOBS_CHECK_EQ(accepted.disposition, IngestDisposition::Accepted);

  // A claim outside the declared scope is fenced as a claim, not as an
  // observation, and the fenced claim stays visible.
  const IngestOutcome mixed =
      fixture.ingest(ObservationBuilder(source_id("scoped"), "scoped/boot-1", 2)
                         .claim_text(kDeviceA, "link.state", "down")
                         .claim(kDeviceB, "device.model", *Value::text("acme/1000", kLimits.values))
                         .build());
  FABOBS_CHECK_EQ(mixed.disposition, IngestDisposition::Accepted);
  FABOBS_CHECK_EQ(mixed.claims_accepted, 1u);
  FABOBS_CHECK_EQ(mixed.claims_fenced, 1u);

  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  FABOBS_REQUIRE_EQ(snapshot->fenced_claims().size(), std::size_t{1});
  FABOBS_CHECK_EQ(snapshot->fenced_claims().front().fence, StatusCode::FencedAuthority);
  FABOBS_CHECK_EQ(snapshot->fenced_claims().front().aspect.view(), std::string_view("device.model"));
  const AspectState* model = snapshot->find_aspect(kDeviceB.ref(), "device.model");
  FABOBS_REQUIRE(model != nullptr);
  // The fenced claim is retained as evidence of what was said, and it can never
  // assert, so the aspect stays stale rather than becoming known.
  FABOBS_CHECK_EQ(model->truth, TruthState::Stale);
  FABOBS_REQUIRE_EQ(model->claims.size(), std::size_t{1});
  FABOBS_CHECK(model->claims.front().fenced);

  // An observation whose claims are all fenced is refused outright.
  const IngestOutcome all_fenced =
      fixture.ingest(ObservationBuilder(source_id("scoped"), "scoped/boot-1", 3)
                         .claim(kDeviceB, "device.model", *Value::text("acme/2000", kLimits.values))
                         .build());
  FABOBS_CHECK_EQ(all_fenced.disposition, IngestDisposition::Fenced);
  FABOBS_CHECK_EQ(all_fenced.code, StatusCode::FencedAuthority);
}

FABOBS_TEST(ingest, fences_replayed_claim_revisions) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                     .claim_text(kDeviceA, "link.state", "up", true, 5)
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
  const IngestOutcome replayed =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 2)
                         .claim_text(kDeviceA, "link.state", "down", true, 4)
                         .build());
  FABOBS_CHECK_EQ(replayed.disposition, IngestDisposition::Fenced);
  // The specific reason survives: this was a claim revision replay.
  FABOBS_CHECK_EQ(replayed.code, StatusCode::FencedRevision);

  const IngestOutcome advanced =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 3)
                         .claim_text(kDeviceA, "link.state", "down", true, 6)
                         .build());
  FABOBS_CHECK_EQ(advanced.disposition, IngestDisposition::Accepted);
}

FABOBS_TEST(ingest, preserves_disagreement_between_sources) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("left"), "left/boot-1", 1)
                     .claim_text(kDeviceA, "link.state", "up")
                     .build());
  fixture.ingest(ObservationBuilder(source_id("right"), "right/boot-1", 1)
                     .claim_text(kDeviceA, "link.state", "down")
                     .build());
  const AspectState* aspect = fixture->current()->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Conflicting);
  FABOBS_CHECK_EQ(aspect->conflicts.size(), std::size_t{2});
  FABOBS_CHECK_EQ(aspect->claims.size(), std::size_t{2});
  FABOBS_CHECK(!aspect->agreed_value.has_value());
}

FABOBS_TEST(ingest, partial_visibility_is_incomplete_not_known) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("only"), "only/boot-1", 1)
                     .claim_text(kDeviceA, "reachability.state", "reachable")
                     .build());
  const AspectState* aspect =
      fixture->current()->find_aspect(kDeviceA.ref(), "reachability.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Incomplete);
  FABOBS_CHECK_EQ(aspect->coverage.distinct_fresh_sources, 1u);
  FABOBS_CHECK_EQ(aspect->coverage.required_distinct_fresh_sources, 2u);

  fixture.ingest(ObservationBuilder(source_id("second"), "second/boot-1", 1)
                     .claim_text(kDeviceA, "reachability.state", "reachable")
                     .build());
  aspect = fixture->current()->find_aspect(kDeviceA.ref(), "reachability.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Incomplete);
}

FABOBS_TEST(ingest, authority_is_capped_by_the_source_declaration) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  std::vector<AspectId> scope = {*well_known_aspect("reachability.state")};
  FABOBS_CHECK(fixture
                   ->register_source(
                       scoped_source("inferred", scope, SourceAuthority::Inferred))
                   .ok());
  fixture.ingest(ObservationBuilder(source_id("inferred"), "inferred/boot-1", 1)
                     .claim_text(kDeviceA, "reachability.state", "reachable")
                     .build());
  fixture.ingest(ObservationBuilder(source_id("reported"), "reported/boot-1", 1)
                     .claim_text(kDeviceA, "reachability.state", "reachable")
                     .build());
  const AspectState* aspect =
      fixture->current()->find_aspect(kDeviceA.ref(), "reachability.state");
  FABOBS_REQUIRE(aspect != nullptr);
  // Two fresh sources that agree, but neither reaches the corroborated
  // authority the aspect requires.
  FABOBS_CHECK_EQ(aspect->coverage.distinct_fresh_sources, 2u);
  FABOBS_CHECK_EQ(aspect->coverage.best_fresh_authority, SourceAuthority::Reported);
  FABOBS_CHECK_EQ(aspect->coverage.required_authority, SourceAuthority::Corroborated);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Incomplete);
}

FABOBS_TEST(ingest, explicit_unsupported_declaration_is_reported_as_unsupported) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                     .claim(kDeviceA, "congestion.level", Value::null(), false)
                     .build());
  const AspectState* aspect = fixture->current()->find_aspect(kDeviceA.ref(), "congestion.level");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Unsupported);
}

FABOBS_TEST(ingest, unknown_aspects_are_refused_when_the_policy_says_so) {
  Policy policy;
  policy.ingest.reject_unknown_aspects = true;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  const IngestOutcome outcome =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                         .claim_text(kDeviceA, "link.custom_metric", "1")
                         .build());
  FABOBS_CHECK_EQ(outcome.disposition, IngestDisposition::Fenced);
  FABOBS_CHECK_EQ(outcome.code, StatusCode::Unsupported);

  Policy permissive;
  permissive.ingest.reject_unknown_aspects = false;
  ObservatoryFixture lenient(permissive);
  FABOBS_REQUIRE(lenient.valid());
  FABOBS_CHECK_EQ(lenient.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                     .claim_text(kDeviceA, "link.custom_metric", "1")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
}

FABOBS_TEST(ingest, strict_generation_fencing_is_available_and_recorded) {
  Policy strict;
  strict.ingest.strict_generation_fence = true;
  ObservatoryFixture fixture(strict);
  FABOBS_REQUIRE(fixture.valid());
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("a"), "a/boot-1", 1, 5, 1)
                                     .claim_text(kDeviceA, "link.state", "up")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
  // With strict fencing a lower generation from a *different* source is refused
  // at ingest rather than retained and marked superseded.
  const IngestOutcome late =
      fixture.ingest(ObservationBuilder(source_id("b"), "b/boot-1", 1, 4, 1)
                         .claim_text(kDeviceA, "link.state", "down")
                         .build());
  FABOBS_CHECK_EQ(late.code, StatusCode::FencedStaleGeneration);

  // The permissive default keeps it, marks it superseded and never lets it assert.
  ObservatoryFixture permissive;
  FABOBS_REQUIRE(permissive.valid());
  FABOBS_CHECK_EQ(permissive.ingest(ObservationBuilder(source_id("a"), "a/boot-1", 1, 5, 1)
                                        .claim_text(kDeviceA, "link.state", "up")
                                        .build())
                      .disposition,
                  IngestDisposition::Accepted);
  FABOBS_CHECK_EQ(permissive.ingest(ObservationBuilder(source_id("b"), "b/boot-1", 1, 4, 1)
                                        .claim_text(kDeviceA, "link.state", "down")
                                        .build())
                      .disposition,
                  IngestDisposition::Accepted);
  const AspectState* aspect = permissive->current()->find_aspect(kDeviceA.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Known);
  FABOBS_REQUIRE(aspect->agreed_value.has_value());
  FABOBS_CHECK_EQ(aspect->agreed_value->as_text(), std::string("up"));
  FABOBS_CHECK_EQ(permissive->current()->stats().superseded_claims, std::uint64_t{1});
}

FABOBS_TEST(ingest, hostile_generation_jump_is_refused) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const IngestOutcome jump =
      fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1,
                                        static_cast<std::uint64_t>(1) << 40, 1)
                         .claim_text(kDeviceA, "link.state", "up")
                         .build());
  FABOBS_CHECK_EQ(jump.code, StatusCode::OutOfRange);
}

FABOBS_TEST(ingest, final_state_is_independent_of_interleaving) {
  // Each source keeps its own order - a source's sequence is a fence, and
  // re-ordering inside one source's stream is a replay, not a reordering. What
  // must not matter is how the sources interleave.
  constexpr std::uint32_t kSources = 4;
  constexpr std::uint64_t kPerSource = 6;
  std::vector<std::vector<Observation>> streams(kSources);
  for (std::uint32_t source = 0; source < kSources; ++source) {
    for (std::uint64_t sequence = 1; sequence <= kPerSource; ++sequence) {
      streams[source].push_back(
          ObservationBuilder(source_id("s" + std::to_string(source)),
                             "s" + std::to_string(source) + "/boot-1", sequence, 2, 1)
              .claim_text(device_subject("d" + std::to_string(sequence)), "operational.health",
                          (source + sequence) % 2 == 0 ? "ok" : "degraded")
              .build());
    }
  }

  std::vector<Observation> sequential;
  for (std::uint32_t source = 0; source < kSources; ++source) {
    for (const Observation& observation : streams[source]) {
      sequential.push_back(observation);
    }
  }

  ObservatoryFixture reference;
  FABOBS_REQUIRE(reference.valid());
  for (const Observation& observation : sequential) {
    reference.ingest(observation, IngestOptions::DeferSnapshot);
  }
  std::shared_ptr<const Snapshot> expected;
  reference->publish(expected);

  std::mt19937 generator(4242u);
  for (int attempt = 0; attempt < 8; ++attempt) {
    std::vector<std::size_t> order(kSources);
    for (std::uint32_t index = 0; index < kSources; ++index) {
      order[index] = index;
    }
    std::vector<Observation> interleaved;
    for (std::uint64_t round = 0; round < kPerSource; ++round) {
      std::shuffle(order.begin(), order.end(), generator);
      for (const std::size_t source : order) {
        interleaved.push_back(streams[source][static_cast<std::size_t>(round)]);
      }
    }
    ObservatoryFixture candidate;
    FABOBS_REQUIRE(candidate.valid());
    for (const Observation& observation : interleaved) {
      candidate.ingest(observation, IngestOptions::DeferSnapshot);
    }
    const std::shared_ptr<const Snapshot> actual = candidate->current();
    FABOBS_REQUIRE(actual != nullptr);
    FABOBS_CHECK_EQ(actual->id(), expected->id());
  }
}

FABOBS_TEST(ingest, duplicate_submission_is_idempotent) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const Observation observation = ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                      .claim_text(kDeviceA, "link.state", "up")
                                      .build();
  FABOBS_CHECK_EQ(fixture.ingest(observation).disposition, IngestDisposition::Accepted);
  const SnapshotId first = fixture->current()->id();
  for (int attempt = 0; attempt < 5; ++attempt) {
    const IngestOutcome repeat = fixture.ingest(observation);
    FABOBS_CHECK_EQ(repeat.disposition, IngestDisposition::Duplicate);
  }
  FABOBS_CHECK_EQ(fixture->current()->id(), first);
  FABOBS_CHECK_EQ(fixture->stats().evidence_records, std::uint64_t{1});
}

FABOBS_TEST(ingest, deferred_publication_only_rebuilds_when_asked) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
    fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", sequence)
                       .claim_text(device_subject("d" + std::to_string(sequence)), "link.state", "up")
                       .build(),
                   IngestOptions::DeferSnapshot);
  }
  // Opening published the empty initial view; the deferred ingests have not
  // been reflected yet.
  FABOBS_CHECK(fixture->stats().published);
  FABOBS_CHECK(fixture->stats().dirty);
  FABOBS_CHECK_EQ(fixture->stats().snapshots_published, std::uint64_t{1});
  FABOBS_CHECK_EQ(fixture->current()->stats().subjects, std::uint64_t{5});
  FABOBS_CHECK_EQ(fixture->stats().snapshots_published, std::uint64_t{2});
  FABOBS_CHECK(!fixture->stats().dirty);

  std::shared_ptr<const Snapshot> snapshot;
  FABOBS_CHECK(fixture->publish(snapshot).ok());
  FABOBS_REQUIRE(snapshot != nullptr);
  FABOBS_CHECK_EQ(snapshot->stats().subjects, std::uint64_t{5});
  // Publishing again with no new evidence is a no-op, and it is counted.
  FABOBS_CHECK_EQ(fixture->stats().snapshots_published, std::uint64_t{2});
  FABOBS_CHECK_EQ(fixture->stats().publications_skipped_unchanged, std::uint64_t{1});
}

FABOBS_TEST(ingest, refresh_reevaluates_freshness_without_new_evidence) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                     .claim_text(kDeviceA, "link.state", "up")
                     .build());
  const std::shared_ptr<const Snapshot> fresh = fixture->current();
  FABOBS_REQUIRE(fresh != nullptr);
  FABOBS_CHECK_EQ(fresh->find_aspect(kDeviceA.ref(), "link.state")->truth, TruthState::Known);

  std::shared_ptr<const Snapshot> aged;
  FABOBS_CHECK(fixture->refresh(fresh->evaluation_time() + Duration::from_seconds(60), aged).ok());
  FABOBS_REQUIRE(aged != nullptr);
  FABOBS_CHECK(aged->id() != fresh->id());
  FABOBS_CHECK_EQ(aged->find_aspect(kDeviceA.ref(), "link.state")->truth, TruthState::Stale);
  // The evidence itself is unchanged; only the evaluation time moved.
  FABOBS_CHECK_EQ(aged->find_aspect(kDeviceA.ref(), "link.state")->claims.size(), std::size_t{1});
  FABOBS_CHECK_EQ(aged->find_aspect(kDeviceA.ref(), "link.state")->claims.front().value.as_text(),
                  std::string("up"));
}

FABOBS_TEST(ingest, causal_references_never_claim_causation) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                     .claim_text(kDeviceA, "failure.state", "link-down")
                     .build());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 2)
                     .claim_text(kDeviceB, "failure.state", "port-errors")
                     .causal("observed after the link event", CausalStrength::TemporallyPrecedes, 0x11)
                     .build());

  Result<Explanation> explanation = fixture->explain(kDeviceB.ref(), *well_known_aspect("failure.state"));
  FABOBS_REQUIRE(explanation.has_value());
  FABOBS_CHECK_EQ(explanation->causality.size(), std::size_t{1});
  FABOBS_CHECK(explanation->causality.front().find("temporally-precedes") != std::string::npos);
  FABOBS_CHECK(explanation->causality.front().find("no causal relationship is asserted") !=
               std::string::npos);
}

FABOBS_TEST(ingest, explanation_is_deterministic_and_specific) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("left"), "left/boot-1", 1)
                     .claim_text(kDeviceA, "link.state", "up")
                     .build());
  fixture.ingest(ObservationBuilder(source_id("right"), "right/boot-1", 1)
                     .claim_text(kDeviceA, "link.state", "down")
                     .build());

  Result<Explanation> first = fixture->explain(kDeviceA.ref(), *well_known_aspect("link.state"));
  Result<Explanation> second = fixture->explain(kDeviceA.ref(), *well_known_aspect("link.state"));
  FABOBS_REQUIRE(first.has_value());
  FABOBS_REQUIRE(second.has_value());
  FABOBS_CHECK_EQ(first->to_text(), second->to_text());
  FABOBS_CHECK_EQ(first->truth, TruthState::Conflicting);
  FABOBS_CHECK(first->to_text().find("conflicting") != std::string::npos);
  FABOBS_CHECK(first->evidence.size() >= 2);

  // An absent subject explains itself as absent rather than as a zero value.
  Result<Explanation> absent =
      fixture->explain(device_subject("never-seen").ref(), std::nullopt);
  FABOBS_REQUIRE(absent.has_value());
  FABOBS_CHECK_EQ(absent->truth, TruthState::Unknown);
  FABOBS_CHECK(absent->to_text().find("absence of evidence") != std::string::npos);
}

FABOBS_TEST(ingest, sources_report_their_lineage_and_freshness) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1).claim_text(kDeviceA, "link.state", "up").build());
  Result<std::vector<SourceSummary>> sources = fixture->sources();
  FABOBS_REQUIRE(sources.has_value());
  FABOBS_CHECK_EQ(sources->size(), std::size_t{1});
  FABOBS_CHECK_EQ(sources->front().id, source_id("s"));
  FABOBS_CHECK_EQ(sources->front().incarnation.id, incarnation("s/boot-1"));
  FABOBS_CHECK_EQ(sources->front().last_sequence.value(), std::uint64_t{1});
  FABOBS_CHECK_EQ(sources->front().accepted, std::uint64_t{1});
  FABOBS_CHECK_EQ(sources->front().retained_records, std::uint64_t{1});
}
