// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/ingest.hpp"

#include <algorithm>

namespace fabric_observatory {

namespace {

// Deterministic shard selection. The value depends only on the source identity:
// never on addresses, thread identifiers or the time of day.
std::uint64_t source_hash(SourceId source) noexcept {
  return (source.value().hi * 0x9E3779B97F4A7C15ull) ^ (source.value().lo + 0x165667B19E3779F9ull);
}

}  // namespace

std::string_view to_string(ShutdownMode mode) noexcept {
  switch (mode) {
    case ShutdownMode::Drain:
      return "drain";
    case ShutdownMode::Discard:
      return "discard";
  }
  return "drain";
}

IngestPipeline::IngestPipeline(Observatory& observatory, PipelineConfig config)
    : observatory_(&observatory), config_(config) {
  FABRIC_OBSERVATORY_CONTRACT(config_.workers > 0);
  FABRIC_OBSERVATORY_CONTRACT(config_.max_queue > 0);

  const std::uint32_t hard_worker_bound = observatory.policy().limits.max_workers;
  worker_count_ = std::min(config_.workers, std::max(1u, hard_worker_bound));

  const std::uint32_t queue_bound = observatory.policy().limits.max_ingest_queue;
  const std::uint32_t effective_queue = std::min(config_.max_queue, queue_bound);
  const std::uint32_t per_shard = std::max(1u, effective_queue / worker_count_);

  shards_.reserve(worker_count_);
  for (std::uint32_t index = 0; index < worker_count_; ++index) {
    auto shard = std::make_unique<Shard>();
    shard->capacity = per_shard;
    shards_.push_back(std::move(shard));
  }
  counters_.workers = worker_count_;

  running_.store(true, std::memory_order_release);
  threads_.reserve(worker_count_);
  for (std::uint32_t index = 0; index < worker_count_; ++index) {
    threads_.emplace_back([this, index] { worker(index); });
  }
}

IngestPipeline::~IngestPipeline() {
  const ShutdownReport report = shutdown(ShutdownMode::Drain);
  (void)report;
}

std::uint32_t IngestPipeline::shard_for(SourceId source) const noexcept {
  return static_cast<std::uint32_t>(source_hash(source) % worker_count_);
}

Status IngestPipeline::submit(Observation observation) {
  if (!running_.load(std::memory_order_acquire)) {
    return Status::error(StatusCode::ShuttingDown, "the ingest pipeline is not running");
  }
  Shard& shard = *shards_[shard_for(observation.source)];
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    LockOrderGuard guard(LockLevel::Queue);
    if (shard.stopping) {
      return Status::error(StatusCode::ShuttingDown, "the ingest pipeline is stopping");
    }
    if (shard.queue.size() >= shard.capacity) {
      std::lock_guard<std::mutex> counters(counters_mutex_);
      ++counters_.queue_full;
      return Status::error(StatusCode::QueueFull,
                           "the ingest queue for this source shard is full");
    }
    shard.queue.push_back(std::move(observation));
    shard.peak_depth = std::max(shard.peak_depth, static_cast<std::uint32_t>(shard.queue.size()));
  }
  shard.ready.notify_one();
  std::lock_guard<std::mutex> counters(counters_mutex_);
  ++counters_.submitted;
  return Status{};
}

void IngestPipeline::worker(std::uint32_t index) {
  Shard& shard = *shards_[index];
  while (true) {
    Observation item;
    {
      std::unique_lock<std::mutex> lock(shard.mutex);
      LockOrderGuard guard(LockLevel::Queue);
      shard.ready.wait(lock, [&shard] { return !shard.queue.empty() || shard.stopping; });
      if (shard.discard && shard.stopping) {
        const std::size_t dropped = shard.queue.size();
        shard.queue.clear();
        if (dropped > 0) {
          std::lock_guard<std::mutex> counters(counters_mutex_);
          counters_.discarded += dropped;
        }
        return;
      }
      if (shard.queue.empty()) {
        // Stopping with an empty queue: every submitted observation has been
        // processed, so the worker can leave.
        return;
      }
      item = std::move(shard.queue.front());
      shard.queue.pop_front();
    }

    IngestOutcome outcome;
    const IngestOptions options = config_.publish_per_observation ? IngestOptions::PublishSnapshot
                                                                  : IngestOptions::DeferSnapshot;
    observatory_->ingest(item, outcome, options);
    std::lock_guard<std::mutex> counters(counters_mutex_);
    switch (outcome.disposition) {
      case IngestDisposition::Accepted:
        ++counters_.accepted;
        break;
      case IngestDisposition::Duplicate:
        ++counters_.duplicates;
        break;
      case IngestDisposition::Fenced:
        ++counters_.fenced;
        break;
      case IngestDisposition::Rejected:
        ++counters_.rejected;
        break;
    }
  }
}

void IngestPipeline::cancel() {
  for (auto& shard : shards_) {
    {
      std::lock_guard<std::mutex> lock(shard->mutex);
      LockOrderGuard guard(LockLevel::Queue);
      shard->stopping = true;
      shard->discard = true;
    }
    shard->ready.notify_all();
  }
}

ShutdownReport IngestPipeline::shutdown(ShutdownMode mode) {
  ShutdownReport report;
  report.mode = mode;

  if (threads_.empty()) {
    report.stats = stats();
    return report;
  }

  for (auto& shard : shards_) {
    {
      std::lock_guard<std::mutex> lock(shard->mutex);
      LockOrderGuard guard(LockLevel::Queue);
      shard->stopping = true;
      shard->discard = mode == ShutdownMode::Discard;
    }
    shard->ready.notify_all();
  }

  // Real join: every worker thread has finished before shutdown returns.
  for (std::thread& thread : threads_) {
    if (thread.joinable()) {
      thread.join();
      ++report.workers_joined;
    }
  }
  threads_.clear();
  running_.store(false, std::memory_order_release);
  counters_.running = false;

  std::uint32_t peak = 0;
  for (const auto& shard : shards_) {
    std::lock_guard<std::mutex> lock(shard->mutex);
    peak = std::max(peak, shard->peak_depth);
  }
  {
    std::lock_guard<std::mutex> counters(counters_mutex_);
    counters_.peak_queue_depth = peak;
  }
  report.cancelled = mode == ShutdownMode::Discard;
  report.discarded = stats().discarded;
  report.processed = stats().accepted + stats().duplicates + stats().fenced + stats().rejected;
  report.stats = stats();
  return report;
}

PipelineStats IngestPipeline::stats() const {
  // The counters lock and the shard locks are never held at the same time: the
  // runtime keeps one lock level live at a time, which is what makes a lock
  // cycle impossible to construct rather than merely unlikely.
  PipelineStats snapshot;
  {
    std::lock_guard<std::mutex> lock(counters_mutex_);
    snapshot = counters_;
  }
  snapshot.running = running_.load(std::memory_order_acquire);
  std::uint32_t peak = 0;
  for (const auto& shard : shards_) {
    std::lock_guard<std::mutex> shard_lock(shard->mutex);
    LockOrderGuard guard(LockLevel::Queue);
    peak = std::max(peak, shard->peak_depth);
  }
  snapshot.peak_queue_depth = std::max(snapshot.peak_queue_depth, peak);
  return snapshot;
}

}  // namespace fabric_observatory
