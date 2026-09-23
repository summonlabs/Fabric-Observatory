// Adversarial input tests: hostile, malformed and boundary-sized input.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every case here is a deliberate attempt to make the runtime misbehave. The
// expected outcome is always a named refusal or a conservative truth state,
// never a crash, a silent acceptance or an invented fact.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

const SubjectIdentity kDevice = SubjectIdentity::of(DeviceId::derive("device/a"));

std::string base_observation_json() {
  return "{\"schema\":\"fabric-observatory/observation/1\","
         "\"fabric\":\"" + test_fabric().to_text() + "\","
         "\"source\":\"" + source_id("adversary").to_text() + "\","
         "\"incarnation\":\"" + incarnation("adversary/boot-1").to_text() + "\","
         "\"sequence\":\"1\",\"generation\":\"1\",\"epoch\":\"1\",\"observed_at\":\"" +
         std::to_string(kFixtureTime) + "\","
         "\"claims\":[{\"subject\":\"" + kDevice.typed_text() +
         "\",\"aspect\":\"link.state\",\"value\":{\"k\":\"text\",\"v\":\"up\"}}]}";
}

}  // namespace

FABOBS_TEST(adversarial, malformed_json_is_refused_with_a_named_status) {
  const std::vector<std::string> inputs = {
      "",
      "   ",
      "{",
      "}",
      "[]",
      "null",
      "\"text\"",
      "{\"op\":}",
      "{\"schema\":\"x\"",
      "{'schema':'x'}",
      "{\"schema\":\"x\",}",
      "{\"schema\":\"x\"} trailing",
      "{\"schema\":\"\\uD800\"}",
      "{\"schema\":\"\\q\"}",
      "{\"schema\":\"x\",\"sequence\":01}",
      "{\"schema\":\"x\",\"sequence\":1e}",
  };
  for (const std::string& input : inputs) {
    const Result<Observation> parsed = Observation::from_json_text(input, JsonLimits{}, kLimits);
    FABOBS_CHECK_MSG(!parsed.has_value(), "input should have been refused: " + input);
  }
}

FABOBS_TEST(adversarial, deep_and_wide_json_is_bounded) {
  std::string deep = "{\"v\":";
  for (int level = 0; level < 64; ++level) {
    deep += "[";
  }
  for (int level = 0; level < 64; ++level) {
    deep += "]";
  }
  deep += "}";
  const Result<JsonValue> parsed = parse_json(deep, JsonLimits{});
  FABOBS_CHECK_EQ(parsed.status().code(), StatusCode::DepthExceeded);

  std::string wide = "[";
  for (int index = 0; index < 100000; ++index) {
    wide += "1,";
  }
  wide += "1]";
  JsonLimits tight;
  tight.max_nodes = 1000;
  FABOBS_CHECK_EQ(parse_json(wide, tight).status().code(), StatusCode::TooLarge);

  JsonLimits bytes;
  bytes.max_input_bytes = 16;
  FABOBS_CHECK_EQ(parse_json(wide, bytes).status().code(), StatusCode::TooLarge);

  JsonLimits strings;
  strings.max_string_bytes = 8;
  FABOBS_CHECK_EQ(parse_json("\"aaaaaaaaaaaaaaaaaaaa\"", strings).status().code(),
                  StatusCode::TooLarge);
}

FABOBS_TEST(adversarial, value_envelopes_are_validated_not_trusted) {
  const std::vector<std::string> envelopes = {
      "{\"k\":\"uint\",\"v\":\"not a number\"}",
      "{\"k\":\"uint\",\"v\":\"-1\"}",
      "{\"k\":\"int\",\"v\":\"99999999999999999999999\"}",
      "{\"k\":\"bool\",\"v\":\"true\"}",
      "{\"k\":\"real\",\"v\":\"nan\"}",
      "{\"k\":\"list\",\"v\":{}}",
      "{\"k\":\"map\",\"v\":[{\"k\":1,\"v\":{\"k\":\"null\"}}]}",
      "{\"k\":\"blob\",\"v\":\"zz\"}",
      "{\"k\":\"mystery\",\"v\":null}",
      "{\"v\":{\"k\":\"null\"}}",
      "[]",
  };
  for (const std::string& envelope : envelopes) {
    const Result<JsonValue> parsed = parse_json(envelope, JsonLimits{});
    FABOBS_REQUIRE(parsed.has_value());
    const Result<Value> value = value_from_json(*parsed, kLimits.values);
    FABOBS_CHECK_MSG(!value.has_value(), "envelope should have been refused: " + envelope);
  }
}

FABOBS_TEST(adversarial, observations_missing_provenance_are_refused) {
  const std::vector<std::string> mutations = {
      "\"schema\":\"\",",
      "",
      "\"fabric\":\"fab:00000000000000000000000000000000\",",
      "\"source\":\"fab:00000000000000000000000000000000\",",
      "\"incarnation\":\"fab:00000000000000000000000000000000\",",
      "\"sequence\":\"0\",",
      "\"observed_at\":\"0\",",
      "\"claims\":[],",
  };
  const std::string base = base_observation_json();
  for (const std::string& mutation : mutations) {
    std::string candidate = base;
    if (mutation.empty()) {
      // Remove the schema member entirely.
      const std::size_t start = candidate.find("\"schema\"");
      const std::size_t end = candidate.find(',', start);
      candidate.erase(start, end - start + 1);
    } else if (mutation == "\"observed_at\":\"0\",") {
      const std::size_t start = candidate.find("\"observed_at\":\"");
      const std::size_t end = candidate.find('"', start + 15) + 2;
      candidate.replace(start, end - start, mutation);
    } else {
      const std::string key = mutation.substr(0, mutation.find(':') + 1);
      const std::size_t start = candidate.find(key);
      FABOBS_REQUIRE(start != std::string::npos);
      const std::size_t end = candidate.find(',', start);
      candidate.replace(start, end - start + 1, mutation);
    }
    const Result<Observation> parsed = Observation::from_json_text(candidate, JsonLimits{}, kLimits);
    if (mutation == "\"observed_at\":\"0\",") {
      // A zero observation time is legal and simply very old.
      FABOBS_CHECK(parsed.has_value());
      continue;
    }
    FABOBS_CHECK_MSG(!parsed.has_value(), "mutation should have been refused: " + mutation);
  }
}

FABOBS_TEST(adversarial, hostile_claim_shapes_are_refused) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());

  // An unknown domain cannot be parsed into an aspect at all.
  ObservationBuilder unknown_domain(source_id("s"), "s/boot-1", 1);
  const Result<AspectId> parsed = AspectId::parse("nonsense.thing");
  FABOBS_CHECK_EQ(parsed.status().code(), StatusCode::Unsupported);

  // A subject identity that is not reversible is refused at parse time.
  FABOBS_CHECK(!SubjectIdentity::from_text("device/not-hex").has_value());
  FABOBS_CHECK(!SubjectIdentity::from_text("nonsense/device").has_value());
  FABOBS_CHECK(!EntityId::from_text("e:zzzz").has_value());
  FABOBS_CHECK(!LinkId::from_text("link:nonsense").has_value());

  // A claim with a nil subject is refused.
  Observation observation = ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                .claim_text(kDevice, "link.state", "up")
                                .build();
  observation.claims.front().subject = SubjectIdentity{};
  FABOBS_CHECK_EQ(observation.validate(kLimits).status().code(), StatusCode::InvalidArgument);

  // Metadata keys are restricted and bounded.
  const std::vector<std::pair<std::string, std::string>> bad_metadata = {
      {"UPPER", "value"},
      {"has space", "value"},
      {"key", std::string(1000, 'x')},
  };
  for (const auto& entry : bad_metadata) {
    FABOBS_CHECK(!Metadata::make({entry}, kLimits).has_value());
  }
  FABOBS_CHECK(!Metadata::make({{"a", "1"}, {"a", "2"}}, kLimits).has_value());
}

FABOBS_TEST(adversarial, a_source_may_not_assert_beyond_its_declaration) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  SourceDescriptor descriptor;
  descriptor.id = source_id("limited");
  descriptor.name = "limited";
  descriptor.aspects = {*well_known_aspect("link.state")};
  descriptor.max_authority = SourceAuthority::Reported;
  FABOBS_CHECK(fixture->register_source(descriptor).ok());

  // Declaring the same aspect twice is a malformed declaration.
  SourceDescriptor duplicated = descriptor;
  duplicated.aspects.push_back(*well_known_aspect("link.state"));
  FABOBS_CHECK_EQ(fixture->register_source(duplicated).code(),
                  StatusCode::InvalidArgument);

  SourceDescriptor unnamed = descriptor;
  unnamed.name = std::string(200, 'n');
  FABOBS_CHECK_EQ(fixture->register_source(unnamed).code(), StatusCode::TooLarge);

  SourceDescriptor bad_authority = descriptor;
  bad_authority.authority_name = "not a name!";
  FABOBS_CHECK_EQ(fixture->register_source(bad_authority).code(),
                  StatusCode::InvalidArgument);

  SourceDescriptor nil;
  FABOBS_CHECK_EQ(fixture->register_source(nil).code(), StatusCode::InvalidArgument);
}

FABOBS_TEST(adversarial, the_source_bound_is_enforced) {
  Policy policy;
  policy.limits.max_sources = 2;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("a"), "a/boot-1", 1)
                                     .claim_text(kDevice, "link.state", "up")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("b"), "b/boot-1", 1)
                                     .claim_text(kDevice, "link.state", "up")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("c"), "c/boot-1", 1)
                                     .claim_text(kDevice, "link.state", "up")
                                     .build())
                      .code,
                  StatusCode::ResourceExhausted);
}

FABOBS_TEST(adversarial, unbounded_claim_counts_are_refused) {
  Policy policy;
  policy.limits.max_claims_per_observation = 4;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  ObservationBuilder builder(source_id("s"), "s/boot-1", 1);
  for (int index = 0; index < 8; ++index) {
    builder.claim_text(device_subject("d" + std::to_string(index)), "link.state", "up");
  }
  FABOBS_CHECK_EQ(fixture.ingest(builder.build()).code, StatusCode::TooLarge);
}

FABOBS_TEST(adversarial, a_flood_of_claims_is_bounded_in_the_snapshot) {
  Policy policy;
  policy.limits.max_claims_per_subject_aspect = 2;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  for (std::uint32_t source = 0; source < 12; ++source) {
    fixture.ingest(ObservationBuilder(source_id("s" + std::to_string(source)),
                                      "s" + std::to_string(source) + "/boot-1", 1)
                       .claim_text(kDevice, "link.state", source % 2 == 0 ? "up" : "down")
                       .build(),
                   IngestOptions::DeferSnapshot);
  }
  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  const AspectState* aspect = snapshot->find_aspect(kDevice.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->claims.size(), std::size_t{2});
  FABOBS_CHECK(aspect->claims_truncated);
  FABOBS_CHECK_EQ(snapshot->stats().sources, std::uint64_t{12});
}

FABOBS_TEST(adversarial, a_flood_of_subjects_is_bounded) {
  Policy policy;
  policy.limits.max_subjects = 8;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  ObservationBuilder builder(source_id("s"), "s/boot-1", 1);
  for (int index = 0; index < 32; ++index) {
    builder.claim_text(device_subject("d" + std::to_string(index)), "link.state", "up");
  }
  // The observation itself is well formed, so it is accepted; the snapshot is
  // what is bounded, and it says so rather than growing without limit.
  FABOBS_CHECK_EQ(fixture.ingest(builder.build()).disposition, IngestDisposition::Accepted);
  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  FABOBS_CHECK_EQ(snapshot->stats().subjects, std::uint64_t{8});
  FABOBS_CHECK(snapshot->evidence_truncated());
}

FABOBS_TEST(adversarial, clock_skew_and_extreme_times_are_handled) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const std::int64_t now = fixture.clock->now().nanos;

  const auto build_at = [&](std::int64_t observed_at, std::uint64_t sequence) {
    return ObservationBuilder(source_id("s"), "s/boot-1", sequence, 1, 1, observed_at)
        .claim_text(kDevice, "link.state", "up")
        .build();
  };

  FABOBS_CHECK_EQ(fixture.ingest(build_at(now + 3600000000000LL, 1)).code,
                  StatusCode::ClockInconsistent);
  FABOBS_CHECK_EQ(fixture.ingest(build_at(std::numeric_limits<std::int64_t>::max(), 2)).code,
                  StatusCode::ClockInconsistent);
  FABOBS_CHECK_EQ(fixture.ingest(build_at(std::numeric_limits<std::int64_t>::min(), 3))
                      .disposition,
                  IngestDisposition::Accepted);
  const AspectState* aspect = fixture->current()->find_aspect(kDevice.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Stale);
}

FABOBS_TEST(adversarial, duplicate_and_contradictory_claims_in_one_observation) {
  Observation observation = ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                .claim_text(kDevice, "link.state", "up")
                                .build();
  Observation duplicate = observation;
  duplicate.claims.push_back(duplicate.claims.front());
  duplicate.canonicalize();
  FABOBS_CHECK_EQ(duplicate.validate(kLimits).status().code(), StatusCode::InvalidArgument);
}

FABOBS_TEST(adversarial, oversized_and_garbage_canonical_input_is_refused) {
  // A canonical observation that is truncated at every possible point must be
  // refused rather than partially accepted.
  ObservationBuilder builder(source_id("s"), "s/boot-1", 1);
  builder.claim_text(kDevice, "link.state", "up").meta("collector", "adversarial");
  CanonicalEncoder encoder;
  builder.build().encode(encoder);

  for (std::size_t length = 0; length < encoder.buffer().size(); length += 7) {
    CanonicalDecoder decoder(ByteSpan(encoder.buffer().data(), length));
    const Result<Observation> decoded = Observation::decode(decoder, kLimits);
    FABOBS_CHECK_MSG(!decoded.has_value(),
                     "a truncated canonical observation must be refused at length " +
                         std::to_string(length));
  }

  // Flipping bytes must be refused or produce a different, still valid value -
  // never a crash and never the original value by accident.
  std::vector<std::byte> mutated = encoder.buffer();
  for (std::size_t index = 0; index < mutated.size(); ++index) {
    const std::byte original = mutated[index];
    mutated[index] = static_cast<std::byte>(std::to_integer<std::uint8_t>(original) ^ 0xFF);
    CanonicalDecoder decoder(ByteSpan(mutated.data(), mutated.size()));
    const Result<Observation> decoded = Observation::decode(decoder, kLimits);
    if (decoded.has_value()) {
      FABOBS_CHECK(decoded->content_digest() != builder.build().content_digest());
    }
    mutated[index] = original;
  }
}

FABOBS_TEST(adversarial, a_forged_record_length_is_refused_before_it_is_allocated) {
  TempFile file("forged-length");
  JournalOptions options;
  options.fabric = test_fabric();
  options.fsync_on_append = false;
  options.max_bytes = 1u << 20;
  options.max_record_bytes = 4096;
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), options);
    FABOBS_REQUIRE(opened.has_value());
    ObservationBuilder builder(source_id("s"), "s/boot-1", 1);
    builder.claim_text(kDevice, "link.state", "up");
    ObservationRecord record;
    record.observation = builder.build();
    record.received_at = TimePoint{kFixtureTime};
    FABOBS_REQUIRE((*opened)->append(record).has_value());
    (*opened)->close();
  }

  // Rewrite the first record's declared payload length to a huge value. The
  // bound is checked before anything is allocated for the payload.
  const std::uint64_t offset = kJournalHeaderBytes;
  {
    std::fstream stream(file.path(), std::ios::binary | std::ios::in | std::ios::out);
    FABOBS_REQUIRE(stream.good());
    const char huge[4] = {static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF),
                          static_cast<char>(0xFF)};
    stream.seekp(static_cast<std::streamoff>(offset));
    stream.write(huge, 4);
  }

  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), options);
  FABOBS_REQUIRE(reopened.has_value());
  const RecoveryReport& report = (*reopened)->recovery();
  FABOBS_CHECK(report.corrupt);
  FABOBS_CHECK_EQ((*reopened)->recovered_records().size(), std::size_t{0});
  bool named_length = false;
  for (const RecoveryDiagnostic& diagnostic : report.diagnostics) {
    if (diagnostic.code == "record-length") {
      named_length = true;
    }
  }
  FABOBS_CHECK(named_length);
}

FABOBS_TEST(adversarial, a_single_part_journal_stays_bounded) {
  TempFile file("single-part");
  JournalOptions options;
  options.fabric = test_fabric();
  options.fsync_on_append = false;
  options.max_bytes = 2048;
  options.max_record_bytes = 1024;
  options.max_files = 1;
  Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), options);
  FABOBS_REQUIRE(opened.has_value());
  for (std::uint64_t sequence = 1; sequence <= 60; ++sequence) {
    ObservationBuilder builder(source_id("s"), "s/boot-1", sequence, 1, 1,
                               kFixtureTime + static_cast<std::int64_t>(sequence));
    builder.claim_text(device_subject("d" + std::to_string(sequence)), "link.state", "up");
    ObservationRecord record;
    record.observation = builder.build();
    record.received_at = TimePoint{kFixtureTime + static_cast<std::int64_t>(sequence)};
    FABOBS_REQUIRE((*opened)->append(record).has_value());
  }
  FABOBS_CHECK((*opened)->size_bytes() <= options.max_bytes);
  (*opened)->close();

  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), options);
  FABOBS_REQUIRE(reopened.has_value());
  FABOBS_CHECK(!(*reopened)->recovery().corrupt);
  FABOBS_CHECK(!(*reopened)->recovered_records().empty());
  FABOBS_CHECK((*reopened)->recovered_records().size() < 60);
}

FABOBS_TEST(adversarial, identifiers_reject_overlong_and_hostile_text) {
  FABOBS_CHECK_EQ(AspectId::parse(std::string(200, 'a')).status().code(), StatusCode::TooLarge);
  FABOBS_CHECK_EQ(AspectId::parse(std::string("link.state\0hidden", 16)).status().code(),
                  StatusCode::InvalidArgument);
  FABOBS_CHECK(!FabricId::from_text(std::string(40, 'a')).has_value());
  FABOBS_CHECK(!SnapshotId::from_text("snap:not-hex").has_value());
  FABOBS_CHECK(!Digest256::from_hex(std::string(64, 'z')).has_value());
  FABOBS_CHECK(!u128_from_hex("0").has_value());
  FABOBS_CHECK(!PortId::from_text("port:dev/99999999999999999999").has_value());
  FABOBS_CHECK(!PortId::from_text("port:").has_value());
}

FABOBS_TEST(adversarial, rejected_input_does_not_poison_the_runtime) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const std::vector<std::string> garbage = {
      "{}", "{\"op\":\"\"}", "not json at all", "{\"schema\":123}", "[]", "null",
  };
  for (const std::string& input : garbage) {
    Result<Observation> parsed = Observation::from_json_text(input, JsonLimits{}, kLimits);
    if (parsed.has_value()) {
      IngestOutcome outcome;
      fixture->ingest(*parsed, outcome);
      FABOBS_CHECK(outcome.disposition == IngestDisposition::Rejected);
    }
  }
  // A well formed observation still works afterwards.
  FABOBS_CHECK_EQ(fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1)
                                     .claim_text(kDevice, "link.state", "up")
                                     .build())
                      .disposition,
                  IngestDisposition::Accepted);
  FABOBS_CHECK_EQ(fixture->current()->find_aspect(kDevice.ref(), "link.state")->truth,
                  TruthState::Known);
}

FABOBS_TEST(adversarial, explanation_of_a_hostile_snapshot_is_still_deterministic) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  // Contradictory, unsupported and stale evidence at once, from many sources.
  for (std::uint32_t source = 0; source < 6; ++source) {
    fixture.ingest(ObservationBuilder(source_id("s" + std::to_string(source)),
                                      "s" + std::to_string(source) + "/boot-1", 1)
                       .claim_text(kDevice, "link.state", source % 2 == 0 ? "up" : "down")
                       .build(),
                   IngestOptions::DeferSnapshot);
  }
  fixture.ingest(ObservationBuilder(source_id("silent"), "silent/boot-1", 1)
                     .claim(kDevice, "link.state", Value::null(), false)
                     .build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> snapshot;
  FABOBS_CHECK(fixture->publish(snapshot).ok());
  FABOBS_REQUIRE(snapshot != nullptr);

  const Result<Explanation> first = explain_subject(*snapshot, kDevice, std::nullopt, kLimits);
  const Result<Explanation> second = explain_subject(*snapshot, kDevice, std::nullopt, kLimits);
  FABOBS_REQUIRE(first.has_value());
  FABOBS_REQUIRE(second.has_value());
  FABOBS_CHECK_EQ(first->to_text(), second->to_text());
  FABOBS_CHECK(first->to_text().find("0x") == std::string::npos);
  FABOBS_CHECK_EQ(first->truth, TruthState::Conflicting);
}
