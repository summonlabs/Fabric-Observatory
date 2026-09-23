// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/query.hpp"

#include "fabric_observatory/canonical.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace fabric_observatory {

namespace {

bool domain_selected(const std::vector<Domain>& domains, Domain domain) {
  if (domains.empty()) {
    return true;
  }
  return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

bool truth_selected(const std::vector<TruthState>& truths, TruthState truth) {
  if (truths.empty()) {
    return true;
  }
  return std::find(truths.begin(), truths.end(), truth) != truths.end();
}

void encode_claim(CanonicalEncoder& encoder, const ClaimRecord& claim) {
  encoder.u64(claim.source.value().hi);
  encoder.u64(claim.source.value().lo);
  encoder.u64(claim.sequence.value());
  encoder.u8(static_cast<std::uint8_t>(claim.freshness));
  encoder.boolean(claim.supported);
  encoder.boolean(claim.superseded_generation);
  encoder.boolean(claim.superseded_epoch);
  encoder.boolean(claim.superseded_incarnation);
  encoder.boolean(claim.superseded_by_later_sequence);
  claim.value.encode(encoder);
}

void encode_subject(CanonicalEncoder& encoder, const SubjectState& subject,
                    bool include_claims) {
  encoder.u8(static_cast<std::uint8_t>(subject.identity.kind));
  encoder.u64(subject.identity.id.value().hi);
  encoder.u64(subject.identity.id.value().lo);
  encoder.text(subject.identity.typed_text());
  encoder.boolean(subject.parent.has_value());
  if (subject.parent.has_value()) {
    encoder.u64(subject.parent->value().hi);
    encoder.u64(subject.parent->value().lo);
  }
  encoder.u8(static_cast<std::uint8_t>(subject.worst_truth));
  encoder.u64(static_cast<std::uint64_t>(subject.aspects.size()));
  for (const AspectState& aspect : subject.aspects) {
    encoder.text(aspect.aspect.view());
    encoder.u8(static_cast<std::uint8_t>(aspect.truth));
    encoder.boolean(aspect.agreed_value.has_value());
    if (aspect.agreed_value.has_value()) {
      aspect.agreed_value->encode(encoder);
    }
    encoder.u32(aspect.coverage.distinct_fresh_sources);
    encoder.u32(aspect.coverage.required_distinct_fresh_sources);
    encoder.u8(static_cast<std::uint8_t>(aspect.coverage.best_fresh_authority));
    encoder.u8(static_cast<std::uint8_t>(aspect.coverage.required_authority));
    encoder.boolean(aspect.coverage.satisfied);
    if (include_claims) {
      encoder.u64(static_cast<std::uint64_t>(aspect.claims.size()));
      for (const ClaimRecord& claim : aspect.claims) {
        encode_claim(encoder, claim);
      }
    } else {
      encoder.u64(0);
    }
    encoder.u64(static_cast<std::uint64_t>(aspect.conflicts.size()));
    for (const ConflictGroup& conflict : aspect.conflicts) {
      conflict.value.encode(encoder);
      encoder.u64(static_cast<std::uint64_t>(conflict.sources.size()));
      for (const SourceId& source : conflict.sources) {
        encoder.u64(source.value().hi);
        encoder.u64(source.value().lo);
      }
    }
  }
}

// Ancestor walk used by subtree filtering. The walk is bounded so that a
// malformed topology (including a cycle) cannot make a query unbounded.
std::optional<std::uint32_t> depth_from_root(const std::map<EntityId, EntityId>& parents,
                                             const EntityId& start, const EntityId& root,
                                             std::uint32_t max_depth) {
  if (start == root) {
    return 0;
  }
  EntityId cursor = start;
  std::uint32_t depth = 0;
  std::set<EntityId> visited;
  while (depth < max_depth) {
    const auto found = parents.find(cursor);
    if (found == parents.end()) {
      return std::nullopt;
    }
    if (!visited.insert(found->second).second) {
      return std::nullopt;  // cycle
    }
    ++depth;
    if (found->second == root) {
      return depth;
    }
    cursor = found->second;
  }
  return std::nullopt;
}

}  // namespace

std::string QueryResult::canonical_summary() const {
  std::ostringstream out;
  out << "query snapshot " << snapshot.to_text() << "\n";
  out << "  generation " << generation.value() << " epoch " << epoch.value() << "\n";
  out << "  scanned " << scanned_subjects << " matched_subjects " << matched_subjects
      << " matched_aspects " << matched_aspects << " truncated "
      << (truncated ? "yes" : "no") << "\n";
  out << "  digest " << digest.to_hex() << "\n";
  for (const SubjectState& subject : subjects) {
    out << "  subject " << subject.identity.typed_text() << " worst="
        << to_string(subject.worst_truth) << "\n";
    for (const AspectState& aspect : subject.aspects) {
      out << "    aspect " << aspect.aspect.view() << " " << to_string(aspect.truth);
      if (aspect.agreed_value.has_value()) {
        out << " value=" << aspect.agreed_value->display_text();
      }
      out << "\n";
    }
  }
  return out.str();
}

Result<QueryResult> execute_query(const Snapshot& snapshot, const QueryFilter& filter,
                                  const Limits& limits) {
  QueryResult result;
  result.snapshot = snapshot.id();
  result.generation = snapshot.generation();
  result.epoch = snapshot.epoch();

  std::uint32_t effective_limit = limits.max_query_results;
  if (filter.limit != 0 && filter.limit < effective_limit) {
    effective_limit = filter.limit;
  }
  std::uint32_t effective_depth = limits.max_hierarchy_depth;
  if (filter.max_depth != 0 && filter.max_depth < effective_depth) {
    effective_depth = filter.max_depth;
  }

  std::map<EntityId, EntityId> parents;
  for (const SubjectState& subject : snapshot.subjects()) {
    if (subject.parent.has_value()) {
      parents.emplace(subject.identity.id, *subject.parent);
    }
  }

  CanonicalEncoder encoder(1024);
  encoder.tag("query-result");
  encoder.u64(snapshot.generation().value());
  encoder.u64(snapshot.epoch().value());

  for (const SubjectState& subject : snapshot.subjects()) {
    ++result.scanned_subjects;
    if (filter.kind.has_value() && subject.identity.kind != *filter.kind) {
      continue;
    }
    if (filter.subtree_root.has_value()) {
      const std::optional<std::uint32_t> depth =
          depth_from_root(parents, subject.identity.id, *filter.subtree_root, effective_depth + 1);
      if (!depth.has_value()) {
        continue;
      }
    }
    if (result.subjects.size() >= effective_limit) {
      result.truncated = true;
      break;
    }

    SubjectState row;
    row.identity = subject.identity;
    row.parent = subject.parent;
    row.topology_truth = subject.topology_truth;
    row.worst_truth = subject.worst_truth;
    bool row_empty = true;
    for (const AspectState& aspect : subject.aspects) {
      if (!domain_selected(filter.domains, aspect.domain)) {
        continue;
      }
      if (!truth_selected(filter.truths, aspect.truth)) {
        continue;
      }
      if (filter.aspect.has_value() && !(aspect.aspect == *filter.aspect)) {
        continue;
      }
      AspectState selected = aspect;
      if (!filter.include_claims) {
        selected.claims.clear();
      }
      if (!filter.include_conflicts) {
        selected.conflicts.clear();
      }
      row.aspects.push_back(std::move(selected));
      ++result.matched_aspects;
      row_empty = false;
    }
    if (row_empty) {
      continue;
    }
    ++result.matched_subjects;
    encode_subject(encoder, row, filter.include_claims);
    result.subjects.push_back(std::move(row));
  }

  result.digest = encoder.digest();
  return Ok(std::move(result));
}

Result<HierarchyResult> execute_hierarchy(const Snapshot& snapshot, const HierarchyQuery& query,
                                          const Limits& limits) {
  HierarchyResult result;
  result.snapshot = snapshot.id();

  std::uint32_t effective_limit = limits.max_query_results;
  if (query.limit != 0 && query.limit < effective_limit) {
    effective_limit = query.limit;
  }
  std::uint32_t effective_depth = limits.max_hierarchy_depth;
  if (query.max_depth != 0 && query.max_depth < effective_depth) {
    effective_depth = query.max_depth;
  }

  std::map<EntityId, const SubjectState*> by_id;
  for (const SubjectState& subject : snapshot.subjects()) {
    by_id.emplace(subject.identity.id, &subject);
  }

  std::map<EntityId, std::vector<EntityId>> children;
  std::vector<EntityId> roots;
  std::vector<EntityId> unattached;
  for (const SubjectState& subject : snapshot.subjects()) {
    if (subject.parent.has_value() && by_id.find(*subject.parent) != by_id.end()) {
      children[*subject.parent].push_back(subject.identity.id);
      continue;
    }
    // The entity is a root of the observed forest. When it is not the fabric and
    // it has no observed parent - or its parent is not present in this snapshot
    // - the runtime cannot place it in the hierarchy, and says so instead of
    // inventing a position for it.
    roots.push_back(subject.identity.id);
    if (subject.parent.has_value()) {
      ++result.children_of_absent_parents;
    }
    if (query.include_unattached && subject.identity.kind != EntityKind::Fabric) {
      unattached.push_back(subject.identity.id);
    }
  }
  std::sort(unattached.begin(), unattached.end());
  result.unattached = std::move(unattached);
  for (auto& entry : children) {
    std::sort(entry.second.begin(), entry.second.end());
  }

  const auto eligible_root = [&](const SubjectState& subject) {
    if (query.root.has_value()) {
      return subject.identity.id == *query.root;
    }
    if (query.root_kind.has_value()) {
      return subject.identity.kind == *query.root_kind;
    }
    return true;
  };

  std::vector<EntityId> selected_roots;
  for (const EntityId& candidate : roots) {
    const auto found = by_id.find(candidate);
    if (found != by_id.end() && eligible_root(*found->second)) {
      selected_roots.push_back(candidate);
    }
  }
  std::sort(selected_roots.begin(), selected_roots.end());

  // Iterative depth first traversal: no recursion, bounded by the configured
  // hierarchy depth, with a visited set that turns a topology cycle into a
  // reported count instead of an infinite loop.
  std::set<EntityId> visited;
  struct Frame {
    EntityId id;
    std::uint32_t depth;
  };
  std::vector<Frame> stack;
  for (auto it = selected_roots.rbegin(); it != selected_roots.rend(); ++it) {
    stack.push_back(Frame{*it, 0});
  }

  while (!stack.empty()) {
    const Frame frame = stack.back();
    stack.pop_back();
    const auto subject = by_id.find(frame.id);
    if (subject == by_id.end()) {
      continue;
    }
    if (!visited.insert(frame.id).second) {
      ++result.cycles_detected;
      continue;
    }
    if (frame.depth > effective_depth) {
      result.truncated = true;
      ++result.depth_limited;
      continue;
    }
    if (result.nodes.size() >= effective_limit) {
      result.truncated = true;
      break;
    }
    HierarchyNode node;
    node.identity = subject->second->identity;
    node.parent = subject->second->parent;
    node.depth = frame.depth;
    node.topology_truth = subject->second->topology_truth;
    node.parent_absent = subject->second->parent.has_value() &&
                         by_id.find(*subject->second->parent) == by_id.end();
    result.max_depth_reached = std::max(result.max_depth_reached, frame.depth);
    result.nodes.push_back(std::move(node));

    const auto child_list = children.find(frame.id);
    if (child_list != children.end()) {
      for (auto it = child_list->second.rbegin(); it != child_list->second.rend(); ++it) {
        stack.push_back(Frame{*it, frame.depth + 1});
      }
    }
  }

  CanonicalEncoder encoder(1024);
  encoder.tag("hierarchy-result");
  encoder.u64(static_cast<std::uint64_t>(result.nodes.size()));
  for (const HierarchyNode& node : result.nodes) {
    encoder.u64(node.identity.id.value().hi);
    encoder.u64(node.identity.id.value().lo);
    encoder.u32(node.depth);
    encoder.u8(static_cast<std::uint8_t>(node.topology_truth));
    encoder.boolean(node.parent_absent);
  }
  encoder.u64(static_cast<std::uint64_t>(result.unattached.size()));
  for (const EntityId& id : result.unattached) {
    encoder.u64(id.value().hi);
    encoder.u64(id.value().lo);
  }
  result.digest = encoder.digest();
  return Ok(std::move(result));
}

}  // namespace fabric_observatory
