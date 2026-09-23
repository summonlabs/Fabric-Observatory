// Fabric Observatory example: ingest, snapshot, query, explain.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// This example builds observations by hand, feeds them to an in-memory
// observatory and prints what the runtime concluded. It is the smallest
// complete walk through the boundary: the runtime is told about the fabric, it
// never goes looking.

#include "fabric_observatory/observatory.hpp"

#include <cstdio>
#include <string>

using namespace fabric_observatory;

namespace {

const Limits kLimits{};

Observation make_observation(FabricId fabric, SourceId source, const char* incarnation,
                             std::uint64_t sequence, std::uint64_t generation,
                             std::int64_t observed_at, const char* device_name,
                             const char* aspect_name, Value value) {
  Observation observation;
  observation.schema = std::string(observation_schema());
  observation.fabric = fabric;
  observation.source = source;
  observation.incarnation = IncarnationId::derive(incarnation);
  observation.sequence = SourceSequence(sequence);
  observation.generation = GenerationId(generation);
  observation.epoch = EpochId(1);
  observation.observed_at = TimePoint{observed_at};

  Claim claim;
  claim.subject = SubjectIdentity::of(DeviceId::derive(device_name));
  Result<AspectId> aspect = well_known_aspect(aspect_name);
  if (!aspect) {
    std::fprintf(stderr, "example: %s\n", aspect.status().to_string().c_str());
    return {};
  }
  claim.aspect = *aspect;
  claim.value = std::move(value);
  observation.claims.push_back(std::move(claim));
  observation.canonicalize();
  return observation;
}

}  // namespace

int main() {
  const FabricId fabric = FabricId::derive("fabric/example");
  const SourceId inventory = SourceId::derive("source/inventory");
  const SourceId telemetry = SourceId::derive("source/telemetry");

  // A manual clock keeps the example reproducible: freshness is a pure function
  // of the evaluation time and the receive time, never of how long the program
  // happened to run.
  auto clock = std::make_shared<ManualClock>(TimePoint{1000000000000});

  Policy policy;
  policy.name = "example";
  policy.freshness.fresh_window = Duration::from_seconds(30);
  policy.freshness.aging_window = Duration::from_seconds(120);

  ObservatoryConfig config;
  config.fabric = fabric;
  config.policy = policy;
  config.clock = clock;

  Result<std::unique_ptr<Observatory>> opened = Observatory::open(config);
  if (!opened) {
    std::fprintf(stderr, "example: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  Observatory& observatory = **opened;

  const std::int64_t now = clock->now().nanos;

  IngestOutcome outcome;
  observatory.ingest(make_observation(fabric, inventory, "inventory/boot-1", 1, 4, now,
                                      "device/leaf-1", "device.model",
                                      *Value::text("acme/1000", kLimits.values)),
                     outcome);
  observatory.ingest(make_observation(fabric, inventory, "inventory/boot-1", 2, 4, now,
                                      "device/leaf-1", "topology.parent",
                                      *Value::text(SubjectIdentity::of(RackId::derive("rack/a1")).typed_text(),
                                                   kLimits.values)),
                     outcome);
  observatory.ingest(make_observation(fabric, telemetry, "telemetry/boot-1", 1, 4, now,
                                      "device/leaf-1", "operational.health",
                                      *Value::text("ok", kLimits.values)),
                     outcome);

  // Two sources that only tell half the story: reachability requires
  // corroboration from two fresh sources, so this stays Incomplete rather than
  // being reported as Known or as Unknown.
  observatory.ingest(make_observation(fabric, telemetry, "telemetry/boot-1", 2, 4, now,
                                      "device/leaf-1", "reachability.state",
                                      *Value::text("reachable", kLimits.values)),
                     outcome);

  const std::shared_ptr<const Snapshot> snapshot = observatory.current();
  if (snapshot == nullptr) {
    std::fprintf(stderr, "example: no snapshot\n");
    return 1;
  }

  std::printf("%s", snapshot->canonical_summary().c_str());
  std::printf("\n--- explanation ---\n");

  const SubjectIdentity device = SubjectIdentity::of(DeviceId::derive("device/leaf-1"));
  Result<Explanation> explanation =
      observatory.explain(device.ref(), *well_known_aspect("reachability.state"));
  if (explanation) {
    std::printf("%s", explanation->to_text().c_str());
  }
  return 0;
}
