// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_EXPLANATION_HPP
#define FABRIC_OBSERVATORY_EXPLANATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/snapshot.hpp"

namespace fabric_observatory {

// A deterministic account of why an aspect has the truth state it has. Every
// sentence is derived from the snapshot: no clock is read, no container
// iteration order leaks through, and no heap address or thread identity appears
// in the output. The same snapshot always explains itself the same way.
struct Explanation {
  SnapshotId snapshot{};
  SubjectIdentity subject{};
  std::optional<AspectId> aspect{};
  TruthState truth{TruthState::Unknown};
  std::vector<std::string> reasons{};
  std::vector<std::string> evidence{};
  std::vector<std::string> fences{};
  std::vector<std::string> causality{};

  [[nodiscard]] std::string to_text() const;
};

Result<Explanation> explain_subject(const Snapshot& snapshot, const SubjectIdentity& subject,
                                    std::optional<AspectId> aspect, const Limits& limits);

// Explanation of a subject that is not present at all in the snapshot.
Result<Explanation> explain_absent(const Snapshot& snapshot, const EntityRef& subject,
                                   std::optional<AspectId> aspect);

// The canonical one-line reason for a truth state, derived only from the aspect
// state.
std::string truth_state_reason(const AspectState& aspect);

// One line describing a recorded relationship between two observations,
// including the explicit statement that no causal claim is being made.
std::string causality_line(const CausalRef& reference, const Digest256& subject_observation);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_EXPLANATION_HPP
