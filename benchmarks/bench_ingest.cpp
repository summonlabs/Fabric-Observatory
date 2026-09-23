// Benchmark: observation ingest and snapshot publication.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "benchmark_support.hpp"

#include "fabric_observatory/observatory.hpp"

#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabric_observatory::bench;

namespace {

constexpr std::uint32_t kSources = 8;
constexpr std::uint32_t kDevices = 512;
constexpr std::uint32_t kObservationsPerSource = 512;

const Limits kLimits{};

Observation make_observation(FabricId fabric, std::uint32_t source_index, std::uint64_t sequence,
                             std::uint32_t device_index, std::int64_t observed_at) {
  Observation observation;
  observation.schema = std::string(observation_schema());
  observation.fabric = fabric;
  observation.source = SourceId::derive("source/" + std::to_string(source_index));
  observation.incarnation = IncarnationId::derive("source/" + std::to_string(source_index) +
                                                  "/boot-1");
  observation.sequence = SourceSequence(sequence);
  observation.generation = GenerationId(7);
  observation.epoch = EpochId(2);
  observation.observed_at = TimePoint{observed_at};

  Claim claim;
  claim.subject = SubjectIdentity::of(DeviceId::derive("device/" + std::to_string(device_index)));
  claim.aspect = *well_known_aspect("operational.health");
  claim.value = *Value::text(sequence % 2 == 0 ? "ok" : "degraded", kLimits.values);
  observation.claims.push_back(std::move(claim));
  observation.canonicalize();
  return observation;
}

}  // namespace

int main() {
  const FabricId fabric = FabricId::derive("fabric/benchmark");
  auto clock = std::make_shared<ManualClock>(TimePoint{4000000000000});

  Policy policy;
  policy.name = "benchmark";
  policy.freshness.fresh_window = Duration::from_seconds(3600);
  policy.freshness.aging_window = Duration::from_seconds(7200);

  ObservatoryConfig config;
  config.fabric = fabric;
  config.policy = policy;
  config.clock = clock;

  Result<std::unique_ptr<Observatory>> opened = Observatory::open(config);
  if (!opened) {
    std::fprintf(stderr, "bench_ingest: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  Observatory& observatory = **opened;

  std::vector<Observation> observations;
  observations.reserve(static_cast<std::size_t>(kSources) * kObservationsPerSource);
  for (std::uint32_t source = 0; source < kSources; ++source) {
    for (std::uint32_t index = 0; index < kObservationsPerSource; ++index) {
      observations.push_back(make_observation(fabric, source, index + 1,
                                              (source * 97 + index) % kDevices,
                                              clock->now().nanos));
    }
  }

  std::uint64_t checksum = 0;
  Timer timer;
  std::uint64_t accepted = 0;
  for (const Observation& observation : observations) {
    IngestOutcome outcome;
    observatory.ingest(observation, outcome, IngestOptions::DeferSnapshot);
    if (outcome.disposition == IngestDisposition::Accepted) {
      ++accepted;
      checksum += outcome.observation_id.bytes[0];
    }
  }
  report("ingest_deferred", "observations", accepted, timer.elapsed_nanos(), checksum);

  std::shared_ptr<const Snapshot> snapshot;
  Timer publish_timer;
  observatory.publish(snapshot);
  const std::uint64_t publish_nanos = publish_timer.elapsed_nanos();
  checksum += snapshot->digest().bytes[0];
  report("snapshot_publish", "snapshots", 1, publish_nanos, checksum);
  std::printf("  evidence_records=%llu subjects=%llu aspects=%llu claims=%llu\n",
              static_cast<unsigned long long>(snapshot->evidence_records()),
              static_cast<unsigned long long>(snapshot->stats().subjects),
              static_cast<unsigned long long>(snapshot->stats().aspects),
              static_cast<unsigned long long>(snapshot->stats().claims));

  Timer rebuild_timer;
  std::uint64_t rebuilds = 0;
  for (int attempt = 0; attempt < 5; ++attempt) {
    observatory.refresh(snapshot->evaluation_time(), snapshot);
    ++rebuilds;
    checksum += snapshot->digest().bytes[1];
  }
  report("snapshot_rebuild", "snapshots", rebuilds, rebuild_timer.elapsed_nanos(), checksum);
  return 0;
}
