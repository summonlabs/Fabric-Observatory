// Property and invariant tests over seeded randomized workloads.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Every test in this file runs many deterministic seeds. The generator is the
// runtime's own observation model rather than random bytes, so a failure is
// reproducible from the seed printed in the message.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <random>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

constexpr std::uint32_t kSeeds = 60;

class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

  std::uint64_t bounded(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

 private:
  std::uint64_t state_;
};

struct Workload {
  std::vector<std::vector<Observation>> streams;
  std::uint32_t sources{0};
  std::uint32_t generation{0};
  std::uint32_t epoch{0};
};

Workload generate_workload(std::uint64_t seed) {
  Generator generator(seed);
  Workload workload;
  workload.sources = 1 + static_cast<std::uint32_t>(generator.bounded(4));
  const std::uint32_t per_source = 1 + static_cast<std::uint32_t>(generator.bounded(8));
  workload.generation = 1 + static_cast<std::uint32_t>(generator.bounded(3));
  workload.epoch = 1 + static_cast<std::uint32_t>(generator.bounded(3));

  static const char* kAspects[] = {"link.state", "operational.health", "congestion.level",
                                   "capacity.bandwidth_bps", "topology.parent"};
  static const char* kValues[] = {"up", "down", "degraded", "ok", "unknown"};

  for (std::uint32_t source = 0; source < workload.sources; ++source) {
    std::vector<Observation> stream;
    for (std::uint32_t sequence = 1; sequence <= per_source; ++sequence) {
      ObservationBuilder builder(source_id("s" + std::to_string(source)),
                                 "s" + std::to_string(source) + "/boot-1", sequence,
                                 workload.generation, workload.epoch);
      const std::uint32_t claim_count = 1 + static_cast<std::uint32_t>(generator.bounded(3));
      for (std::uint32_t index = 0; index < claim_count; ++index) {
        const std::uint32_t device = static_cast<std::uint32_t>(generator.bounded(5));
        const char* aspect = kAspects[generator.bounded(5)];
        const std::string value = kValues[generator.bounded(5)];
        if (std::string(aspect) == "topology.parent") {
          builder.claim_text(device_subject("d" + std::to_string(device)), aspect,
                             device_subject("d" + std::to_string(generator.bounded(5))).typed_text());
        } else {
          builder.claim_text(device_subject("d" + std::to_string(device)), aspect, value);
        }
      }
      stream.push_back(builder.build());
    }
    workload.streams.push_back(std::move(stream));
  }
  return workload;
}

std::vector<Observation> interleave(const Workload& workload, Generator& generator) {
  std::vector<Observation> ordered;
  std::size_t longest = 0;
  for (const auto& stream : workload.streams) {
    longest = std::max(longest, stream.size());
  }
  std::vector<std::size_t> order(workload.streams.size());
  for (std::size_t round = 0; round < longest; ++round) {
    for (std::size_t index = 0; index < order.size(); ++index) {
      order[index] = index;
    }
    std::shuffle(order.begin(), order.end(), std::mt19937(static_cast<std::uint32_t>(generator.next())));
    for (const std::size_t source : order) {
      if (round < workload.streams[source].size()) {
        ordered.push_back(workload.streams[source][round]);
      }
    }
  }
  return ordered;
}

std::shared_ptr<const Snapshot> build_from(const std::vector<Observation>& observations) {
  ObservatoryFixture fixture;
  if (!fixture.valid()) {
    return nullptr;
  }
  for (const Observation& observation : observations) {
    fixture.ingest(observation, IngestOptions::DeferSnapshot);
  }
  return fixture->current();
}

// Reports the first few lines that differ between two canonical summaries, so a
// determinism failure names the field that moved.
std::string summarize_difference(const std::string& before, const std::string& after) {
  std::vector<std::string> left;
  std::vector<std::string> right;
  std::istringstream before_stream(before);
  std::istringstream after_stream(after);
  std::string line;
  while (std::getline(before_stream, line)) {
    left.push_back(line);
  }
  while (std::getline(after_stream, line)) {
    right.push_back(line);
  }
  std::string report;
  const std::size_t count = std::max(left.size(), right.size());
  std::size_t reported = 0;
  for (std::size_t index = 0; index < count && reported < 6; ++index) {
    const std::string lhs = index < left.size() ? left[index] : std::string("<missing>");
    const std::string rhs = index < right.size() ? right[index] : std::string("<missing>");
    if (lhs != rhs) {
      report += "  - " + lhs + "\n  + " + rhs + "\n";
      ++reported;
    }
  }
  return report.empty() ? std::string("  (summaries are identical)") : report;
}

// Every invariant the runtime claims about a snapshot, checked against one
// snapshot instance.
void check_snapshot_invariants(const Snapshot& snapshot, const std::string& context) {
  FABOBS_CHECK_MSG(snapshot.recompute_digest() == snapshot.digest(),
                   context + ": the digest must be reproducible from the contents");
  FABOBS_CHECK_EQ(snapshot.id().digest(), snapshot.digest());

  bool sorted_subjects = true;
  for (std::size_t index = 1; index < snapshot.subjects().size(); ++index) {
    const SubjectIdentity& previous = snapshot.subjects()[index - 1].identity;
    const SubjectIdentity& current = snapshot.subjects()[index].identity;
    if (previous.kind > current.kind ||
        (previous.kind == current.kind && !(previous.id < current.id))) {
      sorted_subjects = false;
    }
  }
  FABOBS_CHECK_MSG(sorted_subjects, context + ": subjects must be in canonical order");

  std::size_t observed_claims = 0;
  std::size_t observed_aspects = 0;
  std::size_t observed_superseded = 0;
  std::size_t observed_recovered = 0;
  for (const SubjectState& subject : snapshot.subjects()) {
    FABOBS_CHECK_MSG(!subject.aspects.empty(), context + ": a subject exists only because of a claim");
    for (std::size_t index = 1; index < subject.aspects.size(); ++index) {
      FABOBS_CHECK_MSG(subject.aspects[index - 1].aspect < subject.aspects[index].aspect,
                       context + ": aspects must be in canonical order");
    }
    for (const AspectState& aspect : subject.aspects) {
      ++observed_aspects;
      FABOBS_CHECK_MSG(!aspect.claims.empty(), context + ": an aspect exists only because of a claim");
      if (aspect.truth == TruthState::Known) {
        FABOBS_CHECK_MSG(aspect.agreed_value.has_value(),
                         context + ": known implies an agreed value");
        FABOBS_CHECK_MSG(aspect.coverage.satisfied, context + ": known implies coverage");
        FABOBS_CHECK_MSG(aspect.conflicts.empty(), context + ": known implies no conflict");
      }
      if (aspect.truth == TruthState::Conflicting) {
        FABOBS_CHECK_MSG(aspect.conflicts.size() >= 2, context + ": conflicting needs two values");
        FABOBS_CHECK_MSG(!aspect.agreed_value.has_value(),
                         context + ": conflicting must not pick a winner");
      }
      if (aspect.truth != TruthState::Known && aspect.truth != TruthState::Conflicting &&
          aspect.truth != TruthState::Incomplete) {
        FABOBS_CHECK_MSG(!aspect.agreed_value.has_value(),
                         context + ": only known asserts a value");
      }
      for (const ClaimRecord& claim : aspect.claims) {
        ++observed_claims;
        FABOBS_CHECK_MSG(snapshot.find_source(claim.source) != nullptr,
                         context + ": every claim names a source the snapshot reports");
        if (claim.recovered) {
          ++observed_recovered;
          FABOBS_CHECK_MSG(claim.freshness != FreshnessVerdict::Fresh,
                           context + ": recovered evidence is never fresh");
        }
        if (!claim.is_current()) {
          ++observed_superseded;
        }
      }
      for (std::size_t index = 1; index < aspect.claims.size(); ++index) {
        FABOBS_CHECK_MSG(!(aspect.claims[index] < aspect.claims[index - 1]),
                         context + ": claims must be in canonical order");
      }
    }
    if (subject.worst_truth != TruthState::Unknown) {
      for (const AspectState& aspect : subject.aspects) {
        FABOBS_CHECK_MSG(static_cast<std::uint8_t>(subject.worst_truth) <=
                             static_cast<std::uint8_t>(aspect.truth),
                         context + ": the subject truth is the pessimistic one");
      }
    }
  }
  FABOBS_CHECK_EQ(snapshot.stats().claims, static_cast<std::uint64_t>(observed_claims));
  FABOBS_CHECK_EQ(snapshot.stats().aspects, static_cast<std::uint64_t>(observed_aspects));
  FABOBS_CHECK_EQ(snapshot.stats().superseded_claims,
                  static_cast<std::uint64_t>(observed_superseded));
  FABOBS_CHECK_EQ(snapshot.stats().recovered_claims,
                  static_cast<std::uint64_t>(observed_recovered));
}

}  // namespace

FABOBS_TEST(property, snapshot_invariants_hold_for_random_workloads) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    const Workload workload = generate_workload(seed);
    Generator generator(seed * 7919);
    const std::shared_ptr<const Snapshot> snapshot = build_from(interleave(workload, generator));
    FABOBS_REQUIRE(snapshot != nullptr);
    check_snapshot_invariants(*snapshot, "seed " + std::to_string(seed));
  }
}

FABOBS_TEST(property, identity_ignores_interleaving_for_random_workloads) {
  for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
    const Workload workload = generate_workload(seed);
    Generator generator(seed);
    const std::shared_ptr<const Snapshot> reference = build_from(interleave(workload, generator));
    FABOBS_REQUIRE(reference != nullptr);
    for (int attempt = 0; attempt < 3; ++attempt) {
      Generator other(seed * 31 + static_cast<std::uint64_t>(attempt) + 1);
      const std::shared_ptr<const Snapshot> candidate = build_from(interleave(workload, other));
      FABOBS_REQUIRE(candidate != nullptr);
      FABOBS_CHECK_MSG(candidate->id() == reference->id(),
                       "seed " + std::to_string(seed) + " attempt " + std::to_string(attempt) +
                           ": identity must not depend on interleaving");
    }
  }
}

FABOBS_TEST(property, replaying_a_stream_never_changes_the_snapshot) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    const Workload workload = generate_workload(seed);
    Generator generator(seed);
    std::vector<Observation> ordered = interleave(workload, generator);

    ObservatoryFixture fixture;
    FABOBS_REQUIRE(fixture.valid());
    for (const Observation& observation : ordered) {
      fixture.ingest(observation, IngestOptions::DeferSnapshot);
    }
    const std::shared_ptr<const Snapshot> reference = fixture->current();
    FABOBS_REQUIRE(reference != nullptr);

    // Re-submitting the whole stream is idempotent: already accepted evidence is
    // recognised as a duplicate and already refused evidence is refused for the
    // same recorded reason, so the published view does not move.
    for (const Observation& observation : ordered) {
      fixture.ingest(observation, IngestOptions::DeferSnapshot);
    }
    const std::shared_ptr<const Snapshot> replay = fixture->current();
    FABOBS_REQUIRE(replay != nullptr);
    FABOBS_CHECK_MSG(replay->id() == reference->id(),
                     "seed " + std::to_string(seed) + ": a replay must not change the view\n" +
                         summarize_difference(reference->canonical_summary(),
                                              replay->canonical_summary()));
    FABOBS_CHECK_EQ(replay->digest(), reference->digest());
    FABOBS_CHECK_EQ(fixture->stats().evidence_records, reference->evidence_records());
  }
}

FABOBS_TEST(property, every_evidence_record_is_accounted_for) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    const Workload workload = generate_workload(seed);
    Generator generator(seed);
    const std::vector<Observation> ordered = interleave(workload, generator);

    ObservatoryFixture fixture;
    FABOBS_REQUIRE(fixture.valid());
    std::uint64_t accepted = 0;
    std::uint64_t fenced = 0;
    for (const Observation& observation : ordered) {
      const IngestOutcome outcome = fixture.ingest(observation, IngestOptions::DeferSnapshot);
      if (outcome.disposition == IngestDisposition::Accepted) {
        ++accepted;
      } else {
        ++fenced;
      }
    }
    const ObservatoryStats stats = fixture->stats();
    FABOBS_CHECK_EQ(stats.observations_accepted, accepted);
    FABOBS_CHECK_EQ(stats.observations_accepted + stats.observations_rejected +
                        stats.observations_fenced + stats.observations_duplicate,
                    static_cast<std::uint64_t>(ordered.size()));
    FABOBS_CHECK_EQ(stats.evidence_records, accepted);
    FABOBS_CHECK(fenced > 0 || accepted == ordered.size());
    const std::shared_ptr<const Snapshot> snapshot = fixture->current();
    FABOBS_REQUIRE(snapshot != nullptr);
    FABOBS_CHECK_EQ(snapshot->evidence_records(), accepted);
    check_snapshot_invariants(*snapshot, "seed " + std::to_string(seed));
  }
}

FABOBS_TEST(property, freshness_only_ever_degrades_as_time_moves_forward) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    const Workload workload = generate_workload(seed);
    Generator generator(seed);
    const std::vector<Observation> ordered = interleave(workload, generator);

    ObservatoryFixture fixture;
    FABOBS_REQUIRE(fixture.valid());
    for (const Observation& observation : ordered) {
      fixture.ingest(observation, IngestOptions::DeferSnapshot);
    }
    const std::shared_ptr<const Snapshot> initial = fixture->current();
    FABOBS_REQUIRE(initial != nullptr);

    std::map<std::pair<EntityId, std::string>, FreshnessVerdict> previous;
    for (const SubjectState& subject : initial->subjects()) {
      for (const AspectState& aspect : subject.aspects) {
        for (const ClaimRecord& claim : aspect.claims) {
          previous[{subject.identity.id, aspect.aspect.value() + claim.source.to_text()}] =
              claim.freshness;
        }
      }
    }

    TimePoint evaluation = initial->evaluation_time();
    for (int step = 0; step < 4; ++step) {
      evaluation = evaluation + Duration::from_seconds(45);
      std::shared_ptr<const Snapshot> later;
      FABOBS_REQUIRE(fixture->refresh(evaluation, later).ok());
      for (const SubjectState& subject : later->subjects()) {
        for (const AspectState& aspect : subject.aspects) {
          for (const ClaimRecord& claim : aspect.claims) {
            const auto key =
                std::make_pair(subject.identity.id, aspect.aspect.value() + claim.source.to_text());
            const auto found = previous.find(key);
            if (found == previous.end()) {
              continue;
            }
            FABOBS_CHECK_MSG(static_cast<std::uint8_t>(found->second) <=
                                 static_cast<std::uint8_t>(claim.freshness),
                             "seed " + std::to_string(seed) + ": freshness must not improve");
            found->second = claim.freshness;
          }
        }
      }
    }
  }
}

FABOBS_TEST(property, query_results_are_reproducible_for_random_workloads) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    const Workload workload = generate_workload(seed);
    Generator generator(seed);
    const std::shared_ptr<const Snapshot> snapshot = build_from(interleave(workload, generator));
    FABOBS_REQUIRE(snapshot != nullptr);

    QueryFilter filter;
    filter.include_claims = true;
    const Result<QueryResult> first = execute_query(*snapshot, filter, kLimits);
    const Result<QueryResult> second = execute_query(*snapshot, filter, kLimits);
    FABOBS_REQUIRE(first.has_value());
    FABOBS_REQUIRE(second.has_value());
    FABOBS_CHECK_EQ(first->digest, second->digest);
    FABOBS_CHECK_EQ(first->canonical_summary(), second->canonical_summary());

    const Result<SnapshotDiff> diff = diff_snapshots(*snapshot, *snapshot, kLimits);
    FABOBS_REQUIRE(diff.has_value());
    FABOBS_CHECK_EQ(diff->changes.size(), std::size_t{0});
  }
}

FABOBS_TEST(property, history_is_bounded_under_random_publish_sequences) {
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    Generator generator(seed);
    Policy policy;
    policy.limits.max_history_snapshots = 1 + static_cast<std::uint32_t>(generator.bounded(5));
    ObservatoryFixture fixture(policy);
    FABOBS_REQUIRE(fixture.valid());

    const Workload workload = generate_workload(seed);
    Generator order(seed);
    const std::vector<Observation> ordered = interleave(workload, order);
    for (const Observation& observation : ordered) {
      fixture.ingest(observation, IngestOptions::PublishSnapshot);
      const Result<std::vector<HistoryEntry>> entries = fixture->history(1000);
      FABOBS_REQUIRE(entries.has_value());
      FABOBS_CHECK_MSG(entries->size() <= policy.limits.max_history_snapshots,
                       "seed " + std::to_string(seed) + ": history must stay bounded");
      for (std::size_t index = 1; index < entries->size(); ++index) {
        FABOBS_CHECK(entries->at(index - 1).index > entries->at(index).index);
      }
    }
  }
}
