// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/aspect.hpp"

#include "fabric_observatory/util.hpp"

#include <array>

namespace fabric_observatory {

namespace {

constexpr std::array<std::string_view, kDomainCount> kDomainNames = {
    "identity", "topology", "link",        "device", "port",     "path", "capacity",
    "reachability", "authority", "failure", "partition", "congestion", "operational"};

// Well-known aspects. Everything here is observational: each entry names a fact
// about the fabric that an adjacent system can report, not an action this
// runtime takes.
constexpr std::array<AspectDescriptor, 14> kWellKnownAspects = {{
    {"identity.labels", Domain::Identity, 1, SourceAuthority::None,
     "Stable labels attached to an identity by its owner."},
    {"topology.parent", Domain::Topology, 1, SourceAuthority::None,
     "The containment parent of an entity as reported by a topology source."},
    {"link.state", Domain::Link, 1, SourceAuthority::Reported,
     "Observed operational state of a link."},
    {"device.model", Domain::Device, 1, SourceAuthority::None,
     "Vendor, model and serial information reported for a device."},
    {"port.state", Domain::Port, 1, SourceAuthority::Reported,
     "Observed operational state of a port."},
    {"path.state", Domain::Path, 2, SourceAuthority::Reported,
     "Observed state of a path, requiring independent corroboration."},
    {"capacity.bandwidth_bps", Domain::Capacity, 1, SourceAuthority::Reported,
     "Observed usable bandwidth of an entity in bits per second."},
    {"reachability.state", Domain::Reachability, 2, SourceAuthority::Corroborated,
     "Whether an endpoint was observed to be reachable, requiring corroboration."},
    {"authority.owner", Domain::Authority, 1, SourceAuthority::Authoritative,
     "The system that owns the authority for an entity, reported by that system."},
    {"failure.state", Domain::Failure, 1, SourceAuthority::Reported,
     "Observed failure condition affecting an entity."},
    {"partition.membership", Domain::Partition, 1, SourceAuthority::Reported,
     "Observed partition membership of an entity."},
    {"congestion.level", Domain::Congestion, 1, SourceAuthority::Reported,
     "Observed congestion indicator for a link or port."},
    {"operational.admin_state", Domain::OperationalState, 1, SourceAuthority::Reported,
     "Administrative state reported for an entity."},
    {"operational.health", Domain::OperationalState, 1, SourceAuthority::None,
     "Coarse health indicator reported for an entity."},
}};

}  // namespace

std::string_view to_string(Domain domain) noexcept {
  const auto index = static_cast<std::size_t>(domain);
  return index < kDomainCount ? kDomainNames[index] : std::string_view("unknown");
}

std::optional<Domain> domain_from_string(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kDomainCount; ++index) {
    if (kDomainNames[index] == text) {
      return static_cast<Domain>(index);
    }
  }
  return std::nullopt;
}

Result<AspectId> AspectId::parse(std::string_view text) {
  if (text.empty()) {
    return Err<AspectId>(StatusCode::InvalidArgument, "aspect identifier must not be empty");
  }
  if (text.size() > max_length()) {
    return Err<AspectId>(StatusCode::TooLarge, "aspect identifier exceeds 64 characters");
  }
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size()) {
    return Err<AspectId>(StatusCode::InvalidArgument,
                         "aspect identifier must have the form <domain>.<name>");
  }
  const std::optional<Domain> domain = domain_from_string(text.substr(0, dot));
  if (!domain.has_value()) {
    return Err<AspectId>(StatusCode::Unsupported,
                         "aspect identifier does not begin with a known domain");
  }
  // Only the domain prefix is validated against the fixed vocabulary; the name
  // part is restricted to the identifier alphabet so that canonical text and
  // digests stay unambiguous.
  if (!util::is_valid_identifier(text, max_length())) {
    return Err<AspectId>(StatusCode::InvalidArgument,
                         "aspect identifier contains characters outside [a-z0-9._-]");
  }
  AspectId aspect;
  aspect.value_.assign(text);
  aspect.domain_ = *domain;
  return Ok(std::move(aspect));
}

std::span<const AspectDescriptor> well_known_aspects() noexcept {
  return std::span<const AspectDescriptor>(kWellKnownAspects.data(), kWellKnownAspects.size());
}

const AspectDescriptor* find_aspect_descriptor(std::string_view name) noexcept {
  for (const AspectDescriptor& descriptor : kWellKnownAspects) {
    if (descriptor.name == name) {
      return &descriptor;
    }
  }
  return nullptr;
}

Result<AspectId> well_known_aspect(std::string_view name) {
  if (find_aspect_descriptor(name) == nullptr) {
    return Err<AspectId>(StatusCode::NotFound, "no such well-known aspect");
  }
  return AspectId::parse(name);
}

}  // namespace fabric_observatory
