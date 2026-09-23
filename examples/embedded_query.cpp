// Fabric Observatory example: disagreeing sources and hierarchical queries.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/observatory.hpp"
#include "fabric_observatory/query.hpp"

#include <cstdio>
#include <string>

using namespace fabric_observatory;

namespace {

const Limits kLimits{};

Observation claim_for(FabricId fabric, SourceId source, const char* incarnation,
                      std::uint64_t sequence, std::int64_t observed_at,
                      SubjectIdentity subject, const char* aspect_name,
                      const std::string& value) {
  Observation observation;
  observation.schema = std::string(observation_schema());
  observation.fabric = fabric;
  observation.source = source;
  observation.incarnation = IncarnationId::derive(incarnation);
  observation.sequence = SourceSequence(sequence);
  observation.generation = GenerationId(1);
  observation.epoch = EpochId(1);
  observation.observed_at = TimePoint{observed_at};
  Claim claim;
  claim.subject = std::move(subject);
  claim.aspect = *well_known_aspect(aspect_name);
  claim.value = *Value::text(value, kLimits.values);
  observation.claims.push_back(std::move(claim));
  observation.canonicalize();
  return observation;
}

}  // namespace

int main() {
  const FabricId fabric = FabricId::derive("fabric/example");
  auto clock = std::make_shared<ManualClock>(TimePoint{2000000000000});

  ObservatoryConfig config;
  config.fabric = fabric;
  config.clock = clock;

  Result<std::unique_ptr<Observatory>> opened = Observatory::open(config);
  if (!opened) {
    std::fprintf(stderr, "example: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  Observatory& observatory = **opened;

  const std::int64_t now = clock->now().nanos;
  const SourceId left = SourceId::derive("source/left-plane");
  const SourceId right = SourceId::derive("source/right-plane");

  const SubjectIdentity site = SubjectIdentity::of(SiteId::derive("site/alpha"));
  const SubjectIdentity pod = SubjectIdentity::of(PodId::derive("pod/alpha-1"));
  const SubjectIdentity device = SubjectIdentity::of(DeviceId::derive("device/leaf-9"));

  IngestOutcome outcome;
  observatory.ingest(claim_for(fabric, left, "left/boot-1", 1, now, pod, "topology.parent",
                               site.typed_text()),
                     outcome);
  observatory.ingest(claim_for(fabric, left, "left/boot-1", 2, now, device, "topology.parent",
                               pod.typed_text()),
                     outcome);
  // The two planes disagree about the port state. The runtime keeps both
  // statements and reports the conflict instead of picking a winner.
  observatory.ingest(claim_for(fabric, left, "left/boot-1", 3, now, device, "operational.health",
                               "ok"),
                     outcome);
  observatory.ingest(claim_for(fabric, right, "right/boot-1", 1, now, device,
                               "operational.health", "degraded"),
                     outcome);

  const std::shared_ptr<const Snapshot> snapshot = observatory.current();
  std::printf("%s", snapshot->canonical_summary().c_str());

  HierarchyQuery hierarchy;
  Result<HierarchyResult> tree = observatory.hierarchy(hierarchy);
  if (tree) {
    std::printf("\n--- hierarchy ---\n");
    for (const HierarchyNode& node : tree->nodes) {
      std::printf("%*s%s depth=%u topology=%s%s\n", static_cast<int>(node.depth) * 2, "",
                  node.identity.typed_text().c_str(), node.depth,
                  std::string(to_string(node.topology_truth)).c_str(),
                  node.parent_absent ? " (parent absent from this snapshot)" : "");
    }
  }

  QueryFilter filter;
  filter.truths.push_back(TruthState::Conflicting);
  Result<QueryResult> conflicts = observatory.query(filter);
  if (conflicts) {
    std::printf("\n--- conflicting aspects ---\n%s", conflicts->canonical_summary().c_str());
  }
  return 0;
}
