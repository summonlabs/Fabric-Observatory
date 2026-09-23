// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_IDS_HPP
#define FABRIC_OBSERVATORY_IDS_HPP

#include <compare>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "fabric_observatory/digest.hpp"
#include "fabric_observatory/status.hpp"

// Strongly typed identities. Each domain object has its own type; two
// identifiers of different kinds are never interchangeable, and a raw integer
// is never accepted where an identity is expected.

namespace fabric_observatory {

struct Uint128 {
  std::uint64_t hi{0};
  std::uint64_t lo{0};

  friend constexpr bool operator==(const Uint128&, const Uint128&) = default;
  friend constexpr auto operator<=>(const Uint128&, const Uint128&) = default;

  [[nodiscard]] constexpr bool is_zero() const noexcept { return hi == 0 && lo == 0; }
};

// Deterministic 128-bit derivation: the leading 16 bytes of SHA-256 over a
// domain-separated canonical text. No random component, no host state.
[[nodiscard]] Uint128 derive_u128(std::string_view domain, std::string_view canonical_text);
[[nodiscard]] Uint128 derive_u128(std::string_view domain, ByteSpan canonical_bytes);
[[nodiscard]] std::string u128_to_hex(Uint128 value);
[[nodiscard]] Result<Uint128> u128_from_hex(std::string_view text);

struct FabricTag;
struct SiteTag;
struct PodTag;
struct RackTag;
struct DeviceTag;
struct SourceTag;
struct IncarnationTag;

template <class Tag>
struct IdPrefix;

template <>
struct IdPrefix<FabricTag> {
  static constexpr std::string_view value = "fab";
};
template <>
struct IdPrefix<SiteTag> {
  static constexpr std::string_view value = "site";
};
template <>
struct IdPrefix<PodTag> {
  static constexpr std::string_view value = "pod";
};
template <>
struct IdPrefix<RackTag> {
  static constexpr std::string_view value = "rack";
};
template <>
struct IdPrefix<DeviceTag> {
  static constexpr std::string_view value = "dev";
};
template <>
struct IdPrefix<SourceTag> {
  static constexpr std::string_view value = "src";
};
template <>
struct IdPrefix<IncarnationTag> {
  static constexpr std::string_view value = "inc";
};

template <class Tag>
class BasicId {
 public:
  using tag_type = Tag;

  constexpr BasicId() noexcept = default;
  constexpr explicit BasicId(Uint128 value) noexcept : value_(value) {}

  [[nodiscard]] static BasicId from_u128(Uint128 value) noexcept { return BasicId(value); }

  // Derives the identity from canonical text. Dots and slashes in the text are
  // insignificant to the derivation only if the caller normalises them first;
  // this function hashes exactly what it is given.
  [[nodiscard]] static BasicId derive(std::string_view canonical_text) {
    return BasicId(derive_u128(IdPrefix<Tag>::value, canonical_text));
  }

  // Accepts "<prefix>:<32 hex>" and a bare 32-character hex string.
  [[nodiscard]] static Result<BasicId> from_text(std::string_view text) {
    Result<Uint128> parsed = parse_u128_text(text);
    if (!parsed) {
      return Err<BasicId>(parsed.status().code(), parsed.status().message());
    }
    return Ok(BasicId(*parsed));
  }

  [[nodiscard]] constexpr Uint128 value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_.is_zero(); }
  [[nodiscard]] std::string to_text() const {
    return std::string(IdPrefix<Tag>::value) + ":" + u128_to_hex(value_);
  }

  friend constexpr bool operator==(const BasicId&, const BasicId&) = default;
  friend constexpr auto operator<=>(const BasicId&, const BasicId&) = default;

 private:
  [[nodiscard]] static Result<Uint128> parse_u128_text(std::string_view text) {
    if (text.size() > IdPrefix<Tag>::value.size() &&
        text.substr(0, IdPrefix<Tag>::value.size()) == IdPrefix<Tag>::value &&
        text[IdPrefix<Tag>::value.size()] == ':') {
      text.remove_prefix(IdPrefix<Tag>::value.size() + 1);
    }
    return u128_from_hex(text);
  }

  Uint128 value_{};
};

using FabricId = BasicId<FabricTag>;
using SiteId = BasicId<SiteTag>;
using PodId = BasicId<PodTag>;
using RackId = BasicId<RackTag>;
using DeviceId = BasicId<DeviceTag>;
using SourceId = BasicId<SourceTag>;
using IncarnationId = BasicId<IncarnationTag>;

// Port identity is composed: a port is only meaningful with respect to the
// device that owns it.
class PortId {
 public:
  constexpr PortId() noexcept = default;
  constexpr PortId(DeviceId device, std::uint32_t index) noexcept : device_(device), index_(index) {}

  [[nodiscard]] constexpr DeviceId device() const noexcept { return device_; }
  [[nodiscard]] constexpr std::uint32_t index() const noexcept { return index_; }

  [[nodiscard]] static Result<PortId> from_text(std::string_view text);
  [[nodiscard]] std::string to_text() const;

  friend constexpr bool operator==(const PortId&, const PortId&) = default;
  friend constexpr auto operator<=>(const PortId&, const PortId&) = default;

 private:
  DeviceId device_{};
  std::uint32_t index_{0};
};

// A link joins two distinct ports. The pair is stored in canonical order, so a
// link observed from either end has exactly one identity.
class LinkId {
 public:
  constexpr LinkId() noexcept = default;

  [[nodiscard]] static Result<LinkId> make(PortId first, PortId second);

  [[nodiscard]] constexpr PortId local() const noexcept { return local_; }
  [[nodiscard]] constexpr PortId remote() const noexcept { return remote_; }

  [[nodiscard]] static Result<LinkId> from_text(std::string_view text);
  [[nodiscard]] std::string to_text() const;

  friend constexpr bool operator==(const LinkId&, const LinkId&) = default;
  friend constexpr auto operator<=>(const LinkId&, const LinkId&) = default;

 private:
  PortId local_{};
  PortId remote_{};
};

// A path is identified by its ordered hop sequence: two paths with different
// hops are different paths, and the same hops in the same order are the same
// path regardless of who observed it.
class PathId {
 public:
  constexpr PathId() noexcept = default;
  constexpr explicit PathId(Uint128 value) noexcept : value_(value) {}

  [[nodiscard]] static PathId from_hops(std::span<const LinkId> hops);
  [[nodiscard]] static Result<PathId> from_text(std::string_view text);
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] constexpr Uint128 value() const noexcept { return value_; }

  friend constexpr bool operator==(const PathId&, const PathId&) = default;
  friend constexpr auto operator<=>(const PathId&, const PathId&) = default;

 private:
  Uint128 value_{};
};

// Monotonic counters that carry their own semantics.
template <class Tag>
class StrongCounter {
 public:
  using rep = std::uint64_t;

  constexpr StrongCounter() noexcept = default;
  constexpr explicit StrongCounter(rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr StrongCounter next() const noexcept { return StrongCounter(value_ + 1); }

  friend constexpr bool operator==(const StrongCounter&, const StrongCounter&) = default;
  friend constexpr auto operator<=>(const StrongCounter&, const StrongCounter&) = default;

 private:
  rep value_{};
};

using GenerationId = StrongCounter<struct GenerationTag>;
using EpochId = StrongCounter<struct EpochTag>;
using SourceSequence = StrongCounter<struct SourceSequenceTag>;
using ClaimRevision = StrongCounter<struct ClaimRevisionTag>;
using HistoryIndex = StrongCounter<struct HistoryIndexTag>;
using RestartEpoch = StrongCounter<struct RestartEpochTag>;

// Entity identity for the generic storage and query layer. Domain identities
// keep their strong types at every API boundary; the canonical 128-bit entity
// identifier is what the store indexes on, and the derivation is total and
// injective over the canonical text forms below.
enum class EntityKind : std::uint8_t {
  Fabric = 0,
  Site = 1,
  Pod = 2,
  Rack = 3,
  Device = 4,
  Port = 5,
  Link = 6,
  Path = 7,
};

inline constexpr std::size_t kEntityKindCount = 8;

std::string_view to_string(EntityKind kind) noexcept;
Result<EntityKind> entity_kind_from_string(std::string_view text) noexcept;

class EntityId {
 public:
  constexpr EntityId() noexcept = default;
  constexpr explicit EntityId(Uint128 value) noexcept : value_(value) {}

  [[nodiscard]] static Result<EntityId> from_text(std::string_view text);
  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] constexpr Uint128 value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_nil() const noexcept { return value_.is_zero(); }

  friend constexpr bool operator==(const EntityId&, const EntityId&) = default;
  friend constexpr auto operator<=>(const EntityId&, const EntityId&) = default;

 private:
  Uint128 value_{};
};

struct EntityRef {
  EntityKind kind{EntityKind::Fabric};
  EntityId id{};

  friend constexpr bool operator==(const EntityRef&, const EntityRef&) = default;
  friend constexpr auto operator<=>(const EntityRef&, const EntityRef&) = default;

  [[nodiscard]] std::string to_text() const;
  [[nodiscard]] static Result<EntityRef> from_text(std::string_view text);
};

// The typed identity behind an entity identifier. The entity identifier is a
// one-way derivation, so the typed form is carried explicitly rather than
// reconstructed from the hash.
struct SubjectIdentity {
  EntityKind kind{EntityKind::Fabric};
  EntityId id{};
  FabricId fabric{};
  SiteId site{};
  PodId pod{};
  RackId rack{};
  DeviceId device{};
  PortId port{};
  LinkId link{};
  PathId path{};

  [[nodiscard]] static SubjectIdentity of(FabricId value);
  [[nodiscard]] static SubjectIdentity of(SiteId value);
  [[nodiscard]] static SubjectIdentity of(PodId value);
  [[nodiscard]] static SubjectIdentity of(RackId value);
  [[nodiscard]] static SubjectIdentity of(DeviceId value);
  [[nodiscard]] static SubjectIdentity of(PortId value);
  [[nodiscard]] static SubjectIdentity of(LinkId value);
  [[nodiscard]] static SubjectIdentity of(PathId value);

  [[nodiscard]] EntityRef ref() const noexcept { return EntityRef{kind, id}; }
  // Wire and display form: "<kind>/<typed identity>". Unambiguous and fully
  // reversible.
  [[nodiscard]] std::string typed_text() const;
  [[nodiscard]] static Result<SubjectIdentity> from_text(std::string_view text);

  friend bool operator==(const SubjectIdentity&, const SubjectIdentity&) = default;
  friend auto operator<=>(const SubjectIdentity&, const SubjectIdentity&) = default;
};

[[nodiscard]] EntityId entity_id_of(FabricId id);
[[nodiscard]] EntityId entity_id_of(SiteId id);
[[nodiscard]] EntityId entity_id_of(PodId id);
[[nodiscard]] EntityId entity_id_of(RackId id);
[[nodiscard]] EntityId entity_id_of(DeviceId id);
[[nodiscard]] EntityId entity_id_of(PortId id);
[[nodiscard]] EntityId entity_id_of(LinkId id);
[[nodiscard]] EntityId entity_id_of(PathId id);

[[nodiscard]] EntityRef entity_ref_of(FabricId id);
[[nodiscard]] EntityRef entity_ref_of(SiteId id);
[[nodiscard]] EntityRef entity_ref_of(PodId id);
[[nodiscard]] EntityRef entity_ref_of(RackId id);
[[nodiscard]] EntityRef entity_ref_of(DeviceId id);
[[nodiscard]] EntityRef entity_ref_of(PortId id);
[[nodiscard]] EntityRef entity_ref_of(LinkId id);
[[nodiscard]] EntityRef entity_ref_of(PathId id);

// EntityId derivation is deliberately one-way: it is a hash, and nothing in the
// runtime pretends it can be inverted. Where the typed identity is needed it is
// carried next to the entity identifier as a SubjectIdentity (see
// snapshot.hpp), never reconstructed from the hash.

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_IDS_HPP
