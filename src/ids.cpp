// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/ids.hpp"

#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/digest.hpp"
#include "fabric_observatory/util.hpp"

#include <array>
#include <cstring>

namespace fabric_observatory {

namespace {

constexpr std::array<std::string_view, kEntityKindCount> kEntityKindNames = {
    "fabric", "site", "pod", "rack", "device", "port", "link", "path"};

constexpr std::string_view kEntityTextPrefixes[kEntityKindCount] = {
    "fabric/", "site/", "pod/", "rack/", "device/", "port/", "link/", "path/"};

}  // namespace

std::string_view to_string(EntityKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= kEntityKindCount) {
    return "unknown";
  }
  return kEntityKindNames[index];
}

Result<EntityKind> entity_kind_from_string(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kEntityKindCount; ++index) {
    if (kEntityKindNames[index] == text) {
      return Ok(static_cast<EntityKind>(index));
    }
  }
  return Err<EntityKind>(StatusCode::InvalidArgument, "unknown entity kind");
}

Uint128 derive_u128(std::string_view domain, ByteSpan canonical_bytes) {
  Sha256 hasher;
  const auto domain_length = static_cast<std::uint8_t>(domain.size() & 0xFFu);
  hasher.update_byte(domain_length);
  hasher.update(domain);
  hasher.update(canonical_bytes);
  const std::array<std::uint8_t, 32> digest = hasher.finish();

  Uint128 value;
  for (std::size_t index = 0; index < 8; ++index) {
    value.hi = (value.hi << 8u) | static_cast<std::uint64_t>(digest[index]);
    value.lo = (value.lo << 8u) | static_cast<std::uint64_t>(digest[index + 8]);
  }
  return value;
}

Uint128 derive_u128(std::string_view domain, std::string_view canonical_text) {
  return derive_u128(domain,
                     ByteSpan(reinterpret_cast<const std::byte*>(canonical_text.data()),
                              canonical_text.size()));
}

std::string u128_to_hex(Uint128 value) {
  std::array<std::byte, 16> raw{};
  for (std::size_t index = 0; index < 8; ++index) {
    raw[index] = static_cast<std::byte>((value.hi >> (8u * (7 - index))) & 0xFFu);
    raw[index + 8] = static_cast<std::byte>((value.lo >> (8u * (7 - index))) & 0xFFu);
  }
  return util::hex_encode(ByteSpan(raw.data(), raw.size()));
}

Result<Uint128> u128_from_hex(std::string_view text) {
  if (text.size() != 32) {
    return Err<Uint128>(StatusCode::InvalidArgument,
                        "identity hex text must be exactly 32 characters");
  }
  Result<std::vector<std::byte>> decoded = util::hex_decode(text);
  if (!decoded) {
    return Err<Uint128>(decoded.status().code(), decoded.status().message());
  }
  Uint128 value;
  for (std::size_t index = 0; index < 8; ++index) {
    value.hi = (value.hi << 8u) | static_cast<std::uint64_t>(std::to_integer<std::uint8_t>((*decoded)[index]));
    value.lo = (value.lo << 8u) |
               static_cast<std::uint64_t>(std::to_integer<std::uint8_t>((*decoded)[index + 8]));
  }
  return Ok(value);
}

std::string PortId::to_text() const {
  return "port:" + device_.to_text() + "/" + std::to_string(index_);
}

Result<PortId> PortId::from_text(std::string_view text) {
  constexpr std::string_view kPrefix = "port:";
  if (util::starts_with(text, kPrefix)) {
    text.remove_prefix(kPrefix.size());
  }
  const std::size_t slash = text.rfind('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= text.size()) {
    return Err<PortId>(StatusCode::MalformedInput,
                       "port identity must have the form port:<device>/<index>");
  }
  Result<DeviceId> device = DeviceId::from_text(text.substr(0, slash));
  if (!device) {
    return Err<PortId>(device.status().code(), device.status().message());
  }
  const std::string_view index_text = text.substr(slash + 1);
  std::uint64_t index = 0;
  for (const char ch : index_text) {
    if (ch < '0' || ch > '9') {
      return Err<PortId>(StatusCode::MalformedInput, "port index must be a decimal number");
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    const std::optional<std::uint64_t> scaled = checked_mul(index, std::uint64_t{10});
    if (!scaled.has_value()) {
      return Err<PortId>(StatusCode::OutOfRange, "port index overflows");
    }
    const std::optional<std::uint64_t> next = checked_add(*scaled, digit);
    if (!next.has_value()) {
      return Err<PortId>(StatusCode::OutOfRange, "port index overflows");
    }
    index = *next;
  }
  const std::optional<std::uint32_t> narrowed = checked_u32(index);
  if (!narrowed.has_value()) {
    return Err<PortId>(StatusCode::OutOfRange, "port index exceeds the supported range");
  }
  return Ok(PortId(*device, *narrowed));
}

Result<LinkId> LinkId::make(PortId first, PortId second) {
  if (first.device().is_nil() || second.device().is_nil()) {
    return Err<LinkId>(StatusCode::InvalidArgument, "link endpoints must be real ports");
  }
  if (first == second) {
    return Err<LinkId>(StatusCode::InvalidArgument, "a link requires two distinct ports");
  }
  LinkId link;
  if (second < first) {
    link.local_ = second;
    link.remote_ = first;
  } else {
    link.local_ = first;
    link.remote_ = second;
  }
  return Ok(link);
}

std::string LinkId::to_text() const {
  return "link:" + local_.to_text() + "|" + remote_.to_text();
}

Result<LinkId> LinkId::from_text(std::string_view text) {
  constexpr std::string_view kPrefix = "link:";
  if (util::starts_with(text, kPrefix)) {
    text.remove_prefix(kPrefix.size());
  }
  const std::size_t bar = text.find('|');
  if (bar == std::string_view::npos) {
    return Err<LinkId>(StatusCode::MalformedInput,
                       "link identity must have the form link:<local>|<remote>");
  }
  Result<PortId> local = PortId::from_text(text.substr(0, bar));
  if (!local) {
    return Err<LinkId>(local.status().code(), local.status().message());
  }
  Result<PortId> remote = PortId::from_text(text.substr(bar + 1));
  if (!remote) {
    return Err<LinkId>(remote.status().code(), remote.status().message());
  }
  return make(*local, *remote);
}

PathId PathId::from_hops(std::span<const LinkId> hops) {
  Sha256 hasher;
  const std::string_view domain = "path";
  hasher.update_byte(static_cast<std::uint8_t>(domain.size()));
  hasher.update(domain);
  const auto count = static_cast<std::uint64_t>(hops.size());
  for (std::size_t index = 0; index < 8; ++index) {
    const std::uint8_t byte = static_cast<std::uint8_t>((count >> (8u * (7 - index))) & 0xFFu);
    hasher.update_byte(byte);
  }
  for (const LinkId& hop : hops) {
    const std::string hop_text = hop.to_text();
    hasher.update_byte(static_cast<std::uint8_t>(hop_text.size() & 0xFFu));
    hasher.update(hop_text);
  }
  const std::array<std::uint8_t, 32> digest = hasher.finish();
  Uint128 value;
  for (std::size_t index = 0; index < 8; ++index) {
    value.hi = (value.hi << 8u) | static_cast<std::uint64_t>(digest[index]);
    value.lo = (value.lo << 8u) | static_cast<std::uint64_t>(digest[index + 8]);
  }
  return PathId(value);
}

std::string PathId::to_text() const { return "path:" + u128_to_hex(value_); }

Result<PathId> PathId::from_text(std::string_view text) {
  constexpr std::string_view kPrefix = "path:";
  if (util::starts_with(text, kPrefix)) {
    text.remove_prefix(kPrefix.size());
  }
  Result<Uint128> value = u128_from_hex(text);
  if (!value) {
    return Err<PathId>(value.status().code(), value.status().message());
  }
  return Ok(PathId(*value));
}

std::string EntityId::to_text() const { return "e:" + u128_to_hex(value_); }

Result<EntityId> EntityId::from_text(std::string_view text) {
  constexpr std::string_view kPrefix = "e:";
  if (util::starts_with(text, kPrefix)) {
    text.remove_prefix(kPrefix.size());
  }
  Result<Uint128> value = u128_from_hex(text);
  if (!value) {
    return Err<EntityId>(value.status().code(), value.status().message());
  }
  return Ok(EntityId(*value));
}

std::string EntityRef::to_text() const {
  return std::string(fabric_observatory::to_string(kind)) + "/" + u128_to_hex(id.value());
}

Result<EntityRef> EntityRef::from_text(std::string_view text) {
  const std::size_t slash = text.find('/');
  if (slash == std::string_view::npos) {
    return Err<EntityRef>(StatusCode::MalformedInput,
                          "entity reference must have the form <kind>/<hex>");
  }
  Result<EntityKind> kind = entity_kind_from_string(text.substr(0, slash));
  if (!kind) {
    return Err<EntityRef>(kind.status().code(), kind.status().message());
  }
  Result<EntityId> id = EntityId::from_text(text.substr(slash + 1));
  if (!id) {
    return Err<EntityRef>(id.status().code(), id.status().message());
  }
  EntityRef ref;
  ref.kind = *kind;
  ref.id = *id;
  return Ok(ref);
}

namespace {

template <class TypedId>
EntityId derive_entity(EntityKind kind, const TypedId& typed) {
  const std::string canonical =
      std::string(kEntityTextPrefixes[static_cast<std::size_t>(kind)]) + typed.to_text();
  return EntityId(derive_u128("entity", canonical));
}

}  // namespace

EntityId entity_id_of(FabricId id) { return derive_entity(EntityKind::Fabric, id); }
EntityId entity_id_of(SiteId id) { return derive_entity(EntityKind::Site, id); }
EntityId entity_id_of(PodId id) { return derive_entity(EntityKind::Pod, id); }
EntityId entity_id_of(RackId id) { return derive_entity(EntityKind::Rack, id); }
EntityId entity_id_of(DeviceId id) { return derive_entity(EntityKind::Device, id); }
EntityId entity_id_of(PortId id) { return derive_entity(EntityKind::Port, id); }
EntityId entity_id_of(LinkId id) { return derive_entity(EntityKind::Link, id); }
EntityId entity_id_of(PathId id) { return derive_entity(EntityKind::Path, id); }

EntityRef entity_ref_of(FabricId id) { return EntityRef{EntityKind::Fabric, entity_id_of(id)}; }
EntityRef entity_ref_of(SiteId id) { return EntityRef{EntityKind::Site, entity_id_of(id)}; }
EntityRef entity_ref_of(PodId id) { return EntityRef{EntityKind::Pod, entity_id_of(id)}; }
EntityRef entity_ref_of(RackId id) { return EntityRef{EntityKind::Rack, entity_id_of(id)}; }
EntityRef entity_ref_of(DeviceId id) { return EntityRef{EntityKind::Device, entity_id_of(id)}; }
EntityRef entity_ref_of(PortId id) { return EntityRef{EntityKind::Port, entity_id_of(id)}; }
EntityRef entity_ref_of(LinkId id) { return EntityRef{EntityKind::Link, entity_id_of(id)}; }
EntityRef entity_ref_of(PathId id) { return EntityRef{EntityKind::Path, entity_id_of(id)}; }

namespace {

template <class TypedId>
SubjectIdentity typed_subject(EntityKind kind, const TypedId& typed) {
  SubjectIdentity identity;
  identity.kind = kind;
  identity.id = entity_id_of(typed);
  return identity;
}

}  // namespace

SubjectIdentity SubjectIdentity::of(FabricId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Fabric, value);
  identity.fabric = value;
  return identity;
}

SubjectIdentity SubjectIdentity::of(SiteId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Site, value);
  identity.site = value;
  return identity;
}

SubjectIdentity SubjectIdentity::of(PodId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Pod, value);
  identity.pod = value;
  return identity;
}

SubjectIdentity SubjectIdentity::of(RackId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Rack, value);
  identity.rack = value;
  return identity;
}

SubjectIdentity SubjectIdentity::of(DeviceId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Device, value);
  identity.device = value;
  return identity;
}

SubjectIdentity SubjectIdentity::of(PortId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Port, value);
  identity.port = value;
  return identity;
}

SubjectIdentity SubjectIdentity::of(LinkId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Link, value);
  identity.link = value;
  return identity;
}

SubjectIdentity SubjectIdentity::of(PathId value) {
  SubjectIdentity identity = typed_subject(EntityKind::Path, value);
  identity.path = value;
  return identity;
}

std::string SubjectIdentity::typed_text() const {
  const std::string_view kind_text = fabric_observatory::to_string(kind);
  switch (kind) {
    case EntityKind::Fabric:
      return std::string(kind_text) + "/" + fabric.to_text();
    case EntityKind::Site:
      return std::string(kind_text) + "/" + site.to_text();
    case EntityKind::Pod:
      return std::string(kind_text) + "/" + pod.to_text();
    case EntityKind::Rack:
      return std::string(kind_text) + "/" + rack.to_text();
    case EntityKind::Device:
      return std::string(kind_text) + "/" + device.to_text();
    case EntityKind::Port:
      return std::string(kind_text) + "/" + port.to_text();
    case EntityKind::Link:
      return std::string(kind_text) + "/" + link.to_text();
    case EntityKind::Path:
      return std::string(kind_text) + "/" + path.to_text();
  }
  return std::string(kind_text) + "/";
}

Result<SubjectIdentity> SubjectIdentity::from_text(std::string_view text) {
  const std::size_t slash = text.find('/');
  if (slash == std::string_view::npos) {
    return Err<SubjectIdentity>(StatusCode::MalformedInput,
                                "typed identity must have the form <kind>/<identity>");
  }
  Result<EntityKind> kind = entity_kind_from_string(text.substr(0, slash));
  if (!kind) {
    return Err<SubjectIdentity>(kind.status().code(), kind.status().message());
  }
  const std::string_view body = text.substr(slash + 1);
  switch (*kind) {
    case EntityKind::Fabric: {
      Result<FabricId> typed = FabricId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
    case EntityKind::Site: {
      Result<SiteId> typed = SiteId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
    case EntityKind::Pod: {
      Result<PodId> typed = PodId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
    case EntityKind::Rack: {
      Result<RackId> typed = RackId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
    case EntityKind::Device: {
      Result<DeviceId> typed = DeviceId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
    case EntityKind::Port: {
      Result<PortId> typed = PortId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
    case EntityKind::Link: {
      Result<LinkId> typed = LinkId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
    case EntityKind::Path: {
      Result<PathId> typed = PathId::from_text(body);
      if (!typed) {
        return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
      }
      return Ok(SubjectIdentity::of(*typed));
    }
  }
  return Err<SubjectIdentity>(StatusCode::Internal, "unreachable entity kind");
}

}  // namespace fabric_observatory
