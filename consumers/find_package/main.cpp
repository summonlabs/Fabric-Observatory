// An independent downstream consumer of the installed Fabric Observatory package.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This program is not part of the Fabric Observatory build. It is configured
// and built on its own against an installed package, using nothing but
// find_package(FabricObservatory) and the public headers. It proves that the
// exported package is complete: headers, library, include directories and the
// transitive Threads dependency.

#include <fabric_observatory/observatory.hpp>
#include <fabric_observatory/persistence.hpp>

#include <cstdio>
#include <memory>
#include <string>

namespace {

const fabric_observatory::Limits kLimits{};

fabric_observatory::Observation make_observation(
    fabric_observatory::FabricId fabric, fabric_observatory::SourceId source,
    const char* incarnation_name, std::uint64_t sequence, std::int64_t observed_at,
    const char* device, const char* aspect, const char* value) {
  using namespace fabric_observatory;
  Observation observation;
  observation.schema = std::string(observation_schema());
  observation.fabric = fabric;
  observation.source = source;
  observation.incarnation = IncarnationId::derive(incarnation_name);
  observation.sequence = SourceSequence(sequence);
  observation.generation = GenerationId(1);
  observation.epoch = EpochId(1);
  observation.observed_at = TimePoint{observed_at};
  Claim claim;
  claim.subject = SubjectIdentity::of(DeviceId::derive(device));
  claim.aspect = *well_known_aspect(aspect);
  claim.value = *Value::text(value, kLimits.values);
  observation.claims.push_back(std::move(claim));
  observation.canonicalize();
  return observation;
}

}  // namespace

int main() {
  using namespace fabric_observatory;

  std::printf("fabric-observatory runtime version %s\n", std::string(version_string()).c_str());
  std::printf("observation schema %s\n", std::string(observation_schema()).c_str());

  const FabricId fabric = FabricId::derive("fabric/downstream");

  Policy policy;
  policy.name = "downstream";

  auto clock = std::make_shared<ManualClock>(TimePoint{1000000000000});

  ObservatoryConfig config;
  config.fabric = fabric;
  config.policy = policy;
  config.clock = clock;

  Result<std::unique_ptr<Observatory>> opened = Observatory::open(config);
  if (!opened) {
    std::fprintf(stderr, "cannot open the runtime: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  Observatory& observatory = **opened;

  const SourceId source = SourceId::derive("source/downstream");
  for (std::uint64_t sequence = 1; sequence <= 4; ++sequence) {
    IngestOutcome outcome;
    const Status status = observatory.ingest(
        make_observation(fabric, source, "downstream/boot-1", sequence, clock->now().nanos,
                         "device/downstream-1", "link.state", sequence % 2 == 0 ? "up" : "down"),
        outcome);
    if (!status.ok() || outcome.disposition != IngestDisposition::Accepted) {
      std::fprintf(stderr, "ingest failed: %s\n", status.to_string().c_str());
      return 1;
    }
  }

  const std::shared_ptr<const Snapshot> snapshot = observatory.current();
  if (snapshot == nullptr) {
    std::fprintf(stderr, "no snapshot was published\n");
    return 1;
  }
  if (snapshot->recompute_digest() != snapshot->digest()) {
    std::fprintf(stderr, "snapshot digest does not reproduce\n");
    return 1;
  }

  QueryFilter filter;
  filter.aspect = *well_known_aspect("link.state");
  Result<QueryResult> query = observatory.query(filter);
  if (!query || query->subjects.size() != 1) {
    std::fprintf(stderr, "query did not return the ingested subject\n");
    return 1;
  }

  std::printf("snapshot %s\n", snapshot->id().to_text().c_str());
  std::printf("subjects %llu claims %llu\n",
              static_cast<unsigned long long>(snapshot->stats().subjects),
              static_cast<unsigned long long>(snapshot->stats().claims));
  std::printf("lock audit clean: %s\n", lock_audit_report().clean ? "yes" : "no");
  std::printf("downstream consumer ok\n");
  return 0;
}
