// Shared fixtures for the Fabric Observatory test suite.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_TESTS_RUNTIME_FIXTURE_HPP
#define FABRIC_OBSERVATORY_TESTS_RUNTIME_FIXTURE_HPP

#include "fabric_observatory/ingest.hpp"
#include "fabric_observatory/json_io.hpp"
#include "fabric_observatory/observatory.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace fabobs_test {

inline const fabric_observatory::Limits kLimits{};

// The single instant every fixture starts from. Freshness is measured from the
// earlier of an observation's observation and receive times, so a fixture that
// wants fresh evidence must say when it was observed.
inline constexpr std::int64_t kFixtureTime = 1000000000000;

inline fabric_observatory::FabricId test_fabric() {
  return fabric_observatory::FabricId::derive("fabric/test");
}

inline fabric_observatory::SourceId source_id(const std::string& name) {
  return fabric_observatory::SourceId::derive("source/" + name);
}

inline fabric_observatory::IncarnationId incarnation(const std::string& name) {
  return fabric_observatory::IncarnationId::derive("incarnation/" + name);
}

inline fabric_observatory::SubjectIdentity device_subject(const std::string& name) {
  return fabric_observatory::SubjectIdentity::of(
      fabric_observatory::DeviceId::derive("device/" + name));
}

struct ObservationBuilder {
  fabric_observatory::Observation observation{};

  ObservationBuilder(fabric_observatory::SourceId source, const std::string& boot,
                     std::uint64_t sequence, std::uint64_t generation = 1,
                     std::uint64_t epoch = 1, std::int64_t observed_at = kFixtureTime) {
    observation.schema = std::string(fabric_observatory::observation_schema());
    observation.fabric = test_fabric();
    observation.source = source;
    observation.incarnation = incarnation(boot);
    observation.sequence = fabric_observatory::SourceSequence(sequence);
    observation.generation = fabric_observatory::GenerationId(generation);
    observation.epoch = fabric_observatory::EpochId(epoch);
    observation.observed_at = fabric_observatory::TimePoint{observed_at};
  }

  ObservationBuilder& claim(const fabric_observatory::SubjectIdentity& subject,
                            const char* aspect, fabric_observatory::Value value,
                            bool supported = true, std::uint64_t revision = 0) {
    fabric_observatory::Claim entry;
    entry.subject = subject;
    fabric_observatory::Result<fabric_observatory::AspectId> parsed =
        fabric_observatory::AspectId::parse(aspect);
    if (parsed) {
      entry.aspect = *parsed;
    }
    entry.value = std::move(value);
    entry.supported = supported;
    entry.revision = fabric_observatory::ClaimRevision(revision);
    observation.claims.push_back(std::move(entry));
    return *this;
  }

  ObservationBuilder& claim_text(const fabric_observatory::SubjectIdentity& subject,
                                 const char* aspect, const std::string& value) {
    return claim(subject, aspect, *fabric_observatory::Value::text(value, kLimits.values));
  }

  ObservationBuilder& claim_text(const fabric_observatory::SubjectIdentity& subject,
                                 const char* aspect, const std::string& value, bool supported,
                                 std::uint64_t revision) {
    return claim(subject, aspect, *fabric_observatory::Value::text(value, kLimits.values), supported,
                 revision);
  }

  ObservationBuilder& causal(const char* basis, fabric_observatory::CausalStrength strength,
                             std::uint8_t fill) {
    fabric_observatory::CausalRef reference;
    reference.antecedent.bytes.fill(fill);
    reference.strength = strength;
    reference.basis = basis;
    observation.causal.push_back(std::move(reference));
    return *this;
  }

  ObservationBuilder& meta(const std::string& key, const std::string& value) {
    std::vector<std::pair<std::string, std::string>> entries(
        observation.metadata.entries().begin(), observation.metadata.entries().end());
    entries.emplace_back(key, value);
    fabric_observatory::Result<fabric_observatory::Metadata> built =
        fabric_observatory::Metadata::make(std::move(entries), kLimits);
    if (built) {
      observation.metadata = std::move(*built);
    }
    return *this;
  }

  fabric_observatory::Observation build() {
    observation.canonicalize();
    return observation;
  }
};

struct ObservatoryFixture {
  std::shared_ptr<fabric_observatory::ManualClock> clock =
      std::make_shared<fabric_observatory::ManualClock>(
          fabric_observatory::TimePoint{kFixtureTime});
  std::unique_ptr<fabric_observatory::Observatory> observatory{};

  explicit ObservatoryFixture(const fabric_observatory::Policy& policy =
                                  fabric_observatory::Policy{},
                              const std::string& journal_path = std::string{}) {
    fabric_observatory::ObservatoryConfig config;
    config.fabric = test_fabric();
    config.policy = policy;
    config.clock = clock;
    config.journal_path = journal_path;
    fabric_observatory::Result<std::unique_ptr<fabric_observatory::Observatory>> opened =
        fabric_observatory::Observatory::open(config);
    if (opened) {
      observatory = std::move(*opened);
    }
  }

  ~ObservatoryFixture() {
    if (observatory != nullptr) {
      observatory->shutdown();
    }
  }

  fabric_observatory::Observatory& operator*() { return *observatory; }
  fabric_observatory::Observatory* operator->() { return observatory.get(); }
  bool valid() const { return observatory != nullptr; }

  fabric_observatory::IngestOutcome ingest(const fabric_observatory::Observation& observation,
                                           fabric_observatory::IngestOptions options =
                                               fabric_observatory::IngestOptions::PublishSnapshot) {
    fabric_observatory::IngestOutcome outcome;
    observatory->ingest(observation, outcome, options);
    return outcome;
  }
};

// Temporary file helper: every test that touches persistence uses a unique path
// and removes it on destruction, so a failing test never leaves debris.
class TempFile {
 public:
  explicit TempFile(const std::string& tag) {
    path_ = "fabobs-test-" + tag + ".journal";
    std::remove(path_.c_str());
  }

  ~TempFile() {
    std::remove(path_.c_str());
    for (int index = 1; index <= 12; ++index) {
      std::remove((path_ + "." + std::to_string(index)).c_str());
    }
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::string part(int index) const {
    return path_ + "." + std::to_string(index);
  }

 private:
  std::string path_;
};

}  // namespace fabobs_test

#endif  // FABRIC_OBSERVATORY_TESTS_RUNTIME_FIXTURE_HPP
