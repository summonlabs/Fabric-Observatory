// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_LIMITS_HPP
#define FABRIC_OBSERVATORY_LIMITS_HPP

#include <cstddef>
#include <cstdint>

#include "fabric_observatory/value.hpp"

namespace fabric_observatory {

// Every unbounded quantity in the runtime is named here and defaulted to a
// finite value. There is no "unlimited" setting: growth of memory, history,
// persistence, result sets and queues is bounded by construction.
struct Limits {
  // Registered sources and observed subjects.
  std::uint32_t max_sources{1024};
  std::uint32_t max_subjects{1u << 20};
  std::uint32_t max_aspects_per_subject{64};
  std::uint32_t max_claims_per_subject_aspect{32};
  // Source incarnations tracked per source for stale-boot fencing. Restarts
  // beyond this window are accepted as new rather than refused, which keeps
  // memory bounded at the cost of a documented, bounded replay window.
  std::uint32_t max_incarnations_per_source{8};
  // Retained observation records. When the bound is reached the oldest records
  // are evicted and the eviction is counted and reported.
  std::uint32_t max_evidence_records{262144};

  // A single observation.
  std::uint32_t max_claims_per_observation{512};
  std::uint32_t max_causal_refs{64};
  std::size_t max_metadata_entries{32};
  std::size_t max_metadata_key_bytes{64};
  std::size_t max_metadata_value_bytes{256};
  std::size_t max_observation_bytes{1u << 20};

  // Bounded history and result sets.
  std::uint32_t max_history_snapshots{256};
  std::uint32_t max_query_results{4096};
  std::uint32_t max_hierarchy_depth{16};
  std::uint32_t max_diff_changes{8192};
  std::uint32_t max_explanation_lines{128};
  std::uint32_t max_rejections_retained{256};

  // Ingest pipeline.
  std::uint32_t max_ingest_queue{4096};
  std::uint32_t max_workers{8};
  std::uint32_t max_batch_observations{4096};

  // Persistence growth.
  std::uint64_t max_journal_bytes{64ull * 1024ull * 1024ull};
  std::uint32_t max_journal_files{8};
  std::uint64_t max_journal_record_bytes{4ull * 1024ull * 1024ull};
  std::uint32_t max_recovered_observations{1u << 20};

  // Values carried inside claims, metadata and query filters.
  ValueLimits values{};

  friend bool operator==(const Limits&, const Limits&) = default;
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_LIMITS_HPP
