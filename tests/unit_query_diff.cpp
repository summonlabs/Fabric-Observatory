// Unit tests: hierarchical queries, topology presentation and snapshot diffing.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

const SubjectIdentity kSite = SubjectIdentity::of(SiteId::derive("site/alpha"));
const SubjectIdentity kPod = SubjectIdentity::of(PodId::derive("pod/alpha-1"));
const SubjectIdentity kRack = SubjectIdentity::of(RackId::derive("rack/alpha-1-1"));
const SubjectIdentity kDevice = SubjectIdentity::of(DeviceId::derive("device/leaf-1"));
const SubjectIdentity kOrphan = SubjectIdentity::of(DeviceId::derive("device/orphan"));

void ingest_topology(ObservatoryFixture& fixture, std::uint64_t& sequence) {
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", ++sequence)
                     .claim_text(kPod, "topology.parent", kSite.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", ++sequence)
                     .claim_text(kRack, "topology.parent", kPod.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", ++sequence)
                     .claim_text(kDevice, "topology.parent", kRack.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);
}

}  // namespace

FABOBS_TEST(query, filter_by_kind_domain_truth_and_aspect) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  std::uint64_t sequence = 0;
  ingest_topology(fixture, sequence);
  fixture.ingest(ObservationBuilder(source_id("state"), "state/boot-1", ++sequence)
                     .claim_text(kDevice, "link.state", "up")
                     .build(),
                 IngestOptions::DeferSnapshot);
  fixture.ingest(ObservationBuilder(source_id("state"), "state/boot-1", ++sequence)
                     .claim_text(kDevice, "reachability.state", "reachable")
                     .build(),
                 IngestOptions::DeferSnapshot);

  QueryFilter by_kind;
  by_kind.kind = EntityKind::Pod;
  Result<QueryResult> pods = fixture->query(by_kind);
  FABOBS_REQUIRE(pods.has_value());
  FABOBS_REQUIRE_EQ(pods->subjects.size(), std::size_t{1});
  FABOBS_CHECK_EQ(pods->subjects.front().identity.id, kPod.id);

  QueryFilter by_domain;
  by_domain.domains.push_back(Domain::Link);
  Result<QueryResult> links = fixture->query(by_domain);
  FABOBS_REQUIRE(links.has_value());
  FABOBS_CHECK_EQ(links->matched_aspects, std::uint64_t{1});

  QueryFilter by_truth;
  by_truth.truths.push_back(TruthState::Incomplete);
  Result<QueryResult> incomplete = fixture->query(by_truth);
  FABOBS_REQUIRE(incomplete.has_value());
  FABOBS_CHECK_EQ(incomplete->matched_aspects, std::uint64_t{1});
  FABOBS_CHECK_EQ(incomplete->subjects.front().identity.id, kDevice.id);

  QueryFilter by_aspect;
  by_aspect.aspect = *well_known_aspect("link.state");
  Result<QueryResult> link_state = fixture->query(by_aspect);
  FABOBS_REQUIRE(link_state.has_value());
  FABOBS_CHECK_EQ(link_state->subjects.size(), std::size_t{1});
  FABOBS_CHECK_EQ(link_state->subjects.front().aspects.size(), std::size_t{1});

  QueryFilter without_claims;
  without_claims.aspect = *well_known_aspect("link.state");
  without_claims.include_claims = false;
  Result<QueryResult> lean = fixture->query(without_claims);
  FABOBS_REQUIRE(lean.has_value());
  FABOBS_CHECK_EQ(lean->subjects.front().aspects.front().claims.size(), std::size_t{0});
}

FABOBS_TEST(query, results_are_deterministic_and_generation_bound) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  for (std::uint64_t index = 0; index < 8; ++index) {
    ObservationBuilder builder(source_id("s"), "s/boot-1", index + 1, 2, 1, 1000);
    builder.claim_text(device_subject("d" + std::to_string(index)), "link.state", "up");
    fixture.ingest(builder.build(), IngestOptions::DeferSnapshot);
  }
  std::shared_ptr<const Snapshot> snapshot;
  fixture->publish(snapshot);

  QueryFilter filter;
  Result<QueryResult> first = fixture->query(filter);
  Result<QueryResult> second = fixture->query(filter);
  FABOBS_REQUIRE(first.has_value());
  FABOBS_REQUIRE(second.has_value());
  FABOBS_CHECK_EQ(first->digest, second->digest);
  FABOBS_CHECK_EQ(first->snapshot, snapshot->id());
  FABOBS_CHECK_EQ(first->generation.value(), std::uint64_t{2});
  FABOBS_CHECK_EQ(first->canonical_summary(), second->canonical_summary());
  FABOBS_CHECK(first->canonical_summary().find(snapshot->id().to_text()) != std::string::npos);
}

FABOBS_TEST(query, subtree_filter_follows_observed_topology) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  std::uint64_t sequence = 0;
  ingest_topology(fixture, sequence);
  std::shared_ptr<const Snapshot> snapshot;
  fixture->publish(snapshot);

  QueryFilter whole;
  whole.subtree_root = kPod.id;
  Result<QueryResult> subtree = fixture->query(whole);
  FABOBS_REQUIRE(subtree.has_value());
  // The pod itself, the rack below it and the device below that.
  FABOBS_CHECK_EQ(subtree->subjects.size(), std::size_t{3});

  QueryFilter device_only;
  device_only.subtree_root = kDevice.id;
  Result<QueryResult> single = fixture->query(device_only);
  FABOBS_REQUIRE(single.has_value());
  FABOBS_CHECK_EQ(single->subjects.size(), std::size_t{1});

  QueryFilter unreachable;
  unreachable.subtree_root = kOrphan.id;
  Result<QueryResult> none = fixture->query(unreachable);
  FABOBS_REQUIRE(none.has_value());
  FABOBS_CHECK_EQ(none->subjects.size(), std::size_t{0});
}

FABOBS_TEST(query, hierarchy_presents_observed_containment) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  std::uint64_t sequence = 0;
  // The site only ever appears as a parent value, never as the subject of an
  // assertion. The runtime presents exactly what it was told: it does not
  // materialise a subject nobody said anything about, and the pod reports that
  // its parent is absent from this snapshot instead.
  ingest_topology(fixture, sequence);
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", ++sequence)
                     .claim_text(kOrphan, "link.state", "up")
                     .build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> snapshot;
  fixture->publish(snapshot);
  FABOBS_CHECK(snapshot->find_subject(kSite.ref()) == nullptr);

  Result<HierarchyResult> tree = fixture->hierarchy(HierarchyQuery{});
  FABOBS_REQUIRE(tree.has_value());
  FABOBS_CHECK_EQ(tree->cycles_detected, 0u);
  FABOBS_CHECK(!tree->truncated);

  bool saw_pod_root = false;
  bool saw_device = false;
  for (const HierarchyNode& node : tree->nodes) {
    if (node.identity.id == kPod.id) {
      saw_pod_root = true;
      FABOBS_CHECK_EQ(node.depth, 0u);
      FABOBS_REQUIRE(node.parent.has_value());
      FABOBS_CHECK_EQ(*node.parent, kSite.id);
      FABOBS_CHECK(node.parent_absent);
    }
    if (node.identity.id == kDevice.id) {
      saw_device = true;
      FABOBS_CHECK_EQ(node.depth, 2u);
      FABOBS_REQUIRE(node.parent.has_value());
      FABOBS_CHECK_EQ(*node.parent, kRack.id);
      FABOBS_CHECK(!node.parent_absent);
    }
  }
  FABOBS_CHECK(saw_pod_root);
  FABOBS_CHECK(saw_device);
  FABOBS_CHECK_EQ(tree->max_depth_reached, 2u);
  FABOBS_CHECK_EQ(tree->children_of_absent_parents, std::uint64_t{1});
  // Entities the runtime cannot place in the hierarchy are named, not hidden:
  // the pod (parent absent) and the orphan (no parent at all).
  FABOBS_REQUIRE_EQ(tree->unattached.size(), std::size_t{2});
  FABOBS_CHECK(std::is_sorted(tree->unattached.begin(), tree->unattached.end()));
  FABOBS_CHECK(std::find(tree->unattached.begin(), tree->unattached.end(), kOrphan.id) !=
               tree->unattached.end());
  FABOBS_CHECK(std::find(tree->unattached.begin(), tree->unattached.end(), kPod.id) !=
               tree->unattached.end());
  const Result<HierarchyResult> again = fixture->hierarchy(HierarchyQuery{});
  FABOBS_REQUIRE(again.has_value());
  FABOBS_CHECK_EQ(again->digest, tree->digest);
}

FABOBS_TEST(query, hierarchy_presents_every_asserted_level) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  std::uint64_t sequence = 0;
  // Asserting something about the site is what makes it part of the view.
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", ++sequence)
                     .claim_text(kSite, "identity.labels", "site-alpha")
                     .build(),
                 IngestOptions::DeferSnapshot);
  ingest_topology(fixture, sequence);
  std::shared_ptr<const Snapshot> snapshot;
  fixture->publish(snapshot);
  FABOBS_CHECK(snapshot->find_subject(kSite.ref()) != nullptr);

  Result<HierarchyResult> tree = fixture->hierarchy(HierarchyQuery{});
  FABOBS_REQUIRE(tree.has_value());
  FABOBS_CHECK_EQ(tree->max_depth_reached, 3u);
  FABOBS_CHECK_EQ(tree->children_of_absent_parents, std::uint64_t{0});
  for (const HierarchyNode& node : tree->nodes) {
    if (node.identity.id == kSite.id) {
      FABOBS_CHECK_EQ(node.depth, 0u);
      FABOBS_CHECK(!node.parent_absent);
    }
    if (node.identity.id == kDevice.id) {
      FABOBS_CHECK_EQ(node.depth, 3u);
    }
  }
}

FABOBS_TEST(query, hierarchy_detects_cycles_and_absent_parents) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const SubjectIdentity first = SubjectIdentity::of(DeviceId::derive("device/cycle-a"));
  const SubjectIdentity second = SubjectIdentity::of(DeviceId::derive("device/cycle-b"));
  const SubjectIdentity dangling = SubjectIdentity::of(DeviceId::derive("device/dangling"));
  const SubjectIdentity ghost = SubjectIdentity::of(DeviceId::derive("device/ghost"));
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", 1)
                     .claim_text(first, "topology.parent", second.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", 2)
                     .claim_text(second, "topology.parent", first.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", 3)
                     .claim_text(dangling, "topology.parent", ghost.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);

  Result<HierarchyResult> tree = fixture->hierarchy(HierarchyQuery{});
  FABOBS_REQUIRE(tree.has_value());
  // A cycle cannot be entered from a root, so it is never traversed; a parent
  // that is not in the snapshot makes the child a root with an absent parent.
  FABOBS_CHECK_EQ(tree->cycles_detected, 0u);
  bool saw_dangling = false;
  for (const HierarchyNode& node : tree->nodes) {
    if (node.identity.id == dangling.id) {
      saw_dangling = true;
      FABOBS_CHECK(node.parent_absent);
    }
  }
  FABOBS_CHECK(saw_dangling);
  FABOBS_CHECK_EQ(tree->children_of_absent_parents, std::uint64_t{1});

  HierarchyQuery shallow;
  shallow.max_depth = 1;
  Result<HierarchyResult> limited = fixture->hierarchy(shallow);
  FABOBS_REQUIRE(limited.has_value());
  FABOBS_CHECK(limited->depth_limited >= 0u);
}

FABOBS_TEST(query, subtree_filter_terminates_on_a_cyclic_topology) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const SubjectIdentity first = SubjectIdentity::of(DeviceId::derive("device/loop-a"));
  const SubjectIdentity second = SubjectIdentity::of(DeviceId::derive("device/loop-b"));
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", 1)
                     .claim_text(first, "topology.parent", second.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);
  fixture.ingest(ObservationBuilder(source_id("topology"), "topology/boot-1", 2)
                     .claim_text(second, "topology.parent", first.typed_text())
                     .build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> snapshot;
  FABOBS_REQUIRE(fixture->publish(snapshot).ok());

  // The ancestor walk is bounded and cycle aware, so a query over a cycle
  // returns rather than looping.
  QueryFilter filter;
  filter.subtree_root = first.id;
  const Result<QueryResult> result = fixture->query(filter);
  FABOBS_REQUIRE(result.has_value());
  FABOBS_CHECK(result->subjects.size() <= 2);
  FABOBS_CHECK(!result->truncated);
}

FABOBS_TEST(diffing, reports_truth_value_and_source_changes) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("left"), "left/boot-1", 1)
                     .claim_text(kDevice, "link.state", "up")
                     .build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> before;
  fixture->publish(before);

  // A second source disagrees, so the aspect moves from known to conflicting.
  fixture.ingest(ObservationBuilder(source_id("right"), "right/boot-1", 1)
                     .claim_text(kDevice, "link.state", "down")
                     .build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> after;
  fixture->publish(after);
  FABOBS_REQUIRE(before != nullptr);
  FABOBS_REQUIRE(after != nullptr);
  FABOBS_CHECK(before->id() != after->id());

  Result<SnapshotDiff> diff = fixture->diff(before->id(), after->id());
  FABOBS_REQUIRE(diff.has_value());
  FABOBS_CHECK_EQ(diff->before_id, before->id());
  FABOBS_CHECK_EQ(diff->after_id, after->id());
  FABOBS_CHECK(!diff->generation_changed);
  FABOBS_CHECK(!diff->epoch_changed);
  FABOBS_CHECK(!diff->restart_observed);
  FABOBS_REQUIRE_EQ(diff->changes.size(), std::size_t{1});
  const AspectChange& change = diff->changes.front();
  FABOBS_CHECK_EQ(change.subject.id, kDevice.id);
  FABOBS_CHECK_EQ(change.before, TruthState::Known);
  FABOBS_CHECK_EQ(change.after, TruthState::Conflicting);
  FABOBS_REQUIRE(change.value_before.has_value());
  FABOBS_CHECK_EQ(change.value_before->as_text(), std::string("up"));
  FABOBS_CHECK(!change.value_after.has_value());
  FABOBS_REQUIRE_EQ(change.sources_added.size(), std::size_t{1});
  FABOBS_CHECK_EQ(change.sources_added.front(), source_id("right"));

  FABOBS_CHECK(diff->canonical_summary().find(before->id().to_text()) != std::string::npos);
  Result<SnapshotDiff> reverse = fixture->diff(after->id(), before->id());
  FABOBS_REQUIRE(reverse.has_value());
  FABOBS_REQUIRE(reverse->changes.size() == 1);
  FABOBS_CHECK_EQ(reverse->changes.front().before, TruthState::Conflicting);
  FABOBS_CHECK_EQ(reverse->changes.front().after, TruthState::Known);
}

FABOBS_TEST(diffing, reports_added_and_removed_subjects_across_generations) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1, 1, 1)
                     .claim_text(kDevice, "link.state", "up")
                     .build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> before;
  fixture->publish(before);

  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 2, 4, 2)
                     .claim_text(kOrphan, "link.state", "up")
                     .build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> after;
  fixture->publish(after);

  Result<SnapshotDiff> diff = fixture->diff(before->id(), after->id());
  FABOBS_REQUIRE(diff.has_value());
  FABOBS_CHECK(diff->generation_changed);
  FABOBS_CHECK(diff->epoch_changed);
  FABOBS_CHECK_EQ(diff->before_generation.value(), std::uint64_t{1});
  FABOBS_CHECK_EQ(diff->after_generation.value(), std::uint64_t{4});
  FABOBS_REQUIRE_EQ(diff->subjects_added.size(), std::size_t{1});
  FABOBS_CHECK_EQ(diff->subjects_added.front().id, kOrphan.id);
  FABOBS_CHECK_EQ(diff->subjects_removed.size(), std::size_t{0});
}

FABOBS_TEST(diffing, unknown_snapshot_is_reported_not_guessed) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1).claim_text(kDevice, "link.state", "up").build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> only;
  fixture->publish(only);

  const SnapshotId absent = SnapshotId(sha256(std::string_view("not a snapshot")));
  FABOBS_CHECK_EQ(fixture->diff(absent, only->id()).status().code(), StatusCode::NotFound);
  FABOBS_CHECK_EQ(fixture->diff(only->id(), absent).status().code(), StatusCode::NotFound);
  FABOBS_CHECK_EQ(fixture->diff_latest(99).status().code(), StatusCode::NotFound);
}

FABOBS_TEST(diffing, identical_snapshots_diff_to_nothing) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", 1).claim_text(kDevice, "link.state", "up").build(),
                 IngestOptions::DeferSnapshot);
  std::shared_ptr<const Snapshot> first;
  fixture->publish(first);
  std::shared_ptr<const Snapshot> second;
  fixture->publish(second);
  FABOBS_CHECK_EQ(first->id(), second->id());

  Result<SnapshotDiff> diff = fixture->diff(first->id(), second->id());
  FABOBS_REQUIRE(diff.has_value());
  FABOBS_CHECK_EQ(diff->changes.size(), std::size_t{0});
  FABOBS_CHECK_EQ(diff->subjects_added.size(), std::size_t{0});
  FABOBS_CHECK_EQ(diff->subjects_removed.size(), std::size_t{0});
}

FABOBS_TEST(diffing, history_is_available_through_the_facade) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  for (std::uint64_t index = 0; index < 4; ++index) {
    fixture.ingest(ObservationBuilder(source_id("s"), "s/boot-1", index + 1)
                       .claim_text(kDevice, "link.state", index % 2 == 0 ? "up" : "down")
                       .build(),
                   IngestOptions::PublishSnapshot);
  }
  Result<std::vector<HistoryEntry>> entries = fixture->history(10);
  FABOBS_REQUIRE(entries.has_value());
  FABOBS_CHECK(entries->size() >= 2);
  for (std::size_t index = 1; index < entries->size(); ++index) {
    FABOBS_CHECK(entries->at(index - 1).index > entries->at(index).index);
  }
  const HistoryEntry& newest = entries->front();
  Result<std::shared_ptr<const Snapshot>> fetched = fixture->snapshot_at(newest.index);
  FABOBS_REQUIRE(fetched.has_value());
  FABOBS_CHECK_EQ((*fetched)->id(), newest.id);
  Result<std::shared_ptr<const Snapshot>> by_id = fixture->snapshot_by_id(newest.id);
  FABOBS_REQUIRE(by_id.has_value());
  FABOBS_CHECK_EQ((*by_id)->id(), newest.id);
  FABOBS_CHECK_EQ(fixture->snapshot_at(HistoryIndex(9999)).status().code(), StatusCode::NotFound);
}
