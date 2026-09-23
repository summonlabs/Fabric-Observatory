// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_PROVENANCE_HPP
#define FABRIC_OBSERVATORY_PROVENANCE_HPP

#include <compare>
#include <cstdint>
#include <string>
#include <vector>

#include "fabric_observatory/aspect.hpp"
#include "fabric_observatory/ids.hpp"
#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/status.hpp"
#include "fabric_observatory/time.hpp"
#include "fabric_observatory/truth.hpp"

namespace fabric_observatory {

// What a source declares about itself. The declaration is the contract: a claim
// that exceeds the declared authority of its source is fenced, and a claim for
// an aspect outside the declared scope is recorded as out of scope rather than
// silently accepted.
struct SourceDescriptor {
  SourceId id{};
  std::string name{};
  std::string authority_name{};
  std::vector<AspectId> aspects{};
  SourceAuthority max_authority{SourceAuthority::None};
  std::string schema{};

  friend bool operator==(const SourceDescriptor&, const SourceDescriptor&) = default;
  friend auto operator<=>(const SourceDescriptor&, const SourceDescriptor&) = default;

  [[nodiscard]] bool declares(std::string_view aspect) const noexcept;
};

// A single run of a source process. An incarnation is the fence that makes a
// restarted source distinguishable from a replayed one.
struct SourceIncarnation {
  IncarnationId id{};
  TimePoint boot_time{};
  std::uint64_t boot_counter{0};

  friend bool operator==(const SourceIncarnation&, const SourceIncarnation&) = default;
  friend auto operator<=>(const SourceIncarnation&, const SourceIncarnation&) = default;
};

// Per-source ingest bookkeeping. All counters are monotone; nothing here is
// derived from wall clock progress except the recorded times themselves.
struct SourceState {
  SourceDescriptor descriptor{};
  SourceIncarnation incarnation{};
  SourceSequence last_sequence{};
  ClaimRevision last_revision{};
  TimePoint first_receive{};
  TimePoint last_receive{};
  TimePoint last_observation{};
  GenerationId last_generation{};
  EpochId last_epoch{};
  std::uint64_t accepted{0};
  std::uint64_t fenced{0};
  std::uint64_t duplicates{0};
  RestartEpoch recovery_epoch{};
  bool recovered{false};

  friend bool operator==(const SourceState&, const SourceState&) = default;
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_PROVENANCE_HPP
