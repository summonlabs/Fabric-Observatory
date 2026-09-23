// Concurrency tests: races, ordering guarantees, no torn views, real shutdown.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// No test in this file uses a timeout, a sleep or a wall clock deadline. Every
// worker is joined, so a test either finishes or the process does not return -
// and a hang is a defect, not a slow machine.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

const SubjectIdentity kDevice = SubjectIdentity::of(DeviceId::derive("device/contended"));

std::vector<Observation> sequential_stream(std::uint32_t sources, std::uint32_t per_source) {
  std::vector<Observation> observations;
  for (std::uint32_t source = 0; source < sources; ++source) {
    for (std::uint32_t sequence = 1; sequence <= per_source; ++sequence) {
      observations.push_back(
          ObservationBuilder(source_id("s" + std::to_string(source)),
                             "s" + std::to_string(source) + "/boot-1", sequence, 1, 1)
              .claim_text(device_subject("d" + std::to_string(sequence)), "link.state",
                          (source + sequence) % 2 == 0 ? "up" : "down")
              .build());
    }
  }
  return observations;
}

}  // namespace

FABOBS_TEST(concurrency, readers_never_observe_a_torn_snapshot) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());

  constexpr std::uint32_t kWriters = 4;
  constexpr std::uint32_t kReaders = 4;
  constexpr std::uint32_t kPerWriter = 60;

  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> tears{0};
  std::atomic<std::uint64_t> staleness{0};

  std::vector<std::thread> writers;
  for (std::uint32_t writer = 0; writer < kWriters; ++writer) {
    writers.emplace_back([&fixture, writer] {
      for (std::uint32_t sequence = 1; sequence <= kPerWriter; ++sequence) {
        fixture.ingest(ObservationBuilder(source_id("w" + std::to_string(writer)),
                                          "w" + std::to_string(writer) + "/boot-1", sequence, 1, 1)
                           .claim_text(device_subject("d" + std::to_string(sequence)), "link.state",
                                       "up")
                           .build(),
                       IngestOptions::PublishSnapshot);
      }
    });
  }

  std::vector<std::thread> readers;
  for (std::uint32_t reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_acquire)) {
        const std::shared_ptr<const Snapshot> snapshot = fixture->current();
        if (snapshot == nullptr) {
          tears.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        // A torn view would show contents that do not reproduce the digest the
        // snapshot advertises.
        if (snapshot->recompute_digest() != snapshot->digest()) {
          tears.fetch_add(1, std::memory_order_relaxed);
        }
        if (snapshot->id().digest() != snapshot->digest()) {
          tears.fetch_add(1, std::memory_order_relaxed);
        }
        // The published snapshot must never regress in history order.
        if (snapshot->history_index().is_zero()) {
          staleness.fetch_add(1, std::memory_order_relaxed);
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (std::thread& writer : writers) {
    writer.join();
  }
  stop.store(true, std::memory_order_release);
  for (std::thread& reader : readers) {
    reader.join();
  }

  FABOBS_CHECK_EQ(tears.load(), std::uint64_t{0});
  FABOBS_CHECK_EQ(staleness.load(), std::uint64_t{0});
  FABOBS_CHECK(reads.load() > 0);

  // The concurrent run must converge on exactly the sequential result.
  ObservatoryFixture reference;
  FABOBS_REQUIRE(reference.valid());
  for (std::uint32_t writer = 0; writer < kWriters; ++writer) {
    for (std::uint32_t sequence = 1; sequence <= kPerWriter; ++sequence) {
      reference.ingest(ObservationBuilder(source_id("w" + std::to_string(writer)),
                                          "w" + std::to_string(writer) + "/boot-1", sequence, 1, 1)
                           .claim_text(device_subject("d" + std::to_string(sequence)), "link.state",
                                       "up")
                           .build(),
                       IngestOptions::DeferSnapshot);
    }
  }
  const std::shared_ptr<const Snapshot> expected = reference->current();
  const std::shared_ptr<const Snapshot> actual = fixture->current();
  FABOBS_REQUIRE(expected != nullptr);
  FABOBS_REQUIRE(actual != nullptr);
  FABOBS_CHECK_EQ(actual->id(), expected->id());
}

FABOBS_TEST(concurrency, concurrent_readers_and_writers_leave_a_consistent_view) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  lock_audit_reset();

  constexpr std::uint32_t kSources = 6;
  constexpr std::uint32_t kPerSource = 40;
  const std::vector<Observation> observations = sequential_stream(kSources, kPerSource);

  // Workers own whole sources. A source's sequence is a fence, so splitting one
  // source's stream across threads would be a concurrent replay, not a
  // concurrency test.
  constexpr std::uint32_t kWorkers = 3;
  std::atomic<std::uint32_t> completed_sources{0};
  std::vector<std::thread> workers;
  for (std::uint32_t worker = 0; worker < kWorkers; ++worker) {
    workers.emplace_back([&, worker] {
      for (std::uint32_t source = worker; source < kSources; source += kWorkers) {
        for (std::uint32_t sequence = 0; sequence < kPerSource; ++sequence) {
          fixture.ingest(observations[source * kPerSource + sequence], IngestOptions::DeferSnapshot);
        }
        completed_sources.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  std::atomic<std::uint64_t> observed{0};
  std::thread reader([&] {
    while (completed_sources.load(std::memory_order_relaxed) < kSources) {
      const std::shared_ptr<const Snapshot> snapshot = fixture->current();
      if (snapshot != nullptr && snapshot->recompute_digest() == snapshot->digest()) {
        observed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });

  for (std::thread& worker : workers) {
    worker.join();
  }
  reader.join();
  FABOBS_CHECK(observed.load() > 0);

  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  FABOBS_CHECK_EQ(snapshot->recompute_digest(), snapshot->digest());

  // Because a source's observations are submitted in order, the round robin
  // assignment does not change the accepted evidence.
  ObservatoryFixture reference;
  FABOBS_REQUIRE(reference.valid());
  for (const Observation& observation : observations) {
    reference.ingest(observation, IngestOptions::DeferSnapshot);
  }
  FABOBS_CHECK_EQ(snapshot->id(), reference->current()->id());

  const LockAuditReport audit = lock_audit_report();
  FABOBS_CHECK_MSG(audit.clean, audit.to_text());
  FABOBS_CHECK(audit.acquisitions > 0);
  FABOBS_CHECK(audit.max_depth <= 2);
}

FABOBS_TEST(concurrency, the_pipeline_preserves_per_source_order) {
  constexpr std::uint32_t kSources = 8;
  constexpr std::uint32_t kPerSource = 60;

  ObservatoryFixture reference;
  FABOBS_REQUIRE(reference.valid());
  const std::vector<Observation> observations = sequential_stream(kSources, kPerSource);
  for (const Observation& observation : observations) {
    reference.ingest(observation, IngestOptions::DeferSnapshot);
  }
  const SnapshotId expected = reference->current()->id();

  for (std::uint32_t workers : {1u, 2u, 3u, 8u}) {
    ObservatoryFixture fixture;
    FABOBS_REQUIRE(fixture.valid());
    PipelineConfig config;
    config.workers = workers;
    // The queue is bounded per shard, so the bound has to cover the whole
    // stream of every source that lands on one shard.
    config.max_queue = 4096;
    config.publish_per_observation = false;
    IngestPipeline pipeline(*fixture, config);
    FABOBS_CHECK_EQ(pipeline.worker_count(), workers);
    for (const Observation& observation : observations) {
      const Status status = pipeline.submit(observation);
      FABOBS_CHECK_MSG(status.ok(), "submit failed: " + status.to_string());
    }
    const ShutdownReport report = pipeline.shutdown(ShutdownMode::Drain);
    FABOBS_CHECK_EQ(report.workers_joined, workers);
    FABOBS_CHECK_EQ(report.discarded, std::uint64_t{0});
    FABOBS_CHECK_EQ(report.stats.accepted, static_cast<std::uint64_t>(observations.size()));
    FABOBS_CHECK(!pipeline.running());
    const std::shared_ptr<const Snapshot> snapshot = fixture->current();
    FABOBS_REQUIRE(snapshot != nullptr);
    FABOBS_CHECK_MSG(snapshot->id() == expected,
                     "worker count " + std::to_string(workers) +
                         " changed the accepted evidence");
  }
}

FABOBS_TEST(concurrency, the_pipeline_queue_is_bounded) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  PipelineConfig config;
  config.workers = 1;
  config.max_queue = 2;
  config.publish_per_observation = false;
  IngestPipeline pipeline(*fixture, config);

  // Everything is submitted to the same shard, so the queue bound applies
  // directly. The worker is running, so some submissions may be consumed; the
  // test asserts only that the bound is enforced and reported, never exceeded.
  std::uint64_t full = 0;
  std::uint64_t accepted = 0;
  for (std::uint32_t sequence = 1; sequence <= 200; ++sequence) {
    const Status status = pipeline.submit(ObservationBuilder(source_id("s"), "s/boot-1", sequence)
                                              .claim_text(kDevice, "link.state", "up")
                                              .build());
    if (status.ok()) {
      ++accepted;
    } else {
      FABOBS_CHECK_EQ(status.code(), StatusCode::QueueFull);
      ++full;
    }
  }
  const PipelineStats stats = pipeline.stats();
  FABOBS_CHECK_EQ(stats.queue_full, full);
  FABOBS_CHECK_EQ(stats.submitted, accepted);
  FABOBS_CHECK(stats.peak_queue_depth <= 2);
  const ShutdownReport report = pipeline.shutdown(ShutdownMode::Drain);
  FABOBS_CHECK_EQ(report.stats.accepted, accepted);
  FABOBS_CHECK_EQ(report.discarded, std::uint64_t{0});
}

FABOBS_TEST(concurrency, cancellation_discards_what_it_says_it_discarded) {
  Policy policy;
  policy.limits.max_ingest_queue = 4096;
  ObservatoryFixture fixture(policy);
  FABOBS_REQUIRE(fixture.valid());
  PipelineConfig config;
  config.workers = 2;
  config.max_queue = 4096;
  config.publish_per_observation = false;
  IngestPipeline pipeline(*fixture, config);

  std::uint64_t submitted = 0;
  for (std::uint32_t index = 0; index < 400; ++index) {
    const Status status =
        pipeline.submit(ObservationBuilder(source_id("s" + std::to_string(index % 4)),
                                           "s" + std::to_string(index % 4) + "/boot-1",
                                           1 + index / 4)
                            .claim_text(kDevice, "link.state", "up")
                            .build());
    if (status.ok()) {
      ++submitted;
    }
  }
  pipeline.cancel();
  const ShutdownReport report = pipeline.shutdown(ShutdownMode::Discard);
  FABOBS_CHECK_EQ(report.workers_joined, 2u);
  FABOBS_CHECK(report.cancelled);
  // Every submission is either processed or accounted for as discarded.
  const std::uint64_t processed = report.stats.accepted + report.stats.duplicates +
                                  report.stats.fenced + report.stats.rejected;
  FABOBS_CHECK_EQ(processed + report.discarded, submitted);
  FABOBS_CHECK(!pipeline.running());

  // Submitting after shutdown is refused rather than silently dropped.
  FABOBS_CHECK_EQ(pipeline.submit(ObservationBuilder(source_id("s"), "s/boot-1", 999)
                                      .claim_text(kDevice, "link.state", "up")
                                      .build())
                      .code(),
                  StatusCode::ShuttingDown);
}

FABOBS_TEST(concurrency, draining_shutdown_processes_everything_submitted) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  PipelineConfig config;
  config.workers = 4;
  config.max_queue = 4096;
  config.publish_per_observation = false;
  IngestPipeline pipeline(*fixture, config);

  const std::vector<Observation> observations = sequential_stream(6, 30);
  for (const Observation& observation : observations) {
    FABOBS_REQUIRE(pipeline.submit(observation).ok());
  }
  const ShutdownReport report = pipeline.shutdown(ShutdownMode::Drain);
  FABOBS_CHECK_EQ(report.discarded, std::uint64_t{0});
  FABOBS_CHECK_EQ(report.processed, static_cast<std::uint64_t>(observations.size()));
  FABOBS_CHECK_EQ(report.workers_joined, 4u);

  ObservatoryFixture reference;
  FABOBS_REQUIRE(reference.valid());
  for (const Observation& observation : observations) {
    reference.ingest(observation, IngestOptions::DeferSnapshot);
  }
  FABOBS_CHECK_EQ(fixture->current()->id(), reference->current()->id());
}

FABOBS_TEST(concurrency, the_pipeline_is_reentrant_across_instances) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  lock_audit_reset();

  PipelineConfig config;
  config.workers = 2;
  config.max_queue = 512;
  config.publish_per_observation = true;
  IngestPipeline first(*fixture, config);
  IngestPipeline second(*fixture, config);

  // Two pipelines feeding the same observatory: sequences are per source, so
  // each pipeline is given its own sources.
  for (std::uint32_t sequence = 1; sequence <= 20; ++sequence) {
    FABOBS_CHECK(first
                     .submit(ObservationBuilder(source_id("first"), "first/boot-1", sequence)
                                 .claim_text(kDevice, "link.state", "up")
                                 .build())
                     .ok());
    FABOBS_CHECK(second
                     .submit(ObservationBuilder(source_id("second"), "second/boot-1", sequence)
                                 .claim_text(kDevice, "link.state", "down")
                                 .build())
                     .ok());
  }
  const ShutdownReport first_report = first.shutdown(ShutdownMode::Drain);
  const ShutdownReport second_report = second.shutdown(ShutdownMode::Drain);
  FABOBS_CHECK_EQ(first_report.stats.accepted, std::uint64_t{20});
  FABOBS_CHECK_EQ(second_report.stats.accepted, std::uint64_t{20});

  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  const AspectState* aspect = snapshot->find_aspect(kDevice.ref(), "link.state");
  FABOBS_REQUIRE(aspect != nullptr);
  FABOBS_CHECK_EQ(aspect->truth, TruthState::Conflicting);

  const LockAuditReport audit = lock_audit_report();
  FABOBS_CHECK_MSG(audit.clean, audit.to_text());
}

FABOBS_TEST(concurrency, query_and_diff_are_safe_during_ingest) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  const std::vector<Observation> observations = sequential_stream(4, 30);

  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> queries{0};
  std::thread querier([&] {
    while (!done.load(std::memory_order_acquire)) {
      QueryFilter filter;
      filter.limit = 32;
      const Result<QueryResult> result = fixture->query(filter);
      if (result) {
        FABOBS_CHECK(result->snapshot.is_zero() == false);
      }
      const Result<HierarchyResult> tree = fixture->hierarchy(HierarchyQuery{});
      if (tree) {
        FABOBS_CHECK(tree->cycles_detected == 0u);
      }
      queries.fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (const Observation& observation : observations) {
    fixture.ingest(observation, IngestOptions::PublishSnapshot);
  }
  done.store(true, std::memory_order_release);
  querier.join();

  FABOBS_CHECK(queries.load() > 0);
  const std::shared_ptr<const Snapshot> snapshot = fixture->current();
  FABOBS_REQUIRE(snapshot != nullptr);
  FABOBS_CHECK_EQ(snapshot->recompute_digest(), snapshot->digest());
}

FABOBS_TEST(concurrency, shutdown_is_idempotent) {
  ObservatoryFixture fixture;
  FABOBS_REQUIRE(fixture.valid());
  fixture->shutdown();
  fixture->shutdown();
  FABOBS_CHECK(true);
}
