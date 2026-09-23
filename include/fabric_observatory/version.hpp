// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_VERSION_HPP
#define FABRIC_OBSERVATORY_VERSION_HPP

#include <cstdint>
#include <string_view>

#define FABRIC_OBSERVATORY_VERSION_MAJOR 1
#define FABRIC_OBSERVATORY_VERSION_MINOR 0
#define FABRIC_OBSERVATORY_VERSION_PATCH 0

// The observation wire schema this runtime emits and accepts by default.
#define FABRIC_OBSERVATORY_OBSERVATION_SCHEMA "fabric-observatory/observation/1"
// The on-disk journal format version. Bumping it is a compatibility break.
#define FABRIC_OBSERVATORY_JOURNAL_FORMAT_VERSION 1

namespace fabric_observatory {

struct Version {
  std::uint32_t major{};
  std::uint32_t minor{};
  std::uint32_t patch{};

  friend constexpr bool operator==(const Version&, const Version&) = default;
  friend constexpr auto operator<=>(const Version&, const Version&) = default;
};

inline constexpr Version kRuntimeVersion{FABRIC_OBSERVATORY_VERSION_MAJOR,
                                         FABRIC_OBSERVATORY_VERSION_MINOR,
                                         FABRIC_OBSERVATORY_VERSION_PATCH};

inline constexpr std::uint32_t kJournalFormatVersion = FABRIC_OBSERVATORY_JOURNAL_FORMAT_VERSION;

std::string_view version_string() noexcept;
std::string_view observation_schema() noexcept;

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_VERSION_HPP
