// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_QUERY_HPP
#define FABRIC_OBSERVATORY_QUERY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_observatory/digest.hpp"
#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/snapshot.hpp"

namespace fabric_observatory {

// A hierarchical query is always answered against one immutable snapshot, so
// every result is generation bound and repeatable.
struct QueryFilter {
  std::optional<EntityKind> kind{};
  std::vector<Domain> domains{};
  std::vector<TruthState> truths{};
  std::optional<AspectId> aspect{};
  // Restricts the result to the subjects at or below this entity in the
  // observed topology.
  std::optional<EntityId> subtree_root{};
  std::uint32_t max_depth{0};
  std::uint32_t limit{0};
  bool include_claims{true};
  bool include_conflicts{true};

  friend bool operator==(const QueryFilter&, const QueryFilter&) = default;
};

struct QueryResult {
  SnapshotId snapshot{};
  GenerationId generation{};
  EpochId epoch{};
  std::vector<SubjectState> subjects{};
  std::uint64_t scanned_subjects{0};
  std::uint64_t matched_subjects{0};
  std::uint64_t matched_aspects{0};
  bool truncated{false};
  // Digest over the canonical encoding of the returned rows. Two runs of the
  // same query against the same snapshot produce the same digest.
  Digest256 digest{};
  std::string canonical_summary() const;
};

Result<QueryResult> execute_query(const Snapshot& snapshot, const QueryFilter& filter,
                                  const Limits& limits);

struct HierarchyQuery {
  std::optional<EntityKind> root_kind{};
  std::optional<EntityId> root{};
  std::uint32_t max_depth{0};
  std::uint32_t limit{0};
  bool include_unattached{true};

  friend bool operator==(const HierarchyQuery&, const HierarchyQuery&) = default;
};

struct HierarchyNode {
  SubjectIdentity identity{};
  std::optional<EntityId> parent{};
  std::uint32_t depth{0};
  TruthState topology_truth{TruthState::Unknown};
  // True when the observed parent is not itself present in this snapshot.
  bool parent_absent{false};

  friend bool operator==(const HierarchyNode&, const HierarchyNode&) = default;
  friend auto operator<=>(const HierarchyNode&, const HierarchyNode&) = default;
};

struct HierarchyResult {
  SnapshotId snapshot{};
  std::vector<HierarchyNode> nodes{};
  std::vector<EntityId> unattached{};
  std::uint32_t max_depth_reached{0};
  std::uint32_t cycles_detected{0};
  std::uint64_t depth_limited{0};
  std::uint64_t children_of_absent_parents{0};
  bool truncated{false};
  Digest256 digest{};
};

Result<HierarchyResult> execute_hierarchy(const Snapshot& snapshot, const HierarchyQuery& query,
                                          const Limits& limits);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_QUERY_HPP
