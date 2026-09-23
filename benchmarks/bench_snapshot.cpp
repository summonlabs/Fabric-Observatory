// Benchmark: deterministic snapshot construction from a fixed evidence set.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "benchmark_support.hpp"

#include "fabric_observatory/snapshot.hpp"

#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabric_observatory::bench;

namespace {

constexpr std::uint32_t kSources = 6;
constexpr std::uint32_t kSubjects = 400;

const Limits kLimits{};

ObservationRecord make_record(FabricId fabric, std::uint32_t source, std::uint32_t subject,
                              std::int64_t received_at) {
  Observation observation;
  observation.schema = std::string(observation_schema());
  observation.fabric = fabric;
  observation.source = SourceId::derive("source/" + std::to_string(source));
  observation.incarnation = IncarnationId::derive("source/" + std::to_string(source) + "/boot-1");
  observation.sequence = SourceSequence(subject + 1);
  observation.generation = GenerationId(3);
  observation.epoch = EpochId(1);
  observation.observed_at = TimePoint{received_at};

  Claim claim;
  claim.subject = SubjectIdentity::of(DeviceId::derive("device/" + std::to_string(subject)));
  claim.aspect = *well_known_aspect("reachability.state");
  claim.value = *Value::text(source % 3 == 0 ? "reachable" : "unreachable", kLimits.values);
  observation.claims.push_back(std::move(claim));
  observation.canonicalize();

  ObservationRecord record;
  record.observation = std::move(observation);
  record.received_at = TimePoint{received_at};
  return record;
}

}  // namespace

int main() {
  const FabricId fabric = FabricId::derive("fabric/benchmark");
  std::vector<ObservationRecord> records;
  records.reserve(static_cast<std::size_t>(kSources) * kSubjects);
  for (std::uint32_t source = 0; source < kSources; ++source) {
    for (std::uint32_t subject = 0; subject < kSubjects; ++subject) {
      records.push_back(make_record(fabric, source, subject, 5000000000000));
    }
  }

  Policy policy;
  policy.name = "benchmark";

  SnapshotBuildRequest request;
  request.fabric = fabric;
  request.policy = policy;
  request.evaluation_time = TimePoint{5000000000000};
  request.records = records;

  std::uint64_t checksum = 0;
  std::uint64_t builds = 0;
  Timer timer;
  for (int attempt = 0; attempt < 10; ++attempt) {
    const std::shared_ptr<const Snapshot> snapshot = Snapshot::build(request);
    ++builds;
    checksum += snapshot->digest().bytes[0];
    if (attempt == 0) {
      std::printf("  subjects=%llu aspects=%llu claims=%llu conflicting_aspects=%llu\n",
                  static_cast<unsigned long long>(snapshot->stats().subjects),
                  static_cast<unsigned long long>(snapshot->stats().aspects),
                  static_cast<unsigned long long>(snapshot->stats().claims),
                  static_cast<unsigned long long>(snapshot->stats().conflicts));
    }
  }
  report("snapshot_build", "snapshots", builds, timer.elapsed_nanos(), checksum);

  // Determinism is part of the benchmark: the same request built repeatedly must
  // produce exactly one identity.
  const std::shared_ptr<const Snapshot> first = Snapshot::build(request);
  const std::shared_ptr<const Snapshot> second = Snapshot::build(request);
  std::printf("  deterministic_identity=%s digest=%s\n",
              first->id() == second->id() ? "yes" : "no", first->digest().to_short_hex().c_str());
  return 0;
}
