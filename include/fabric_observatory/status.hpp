// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_STATUS_HPP
#define FABRIC_OBSERVATORY_STATUS_HPP

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "fabric_observatory/export.hpp"

namespace fabric_observatory {

// Every failure mode of the runtime is a distinct, named condition. Fence
// conditions are separate codes because they are expected operational events,
// not errors: a replayed observation is fenced, not lost.
enum class StatusCode : std::uint16_t {
  Ok = 0,

  // Caller contract failures.
  InvalidArgument = 1,
  OutOfRange = 2,
  NotFound = 3,
  AlreadyExists = 4,

  // Capability boundaries. The runtime reports what it cannot do rather than
  // guessing.
  Unsupported = 5,
  NotPermittedByBoundary = 6,

  // Bounded-resource conditions.
  ResourceExhausted = 7,
  QueueFull = 8,
  TooLarge = 9,
  LimitExceeded = 10,
  DepthExceeded = 11,

  // Input integrity.
  MalformedInput = 12,
  IntegrityFailure = 13,
  VersionMismatch = 14,
  Corrupt = 15,

  // Provenance and ordering fences.
  FencedSequence = 16,
  FencedIncarnation = 17,
  FencedStaleGeneration = 18,
  FencedStaleEpoch = 19,
  FencedAuthority = 20,
  FencedRevision = 21,
  FencedDuplicateContent = 22,
  ClockInconsistent = 23,
  FabricMismatch = 24,
  SourceUnknown = 25,

  // Lifecycle.
  IoError = 26,
  Cancelled = 27,
  ShuttingDown = 28,
  NotOpen = 29,
  Internal = 30,
};

std::string_view to_string(StatusCode code) noexcept;
std::optional<StatusCode> status_code_from_string(std::string_view text) noexcept;

class Status {
 public:
  Status() noexcept = default;

  static Status error(StatusCode code, std::string message) {
    FABRIC_OBSERVATORY_CONTRACT(code != StatusCode::Ok);
    return Status(code, std::move(message));
  }

  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Status& lhs, const Status& rhs) noexcept {
    return lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_;
  }

 private:
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  StatusCode code_{StatusCode::Ok};
  std::string message_{};
};

// Result<T> carries exactly one of a value or a non-ok Status. The invariant is
// established by construction; there is no partially valid state.
template <class T>
class Result {
 public:
  // Result<Status> would be ambiguous between "a value that happens to be a
  // Status" and "a failure"; the runtime never uses it. Every fallible operation
  // returns Result<void> or Result<T> with T distinct from Status.
  Result(T value)
    requires(!std::same_as<T, Status>)
      : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Status status) : storage_(std::in_place_index<1>, std::move(status)) {
    FABRIC_OBSERVATORY_CONTRACT_MSG(!std::get<1>(storage_).ok(),
                                    "Result constructed from an ok Status without a value");
  }

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T& value() const& {
    FABRIC_OBSERVATORY_CONTRACT(has_value());
    return std::get<0>(storage_);
  }
  [[nodiscard]] T& value() & {
    FABRIC_OBSERVATORY_CONTRACT(has_value());
    return std::get<0>(storage_);
  }
  [[nodiscard]] T&& value() && {
    FABRIC_OBSERVATORY_CONTRACT(has_value());
    return std::move(std::get<0>(storage_));
  }

  [[nodiscard]] const T* operator->() const { return &value(); }
  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T& operator*() & { return value(); }

  [[nodiscard]] const Status& status() const noexcept {
    static const Status kOk{};
    return has_value() ? kOk : std::get<1>(storage_);
  }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Status> storage_;
};

template <>
class Result<void> {
 public:
  Result() noexcept = default;
  Result(Status status) : status_(std::move(status)) {}

  [[nodiscard]] bool has_value() const noexcept { return status_.ok(); }
  explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  Status status_{};
};

using VoidResult = Result<void>;

inline Status OkStatus() noexcept { return Status{}; }

template <class T>
Result<T> Ok(T value) {
  return Result<T>(std::move(value));
}

template <class T = void>
Result<T> Err(StatusCode code, std::string message) {
  return Result<T>(Status::error(code, std::move(message)));
}

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_STATUS_HPP
