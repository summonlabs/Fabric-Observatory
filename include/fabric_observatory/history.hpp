// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_HISTORY_HPP
#define FABRIC_OBSERVATORY_HISTORY_HPP

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "fabric_observatory/snapshot.hpp"

namespace fabric_observatory {

// A descriptor of a retained snapshot. History entries are cheap to copy and
// are what the history tooling and the CLI report.
struct HistoryEntry {
  HistoryIndex index{};
  SnapshotId id{};
  TimePoint published_at{};
  GenerationId generation{};
  EpochId epoch{};
  TimePoint evaluation_time{};
  SnapshotStats stats{};

  friend bool operator==(const HistoryEntry&, const HistoryEntry&) = default;
  friend auto operator<=>(const HistoryEntry&, const HistoryEntry&) = default;
};

// Bounded snapshot history. Capacity is fixed at construction; the oldest entry
// is evicted when the ring is full and the eviction is counted, so growth is
// bounded and the loss is visible rather than silent.
//
// Thread safety: History has no internal lock. It is owned and guarded by the
// Observatory state lock, which is why it can be used without nesting locks.
class History {
 public:
  History() = default;
  explicit History(std::uint32_t capacity);

  // Clears the ring and counters and sets a new capacity. Must not be called
  // while another thread is using the history.
  void reset(std::uint32_t capacity);

  void append(std::shared_ptr<const Snapshot> snapshot, TimePoint published_at);

  [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t size() const noexcept { return ring_.size(); }
  [[nodiscard]] bool empty() const noexcept { return ring_.empty(); }
  [[nodiscard]] std::uint64_t total_published() const noexcept { return total_published_; }
  [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }
  [[nodiscard]] HistoryIndex first_index() const noexcept { return first_index_; }
  [[nodiscard]] HistoryIndex last_index() const noexcept { return last_index_; }

  [[nodiscard]] std::shared_ptr<const Snapshot> latest() const;
  [[nodiscard]] std::shared_ptr<const Snapshot> at(HistoryIndex index) const;
  [[nodiscard]] std::shared_ptr<const Snapshot> find(const SnapshotId& id) const;

  // Newest first, bounded by max_entries.
  [[nodiscard]] std::vector<HistoryEntry> entries(std::uint32_t max_entries) const;
  // Oldest first, bounded by max_entries.
  [[nodiscard]] std::vector<HistoryEntry> entries_chronological(std::uint32_t max_entries) const;

 private:
  std::uint32_t capacity_{256};
  std::deque<std::shared_ptr<const Snapshot>> ring_{};
  HistoryIndex first_index_{};
  HistoryIndex last_index_{};
  std::uint64_t total_published_{0};
  std::uint64_t evicted_{0};
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_HISTORY_HPP
