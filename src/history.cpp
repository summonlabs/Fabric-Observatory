// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/history.hpp"

namespace fabric_observatory {

namespace {

HistoryEntry describe(const std::shared_ptr<const Snapshot>& snapshot) {
  HistoryEntry entry;
  entry.index = snapshot->history_index();
  entry.id = snapshot->id();
  entry.published_at = snapshot->published_at();
  entry.generation = snapshot->generation();
  entry.epoch = snapshot->epoch();
  entry.evaluation_time = snapshot->evaluation_time();
  entry.stats = snapshot->stats();
  return entry;
}

}  // namespace

History::History(std::uint32_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

void History::reset(std::uint32_t capacity) {
  capacity_ = capacity == 0 ? 1 : capacity;
  ring_.clear();
  first_index_ = HistoryIndex{};
  last_index_ = HistoryIndex{};
  total_published_ = 0;
  evicted_ = 0;
}

void History::append(std::shared_ptr<const Snapshot> snapshot, TimePoint published_at) {
  FABRIC_OBSERVATORY_CONTRACT(snapshot != nullptr);
  const HistoryIndex index =
      last_index_.is_zero() ? HistoryIndex(1) : last_index_.next();
  snapshot->set_publication(index, published_at);
  ring_.push_back(std::move(snapshot));
  last_index_ = index;
  ++total_published_;
  while (ring_.size() > capacity_) {
    ring_.pop_front();
    ++evicted_;
  }
  // The ring is a contiguous run of indices ending at last_index_.
  first_index_ = HistoryIndex(last_index_.value() - static_cast<std::uint64_t>(ring_.size()) + 1);
}

std::shared_ptr<const Snapshot> History::latest() const {
  if (ring_.empty()) {
    return nullptr;
  }
  return ring_.back();
}

std::shared_ptr<const Snapshot> History::at(HistoryIndex index) const {
  if (ring_.empty() || index < first_index_ || index > last_index_) {
    return nullptr;
  }
  const std::uint64_t offset = index.value() - first_index_.value();
  if (offset >= ring_.size()) {
    return nullptr;
  }
  return ring_[static_cast<std::size_t>(offset)];
}

std::shared_ptr<const Snapshot> History::find(const SnapshotId& id) const {
  for (const auto& snapshot : ring_) {
    if (snapshot->id() == id) {
      return snapshot;
    }
  }
  return nullptr;
}

std::vector<HistoryEntry> History::entries(std::uint32_t max_entries) const {
  std::vector<HistoryEntry> result;
  for (auto it = ring_.rbegin(); it != ring_.rend(); ++it) {
    if (result.size() >= max_entries) {
      break;
    }
    result.push_back(describe(*it));
  }
  return result;
}

std::vector<HistoryEntry> History::entries_chronological(std::uint32_t max_entries) const {
  std::vector<HistoryEntry> result;
  const std::size_t skip = ring_.size() > max_entries ? ring_.size() - max_entries : 0;
  for (std::size_t index = skip; index < ring_.size(); ++index) {
    result.push_back(describe(ring_[index]));
  }
  return result;
}

}  // namespace fabric_observatory
