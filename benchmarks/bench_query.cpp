// Benchmark: query and diff against a published snapshot.
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
constexpr std::uint32_t kDevices = 1024;

const Limits kLimits{};

Observation make_observation(FabricId fabric, std::uint32_t source, std::uint64_t sequence,
                             std::uint32_t device) {
  Observation observation;
  observation.schema = std::string(observation_schema());
  observation.fabric = fabric;
  observation.source = SourceId::derive("source/" + std::to_string(source));
  observation.incarnation = IncarnationId::derive("source/" + std::to_string(source) + "/boot-1");
  observation.sequence = SourceSequence(sequence);
  observation.generation = GenerationId(11);
  observation.epoch = EpochId(1);
  observation.observed_at = TimePoint{6000000000000};

  Claim claim;
  claim.subject = SubjectIdentity::of(DeviceId::derive("device/" + std::to_string(device)));
  claim.aspect = *well_known_aspect("link.state");
  claim.value = *Value::text("up", kLimits.values);
  observation.claims.push_back(std::move(claim));
  observation.canonicalize();
  return observation;
}

}  // namespace

int main() {
  const FabricId fabric = FabricId::derive("fabric/benchmark");
  auto clock = std::make_shared<ManualClock>(TimePoint{6000000000000});

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
    std::fprintf(stderr, "bench_query: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  Observatory& observatory = **opened;

  std::vector<Observation> observations;
  observations.reserve(kSources * kDevices);
  for (std::uint32_t source = 0; source < kSources; ++source) {
    for (std::uint32_t device = 0; device < kDevices; ++device) {
      observations.push_back(make_observation(fabric, source, device + 1, device));
    }
  }
  IngestBatchReport batch;
  observatory.ingest_batch(observations, batch, IngestOptions::PublishSnapshot);

  std::uint64_t checksum = 0;
  Timer full_timer;
  std::uint64_t full_queries = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    QueryFilter filter;
    Result<QueryResult> result = observatory.query(filter);
    if (result) {
      ++full_queries;
      checksum += static_cast<std::uint64_t>(result->matched_subjects);
      checksum += result->digest.bytes[0];
    }
  }
  report("query_all_subjects", "queries", full_queries, full_timer.elapsed_nanos(), checksum);

  Timer aspect_timer;
  std::uint64_t aspect_queries = 0;
  for (int attempt = 0; attempt < 50; ++attempt) {
    QueryFilter filter;
    filter.aspect = *well_known_aspect("link.state");
    filter.limit = 64;
    Result<QueryResult> result = observatory.query(filter);
    if (result) {
      ++aspect_queries;
      checksum += static_cast<std::uint64_t>(result->matched_aspects);
    }
  }
  report("query_single_aspect", "queries", aspect_queries, aspect_timer.elapsed_nanos(), checksum);

  {
    std::shared_ptr<const Snapshot> snapshot;
    observatory.refresh(TimePoint{6000000000000 + 1}, snapshot);
  }
  Timer diff_timer;
  std::uint64_t diffs = 0;
  for (int attempt = 0; attempt < 50; ++attempt) {
    Result<SnapshotDiff> diff = observatory.diff_latest(1);
    if (diff) {
      ++diffs;
      checksum += static_cast<std::uint64_t>(diff->changes.size());
    }
  }
  report("diff_latest", "diffs", diffs, diff_timer.elapsed_nanos(), checksum);

  Timer hierarchy_timer;
  std::uint64_t hierarchies = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    Result<HierarchyResult> result = observatory.hierarchy(HierarchyQuery{});
    if (result) {
      ++hierarchies;
      checksum += static_cast<std::uint64_t>(result->nodes.size());
    }
  }
  report("hierarchy", "traversals", hierarchies, hierarchy_timer.elapsed_nanos(), checksum);
  return 0;
}
