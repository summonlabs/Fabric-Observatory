// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/status.hpp"

#include "fabric_observatory/version.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace fabric_observatory {

void fail_fast(const char* expression, const char* file, int line, const char* message) noexcept {
  std::fprintf(stderr, "fabric-observatory: fatal: %s (%s:%d): %s\n",
               message != nullptr ? message : "contract violation",
               file != nullptr ? file : "<unknown>",
               line,
               expression != nullptr ? expression : "<no expression>");
  std::fflush(stderr);
  std::abort();
}

std::string_view version_string() noexcept { return "1.0.0"; }

std::string_view observation_schema() noexcept { return FABRIC_OBSERVATORY_OBSERVATION_SCHEMA; }

namespace {

struct StatusName {
  StatusCode code;
  std::string_view name;
};

constexpr StatusName kStatusNames[] = {
    {StatusCode::Ok, "Ok"},
    {StatusCode::InvalidArgument, "InvalidArgument"},
    {StatusCode::OutOfRange, "OutOfRange"},
    {StatusCode::NotFound, "NotFound"},
    {StatusCode::AlreadyExists, "AlreadyExists"},
    {StatusCode::Unsupported, "Unsupported"},
    {StatusCode::NotPermittedByBoundary, "NotPermittedByBoundary"},
    {StatusCode::ResourceExhausted, "ResourceExhausted"},
    {StatusCode::QueueFull, "QueueFull"},
    {StatusCode::TooLarge, "TooLarge"},
    {StatusCode::LimitExceeded, "LimitExceeded"},
    {StatusCode::DepthExceeded, "DepthExceeded"},
    {StatusCode::MalformedInput, "MalformedInput"},
    {StatusCode::IntegrityFailure, "IntegrityFailure"},
    {StatusCode::VersionMismatch, "VersionMismatch"},
    {StatusCode::Corrupt, "Corrupt"},
    {StatusCode::FencedSequence, "FencedSequence"},
    {StatusCode::FencedIncarnation, "FencedIncarnation"},
    {StatusCode::FencedStaleGeneration, "FencedStaleGeneration"},
    {StatusCode::FencedStaleEpoch, "FencedStaleEpoch"},
    {StatusCode::FencedAuthority, "FencedAuthority"},
    {StatusCode::FencedRevision, "FencedRevision"},
    {StatusCode::FencedDuplicateContent, "FencedDuplicateContent"},
    {StatusCode::ClockInconsistent, "ClockInconsistent"},
    {StatusCode::FabricMismatch, "FabricMismatch"},
    {StatusCode::SourceUnknown, "SourceUnknown"},
    {StatusCode::IoError, "IoError"},
    {StatusCode::Cancelled, "Cancelled"},
    {StatusCode::ShuttingDown, "ShuttingDown"},
    {StatusCode::NotOpen, "NotOpen"},
    {StatusCode::Internal, "Internal"},
};

}  // namespace

std::string_view to_string(StatusCode code) noexcept {
  for (const StatusName& entry : kStatusNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return "Unknown";
}

std::optional<StatusCode> status_code_from_string(std::string_view text) noexcept {
  for (const StatusName& entry : kStatusNames) {
    if (entry.name == text) {
      return entry.code;
    }
  }
  return std::nullopt;
}

std::string Status::to_string() const {
  std::string result(fabric_observatory::to_string(code_));
  if (!message_.empty()) {
    result += ": ";
    result += message_;
  }
  return result;
}

}  // namespace fabric_observatory
