// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/lock_audit.hpp"

#include "fabric_observatory/export.hpp"

#include <array>
#include <atomic>

namespace fabric_observatory {

namespace {

constexpr std::size_t kMaxAuditedDepth = 8;

struct ThreadState {
  std::array<LockLevel, kMaxAuditedDepth> held{};
  std::uint32_t depth{0};
};

ThreadState& thread_state() noexcept {
  thread_local ThreadState state;
  return state;
}

std::atomic<std::uint64_t> g_acquisitions{0};
std::atomic<std::uint64_t> g_reentrancy{0};
std::atomic<std::uint64_t> g_order{0};
std::atomic<std::uint32_t> g_max_depth{0};

}  // namespace

std::string_view to_string(LockLevel level) noexcept {
  switch (level) {
    case LockLevel::Queue:
      return "queue";
    case LockLevel::State:
      return "state";
    case LockLevel::Persistence:
      return "persistence";
  }
  return "unknown";
}

LockOrderGuard::LockOrderGuard(LockLevel level) noexcept : level_(level), engaged_(true) {
  ThreadState& state = thread_state();
  g_acquisitions.fetch_add(1, std::memory_order_relaxed);

  for (std::uint32_t index = 0; index < state.depth; ++index) {
    // Re-entering a level that this thread already holds is a self-deadlock in
    // waiting: every lock in this runtime is non-recursive.
    if (state.held[index] == level_) {
      g_reentrancy.fetch_add(1, std::memory_order_relaxed);
      FABRIC_OBSERVATORY_CONTRACT_MSG(false, "lock reentrancy detected by the lock auditor");
    }
    // Acquiring a lower or equal level while holding a higher one inverts the
    // documented hierarchy.
    if (static_cast<std::uint8_t>(state.held[index]) >= static_cast<std::uint8_t>(level_)) {
      g_order.fetch_add(1, std::memory_order_relaxed);
      FABRIC_OBSERVATORY_CONTRACT_MSG(false, "lock order violation detected by the lock auditor");
    }
  }

  if (state.depth >= kMaxAuditedDepth) {
    FABRIC_OBSERVATORY_CONTRACT_MSG(false, "lock nesting deeper than the auditor supports");
  }
  state.held[state.depth] = level_;
  ++state.depth;

  std::uint32_t observed = g_max_depth.load(std::memory_order_relaxed);
  while (observed < state.depth &&
         !g_max_depth.compare_exchange_weak(observed, state.depth, std::memory_order_relaxed)) {
  }
}

LockOrderGuard::~LockOrderGuard() {
  ThreadState& state = thread_state();
  FABRIC_OBSERVATORY_CONTRACT(state.depth > 0);
  --state.depth;
}

std::string LockAuditReport::to_text() const {
  std::string out;
  out += "lock_audit acquisitions=" + std::to_string(acquisitions);
  out += " max_depth=" + std::to_string(max_depth);
  out += " reentrancy_violations=" + std::to_string(reentrancy_violations);
  out += " order_violations=" + std::to_string(order_violations);
  out += clean ? " clean=yes" : " clean=no";
  return out;
}

LockAuditReport lock_audit_report() noexcept {
  LockAuditReport report;
  report.acquisitions = g_acquisitions.load(std::memory_order_relaxed);
  report.reentrancy_violations = g_reentrancy.load(std::memory_order_relaxed);
  report.order_violations = g_order.load(std::memory_order_relaxed);
  report.max_depth = g_max_depth.load(std::memory_order_relaxed);
  report.clean = report.reentrancy_violations == 0 && report.order_violations == 0;
  return report;
}

void lock_audit_reset() noexcept {
  g_acquisitions.store(0, std::memory_order_relaxed);
  g_reentrancy.store(0, std::memory_order_relaxed);
  g_order.store(0, std::memory_order_relaxed);
  g_max_depth.store(0, std::memory_order_relaxed);
}

}  // namespace fabric_observatory
