// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_INGEST_HPP
#define FABRIC_OBSERVATORY_INGEST_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/lock_audit.hpp"
#include "fabric_observatory/observation.hpp"
#include "fabric_observatory/observatory.hpp"

namespace fabric_observatory {

enum class ShutdownMode : std::uint8_t {
  Drain = 0,    // process every queued observation before returning
  Discard = 1,  // stop immediately and account for what was discarded
};

std::string_view to_string(ShutdownMode mode) noexcept;

struct PipelineConfig {
  std::uint32_t workers{4};
  std::uint32_t max_queue{4096};
  // When false, accepted observations make the published snapshot stale but do
  // not force a rebuild; the caller publishes explicitly.
  bool publish_per_observation{true};

  friend bool operator==(const PipelineConfig&, const PipelineConfig&) = default;
};

struct PipelineStats {
  std::uint64_t submitted{0};
  std::uint64_t accepted{0};
  std::uint64_t duplicates{0};
  std::uint64_t fenced{0};
  std::uint64_t rejected{0};
  std::uint64_t queue_full{0};
  std::uint64_t discarded{0};
  std::uint32_t workers{0};
  std::uint32_t peak_queue_depth{0};
  bool running{false};

  friend bool operator==(const PipelineStats&, const PipelineStats&) = default;
};

struct ShutdownReport {
  ShutdownMode mode{ShutdownMode::Drain};
  std::uint64_t processed{0};
  std::uint64_t discarded{0};
  std::uint32_t workers_joined{0};
  bool cancelled{false};
  PipelineStats stats{};

  friend bool operator==(const ShutdownReport&, const ShutdownReport&) = default;
};

// Concurrent ingest front end.
//
// Ordering: observations are sharded by source identity, and each worker
// consumes exactly one shard. Per-source ordering is therefore preserved while
// different sources proceed in parallel, so the accepted evidence set - and the
// resulting snapshot - does not depend on thread scheduling.
//
// Bounds: the queue is bounded per shard; submit() reports QueueFull instead of
// blocking or growing. Shutdown is a real join: every worker thread is joined
// before shutdown() returns, and nothing is dropped without being counted.
class IngestPipeline {
 public:
  IngestPipeline(Observatory& observatory, PipelineConfig config);
  ~IngestPipeline();

  IngestPipeline(const IngestPipeline&) = delete;
  IngestPipeline& operator=(const IngestPipeline&) = delete;

  [[nodiscard]] Status submit(Observation observation);
  void cancel();
  [[nodiscard]] ShutdownReport shutdown(ShutdownMode mode);
  [[nodiscard]] PipelineStats stats() const;
  [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }
  [[nodiscard]] std::uint32_t worker_count() const noexcept { return worker_count_; }

 private:
  struct Shard {
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::deque<Observation> queue;
    std::uint32_t capacity{0};
    std::uint32_t peak_depth{0};
    bool stopping{false};
    bool discard{false};
  };

  void worker(std::uint32_t index);
  [[nodiscard]] std::uint32_t shard_for(SourceId source) const noexcept;

  Observatory* observatory_{nullptr};
  PipelineConfig config_{};
  std::uint32_t worker_count_{0};
  std::vector<std::unique_ptr<Shard>> shards_{};
  std::vector<std::thread> threads_{};
  std::atomic<bool> running_{false};
  mutable std::mutex counters_mutex_;
  PipelineStats counters_{};
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_INGEST_HPP
