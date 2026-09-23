// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_LOCK_AUDIT_HPP
#define FABRIC_OBSERVATORY_LOCK_AUDIT_HPP

#include <cstdint>
#include <string>
#include <string_view>

// Lock discipline
// --------------
// The runtime uses a fixed, documented lock hierarchy and never nests locks in
// the opposite direction:
//
//   level 0  QueueLock       the ingest pipeline queue
//   level 1  StateLock       the authoritative evidence and snapshot state
//   level 2  PersistenceLock the journal file
//
// Two rules make deadlock impossible by construction:
//   1. a lock of level N may only be acquired while holding locks of level < N;
//   2. no lock is ever held across a call into code that acquires a lower level.
// The runtime therefore never holds two locks of the same level, and every lock
// it uses is std::mutex (non-recursive) or std::shared_mutex.
//
// The auditor below makes the discipline checkable at runtime rather than only
// by review. It is compiled in by default; it costs a thread-local counter.

namespace fabric_observatory {

enum class LockLevel : std::uint8_t {
  Queue = 0,
  State = 1,
  Persistence = 2,
};

std::string_view to_string(LockLevel level) noexcept;

// RAII marker created immediately after a lock is acquired and destroyed
// immediately before it is released.
class LockOrderGuard {
 public:
  explicit LockOrderGuard(LockLevel level) noexcept;
  ~LockOrderGuard();

  LockOrderGuard(const LockOrderGuard&) = delete;
  LockOrderGuard& operator=(const LockOrderGuard&) = delete;

 private:
  LockLevel level_;
  bool engaged_;
};

// Deterministic audit report: per-thread observation counts are not reported
// (they are scheduling dependent); only violations and observed nesting depth
// are, and both are stable summaries of a run.
struct LockAuditReport {
  std::uint64_t acquisitions{0};
  std::uint64_t reentrancy_violations{0};
  std::uint64_t order_violations{0};
  std::uint32_t max_depth{0};
  bool clean{false};

  [[nodiscard]] std::string to_text() const;
};

LockAuditReport lock_audit_report() noexcept;
void lock_audit_reset() noexcept;

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_LOCK_AUDIT_HPP
