// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_ASPECT_HPP
#define FABRIC_OBSERVATORY_ASPECT_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "fabric_observatory/status.hpp"
#include "fabric_observatory/truth.hpp"

namespace fabric_observatory {

// The thirteen observational domains this runtime owns a global view of. The
// boundary is deliberately narrower than the fabric itself: topology is
// presented as observed, never discovered; paths are presented as observed,
// never computed.
enum class Domain : std::uint8_t {
  Identity = 0,
  Topology = 1,
  Link = 2,
  Device = 3,
  Port = 4,
  Path = 5,
  Capacity = 6,
  Reachability = 7,
  Authority = 8,
  Failure = 9,
  Partition = 10,
  Congestion = 11,
  OperationalState = 12,
};

inline constexpr std::size_t kDomainCount = 13;

std::string_view to_string(Domain domain) noexcept;
std::optional<Domain> domain_from_string(std::string_view text) noexcept;

// An aspect is "<domain>.<name>", lower-case, bounded in length. Parsing
// guarantees that the domain prefix names one of the thirteen domains, so an
// aspect always belongs to a known domain or is rejected as unsupported input.
class AspectId {
 public:
  AspectId() = default;

  [[nodiscard]] static Result<AspectId> parse(std::string_view text);
  [[nodiscard]] static constexpr std::size_t max_length() noexcept { return 64; }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] Domain domain() const noexcept { return domain_; }

  friend bool operator==(const AspectId&, const AspectId&) = default;
  friend auto operator<=>(const AspectId&, const AspectId&) = default;

 private:
  std::string value_{};
  Domain domain_{Domain::Identity};
};

// Static description of a well-known aspect. Requirements are policy defaults;
// a Policy may override them, and every snapshot records the policy digest that
// produced it so an override is never invisible.
struct AspectDescriptor {
  std::string_view name;
  Domain domain;
  std::uint32_t required_distinct_fresh_sources;
  SourceAuthority required_authority;
  std::string_view description;
};

std::span<const AspectDescriptor> well_known_aspects() noexcept;
const AspectDescriptor* find_aspect_descriptor(std::string_view name) noexcept;

// A convenience for building the aspect identifiers of the well-known set.
Result<AspectId> well_known_aspect(std::string_view name);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_ASPECT_HPP
