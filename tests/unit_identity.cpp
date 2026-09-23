// Unit tests: identities, digests, time, values, JSON, checked arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "fabric_observatory/aspect.hpp"
#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/ids.hpp"
#include "fabric_observatory/time.hpp"
#include "fabric_observatory/json.hpp"
#include "fabric_observatory/util.hpp"
#include "fabric_observatory/value.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace fabric_observatory;

namespace {

std::string hex_of(const Digest256& digest) { return digest.to_hex(); }

}  // namespace

FABOBS_TEST(identity, sha256_known_vectors) {
  FABOBS_CHECK_EQ(hex_of(sha256(std::string_view(""))),
                  std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  FABOBS_CHECK_EQ(hex_of(sha256(std::string_view("abc"))),
                  std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  FABOBS_CHECK_EQ(
      hex_of(sha256(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
      std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  FABOBS_CHECK_EQ(
      hex_of(sha256(std::string_view("The quick brown fox jumps over the lazy dog"))),
      std::string("d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
}

FABOBS_TEST(identity, sha256_incremental_matches_oneshot) {
  std::string payload;
  for (int index = 0; index < 5000; ++index) {
    payload.push_back(static_cast<char>('a' + (index % 26)));
  }
  const Digest256 oneshot = sha256(std::string_view(payload));
  Sha256 hasher;
  std::size_t offset = 0;
  std::size_t step = 1;
  while (offset < payload.size()) {
    const std::size_t take = std::min(step, payload.size() - offset);
    hasher.update(std::string_view(payload).substr(offset, take));
    offset += take;
    step = step * 3 + 1;
    if (step > 997) {
      step = 1;
    }
  }
  FABOBS_CHECK_EQ(Digest256{hasher.finish()}, oneshot);
}

FABOBS_TEST(identity, crc32c_known_vectors) {
  FABOBS_CHECK_EQ(crc32c(std::string_view("")), 0x00000000u);
  FABOBS_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  std::vector<std::byte> zeros(32, std::byte{0});
  FABOBS_CHECK_EQ(crc32c(ByteSpan(zeros.data(), zeros.size())), 0x8A9136AAu);
  std::vector<std::byte> ones(32, std::byte{0xFF});
  FABOBS_CHECK_EQ(crc32c(ByteSpan(ones.data(), ones.size())), 0x62A8AB43u);
}

FABOBS_TEST(identity, digest_text_round_trip) {
  const Digest256 digest = sha256(std::string_view("round trip"));
  Result<Digest256> parsed = Digest256::from_hex(digest.to_hex());
  FABOBS_REQUIRE(parsed.has_value());
  FABOBS_CHECK_EQ(*parsed, digest);
  FABOBS_CHECK(!Digest256::from_hex("abc").has_value());
  FABOBS_CHECK_EQ(Digest256::from_hex(std::string(63, 'a')).status().code(),
                  StatusCode::InvalidArgument);
  FABOBS_CHECK_EQ(digest.to_short_hex().size(), std::size_t{12});
}

FABOBS_TEST(identity, derivation_is_deterministic_and_domain_separated) {
  const DeviceId first = DeviceId::derive("device/leaf-1");
  const DeviceId second = DeviceId::derive("device/leaf-1");
  const DeviceId other = DeviceId::derive("device/leaf-2");
  FABOBS_CHECK_EQ(first, second);
  FABOBS_CHECK(first != other);
  // The same text derived in two domains must not collide.
  const SourceId as_source = SourceId::derive("device/leaf-1");
  FABOBS_CHECK(first.value() != as_source.value());
  FABOBS_CHECK_EQ(DeviceId::from_text(first.to_text()).value(), first);
  FABOBS_CHECK(DeviceId::from_text("not-an-identity").status().code() != StatusCode::Ok);
}

FABOBS_TEST(identity, port_link_path_identities_are_canonical) {
  const DeviceId device_a = DeviceId::derive("device/a");
  const DeviceId device_b = DeviceId::derive("device/b");
  const PortId port_a1(device_a, 1);
  const PortId port_a2(device_a, 2);
  const PortId port_b1(device_b, 1);

  FABOBS_CHECK_EQ(PortId::from_text(port_a1.to_text()).value(), port_a1);
  FABOBS_CHECK(port_a1 != port_a2);

  Result<LinkId> forward = LinkId::make(port_a1, port_b1);
  Result<LinkId> backward = LinkId::make(port_b1, port_a1);
  FABOBS_REQUIRE(forward.has_value());
  FABOBS_REQUIRE(backward.has_value());
  FABOBS_CHECK_EQ(*forward, *backward);
  FABOBS_CHECK_EQ(LinkId::from_text(forward->to_text()).value(), *forward);
  FABOBS_CHECK_EQ(LinkId::make(port_a1, port_a1).status().code(), StatusCode::InvalidArgument);

  const std::vector<LinkId> hops{*forward};
  const PathId path = PathId::from_hops(hops);
  FABOBS_CHECK_EQ(PathId::from_text(path.to_text()).value(), path);
  FABOBS_CHECK_EQ(PathId::from_hops(hops), path);
  const std::vector<LinkId> reversed{*backward};
  FABOBS_CHECK_EQ(PathId::from_hops(reversed), path);
}

FABOBS_TEST(identity, typed_subject_text_is_reversible) {
  const std::vector<SubjectIdentity> subjects = {
      SubjectIdentity::of(FabricId::derive("fabric/a")),
      SubjectIdentity::of(SiteId::derive("site/a")),
      SubjectIdentity::of(PodId::derive("pod/a")),
      SubjectIdentity::of(RackId::derive("rack/a")),
      SubjectIdentity::of(DeviceId::derive("device/a")),
      SubjectIdentity::of(PortId(DeviceId::derive("device/a"), 7)),
      SubjectIdentity::of(*LinkId::make(PortId(DeviceId::derive("device/a"), 1),
                                        PortId(DeviceId::derive("device/b"), 2))),
      SubjectIdentity::of(PathId::from_hops({})),
  };
  for (const SubjectIdentity& subject : subjects) {
    Result<SubjectIdentity> parsed = SubjectIdentity::from_text(subject.typed_text());
    FABOBS_REQUIRE(parsed.has_value());
    FABOBS_CHECK_EQ(*parsed, subject);
  }
  FABOBS_CHECK(!SubjectIdentity::from_text("nonsense").has_value());
  FABOBS_CHECK(!SubjectIdentity::from_text("device/not-an-id").has_value());
}

FABOBS_TEST(identity, time_formatting_is_a_pure_function) {
  FABOBS_CHECK_EQ(format_utc(TimePoint{0}), std::string("1970-01-01T00:00:00.000000000Z"));
  FABOBS_CHECK_EQ(format_utc(TimePoint{1000000000}), std::string("1970-01-01T00:00:01.000000000Z"));
  FABOBS_CHECK_EQ(format_utc(TimePoint{-1}), std::string("1969-12-31T23:59:59.999999999Z"));
  FABOBS_CHECK_EQ(format_utc(TimePoint{1767225600000000000}),
                  std::string("2026-01-01T00:00:00.000000000Z"));
  for (const std::int64_t nanos :
       {std::int64_t{0}, std::int64_t{1}, std::int64_t{-1}, std::int64_t{86399999999999},
        std::int64_t{1767225600123456789}, std::int64_t{-2208988800000000000}}) {
    const Result<TimePoint> parsed = parse_utc(format_utc(TimePoint{nanos}));
    FABOBS_REQUIRE(parsed.has_value());
    FABOBS_CHECK_EQ(parsed->nanos, nanos);
  }
  FABOBS_CHECK_EQ(parse_utc("2026-13-01T00:00:00Z").status().code(), StatusCode::OutOfRange);
  FABOBS_CHECK_EQ(parse_utc("2026-01-01").status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_utc("12345").value().nanos, std::int64_t{12345});
}

FABOBS_TEST(identity, value_canonical_encoding_round_trip) {
  const ValueLimits limits{};
  std::vector<std::pair<std::string, Value>> entries;
  entries.emplace_back("a", Value::integer(-7));
  entries.emplace_back("b", *Value::list({Value::boolean(true), *Value::text("x", limits)}, limits));
  const Value original = *Value::map(
      {{"nested", *Value::map(std::move(entries), limits)},
       {"blob", *Value::blob({std::byte{1}, std::byte{2}, std::byte{3}}, limits)},
       {"real", *Value::real(1.5)},
       {"uint", Value::unsigned_integer(18446744073709551615ull)}},
      limits);

  CanonicalEncoder encoder;
  original.encode(encoder);
  CanonicalDecoder decoder(ByteSpan(encoder.buffer().data(), encoder.buffer().size()));
  Result<Value> decoded = Value::decode(decoder, limits);
  FABOBS_REQUIRE(decoded.has_value());
  FABOBS_CHECK_EQ(*decoded, original);
  FABOBS_CHECK(decoder.ok());
  FABOBS_CHECK(decoder.at_end());

  // Canonical text is injective over the value shapes used by the runtime.
  FABOBS_CHECK(Value::integer(1).canonical_text() != Value::unsigned_integer(1).canonical_text());
  FABOBS_CHECK(Value::integer(1).canonical_text() != Value::real(1.0)->canonical_text());
  FABOBS_CHECK(*Value::text("1", limits) != Value::integer(1));
}

FABOBS_TEST(identity, value_bounds_are_enforced_at_construction) {
  ValueLimits limits;
  limits.max_depth = 3;
  limits.max_list_elements = 4;
  limits.max_map_entries = 2;
  limits.max_text_bytes = 8;
  limits.max_blob_bytes = 2;
  limits.max_total_nodes = 16;

  FABOBS_CHECK_EQ(Value::text(std::string(9, 'x'), limits).status().code(), StatusCode::TooLarge);
  FABOBS_CHECK_EQ(Value::blob({std::byte{1}, std::byte{2}, std::byte{3}}, limits).status().code(),
                  StatusCode::TooLarge);
  FABOBS_CHECK_EQ(Value::list({Value::null(), Value::null(), Value::null(), Value::null(),
                               Value::null()},
                              limits)
                      .status()
                      .code(),
                  StatusCode::TooLarge);
  FABOBS_CHECK_EQ(
      Value::map({{"a", Value::null()}, {"b", Value::null()}, {"c", Value::null()}}, limits)
          .status()
          .code(),
      StatusCode::TooLarge);
  FABOBS_CHECK_EQ(Value::map({{"a", Value::null()}, {"a", Value::null()}}, limits).status().code(),
                  StatusCode::InvalidArgument);
  FABOBS_CHECK_EQ(Value::real(std::numeric_limits<double>::infinity()).status().code(),
                  StatusCode::InvalidArgument);

  Value deep = Value::null();
  for (int level = 0; level < 4; ++level) {
    Result<Value> wrapped = Value::list({deep}, limits);
    if (!wrapped) {
      FABOBS_CHECK_EQ(wrapped.status().code(), StatusCode::DepthExceeded);
      return;
    }
    deep = *wrapped;
  }
  FABOBS_FAIL("depth bound was not enforced");
}

FABOBS_TEST(identity, json_parsing_is_strict_and_bounded) {
  JsonLimits limits;
  FABOBS_CHECK(parse_json("{\"a\":[1,2,{\"b\":null}]}", limits).has_value());
  FABOBS_CHECK(parse_json("\"\\u0041\\uD83D\\uDE00\"", limits).has_value());
  FABOBS_CHECK_EQ(parse_json("", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("{", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("{} trailing", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("{\"a\":1,}", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("01", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("+1", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("\"\\uD800\"", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("\"\\q\"", limits).status().code(), StatusCode::MalformedInput);
  FABOBS_CHECK_EQ(parse_json("[1,2,3,4,5]", limits).has_value(), true);

  JsonLimits shallow;
  shallow.max_depth = 2;
  FABOBS_CHECK_EQ(parse_json("[[[1]]]", shallow).status().code(), StatusCode::DepthExceeded);

  JsonLimits small;
  small.max_input_bytes = 4;
  FABOBS_CHECK_EQ(parse_json("[1,2,3,4,5]", small).status().code(), StatusCode::TooLarge);
}

FABOBS_TEST(identity, value_json_round_trip_preserves_exact_integers) {
  const ValueLimits limits{};
  const std::vector<Value> values = {
      Value::null(),
      Value::boolean(false),
      Value::boolean(true),
      Value::integer(std::numeric_limits<std::int64_t>::min()),
      Value::integer(std::numeric_limits<std::int64_t>::max()),
      Value::unsigned_integer(std::numeric_limits<std::uint64_t>::max()),
      *Value::real(-0.5),
      *Value::text("line\nbreak \"quoted\" \\ backslash", limits),
      *Value::blob({std::byte{0xDE}, std::byte{0xAD}}, limits),
      *Value::list({Value::integer(1), *Value::text("two", limits)}, limits),
      *Value::map({{"k", Value::boolean(true)}}, limits),
  };
  for (const Value& value : values) {
    const JsonValue encoded = value_to_json(value);
    const std::string text = encoded.to_text(false);
    const Result<JsonValue> reparsed = parse_json(text, JsonLimits{});
    FABOBS_REQUIRE(reparsed.has_value());
    const Result<Value> decoded = value_from_json(*reparsed, limits);
    FABOBS_REQUIRE(decoded.has_value());
    FABOBS_CHECK_EQ(*decoded, value);
  }
}

FABOBS_TEST(identity, checked_arithmetic_reports_overflow) {
  FABOBS_CHECK_EQ(*checked_add<std::uint32_t>(1u, 2u), 3u);
  FABOBS_CHECK(!checked_add<std::uint32_t>(0xFFFFFFFFu, 1u).has_value());
  FABOBS_CHECK(!checked_sub<std::uint32_t>(0u, 1u).has_value());
  FABOBS_CHECK_EQ(*checked_sub<std::uint32_t>(5u, 5u), 0u);
  FABOBS_CHECK(!checked_mul<std::uint64_t>(0xFFFFFFFFFFFFFFFFull, 2ull).has_value());
  FABOBS_CHECK_EQ(*checked_mul<std::uint64_t>(0ull, 0xFFFFFFFFFFFFFFFFull), 0ull);
  FABOBS_CHECK_EQ(*checked_u32(7ull), 7u);
  FABOBS_CHECK(!checked_u32(0x100000000ull).has_value());
  // checked_size is honest about the host: a 64-bit size_t can represent every
  // uint64, and the check says so instead of pretending otherwise.
  const bool wide_size_t = sizeof(std::size_t) >= 8;
  FABOBS_CHECK_MSG(
      checked_size(std::numeric_limits<std::uint64_t>::max()).has_value() == wide_size_t,
      "checked_size must reflect the width of size_t on this host");
  FABOBS_CHECK(!checked_from_i64(-1).has_value());
  std::uint32_t accumulator = 10;
  FABOBS_CHECK(checked_accumulate(accumulator, 5u, 100u));
  FABOBS_CHECK_EQ(accumulator, 15u);
  FABOBS_CHECK(!checked_accumulate(accumulator, 1000u, 100u));
  FABOBS_CHECK_EQ(accumulator, 100u);
}

FABOBS_TEST(identity, status_codes_round_trip) {
  for (std::uint16_t code = 0; code <= static_cast<std::uint16_t>(StatusCode::Internal); ++code) {
    const auto typed = static_cast<StatusCode>(code);
    const std::string_view name = to_string(typed);
    FABOBS_CHECK(name != std::string_view("Unknown"));
    const std::optional<StatusCode> parsed = status_code_from_string(name);
    FABOBS_REQUIRE(parsed.has_value());
    FABOBS_CHECK_EQ(*parsed, typed);
  }
  const Status error = Status::error(StatusCode::Corrupt, "checksum mismatch");
  FABOBS_CHECK(!error.ok());
  FABOBS_CHECK_EQ(error.code(), StatusCode::Corrupt);
  FABOBS_CHECK_EQ(error.to_string(), std::string("Corrupt: checksum mismatch"));
}

FABOBS_TEST(identity, entity_reference_text_round_trip) {
  const EntityRef ref{EntityKind::Port, entity_id_of(PortId(DeviceId::derive("device/a"), 4))};
  const Result<EntityRef> parsed = EntityRef::from_text(ref.to_text());
  FABOBS_REQUIRE(parsed.has_value());
  FABOBS_CHECK_EQ(*parsed, ref);
  FABOBS_CHECK(!EntityRef::from_text("device").has_value());
}

FABOBS_TEST(identity, identifiers_reject_hostile_text) {
  FABOBS_CHECK(util::is_valid_identifier("link.state", 64));
  FABOBS_CHECK(!util::is_valid_identifier("link state", 64));
  FABOBS_CHECK(!util::is_valid_identifier("LINK.STATE", 64));
  FABOBS_CHECK(!util::is_valid_identifier("", 64));
  FABOBS_CHECK(!util::is_valid_identifier(std::string(65, 'a'), 64));
  FABOBS_CHECK(!util::is_valid_identifier(std::string("link\0state", 10), 64));
  FABOBS_CHECK_EQ(AspectId::parse("nonsense.name").status().code(), StatusCode::Unsupported);
  FABOBS_CHECK_EQ(AspectId::parse("link.").status().code(), StatusCode::InvalidArgument);
  FABOBS_CHECK_EQ(AspectId::parse(std::string(70, 'a')).status().code(), StatusCode::TooLarge);
}
