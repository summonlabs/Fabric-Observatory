// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/observation.hpp"

#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/provenance.hpp"
#include "fabric_observatory/util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>

namespace fabric_observatory {

namespace {

constexpr std::array<std::string_view, kCausalStrengthCount> kCausalNames = {
    "correlates-with", "temporally-precedes", "contributory-evidence"};

constexpr std::array<std::string_view, kCausalStrengthCount> kCausalDisclaimers = {
    "recorded correlation only; no causal relationship is asserted",
    "recorded temporal ordering only; no causal relationship is asserted",
    "recorded contributing evidence only; no causal relationship is asserted"};

void encode_entity_ref(CanonicalEncoder& encoder, const SubjectIdentity& subject) {
  encoder.u8(static_cast<std::uint8_t>(subject.kind));
  encoder.u64(subject.id.value().hi);
  encoder.u64(subject.id.value().lo);
  // The typed text is carried as well as the derived identifier. On decode the
  // two must agree, which turns a truncated or rewritten record into a detected
  // corruption rather than a plausible looking one.
  encoder.text(subject.typed_text());
}

Result<SubjectIdentity> decode_entity_ref(CanonicalDecoder& decoder) {
  Result<std::uint8_t> raw_kind = decoder.u8();
  if (!raw_kind) {
    return Err<SubjectIdentity>(raw_kind.status().code(), raw_kind.status().message());
  }
  if (*raw_kind > static_cast<std::uint8_t>(EntityKind::Path)) {
    decoder.poison();
    return Err<SubjectIdentity>(StatusCode::Corrupt, "unknown entity kind in canonical input");
  }
  const auto kind = static_cast<EntityKind>(*raw_kind);
  Result<std::uint64_t> hi = decoder.u64();
  if (!hi) {
    return Err<SubjectIdentity>(hi.status().code(), hi.status().message());
  }
  Result<std::uint64_t> lo = decoder.u64();
  if (!lo) {
    return Err<SubjectIdentity>(lo.status().code(), lo.status().message());
  }
  Result<std::string_view> typed = decoder.text();
  if (!typed) {
    return Err<SubjectIdentity>(typed.status().code(), typed.status().message());
  }
  Result<SubjectIdentity> identity = SubjectIdentity::from_text(*typed);
  if (!identity) {
    return Err<SubjectIdentity>(identity.status().code(), identity.status().message());
  }
  if (identity->kind != kind || identity->id.value().hi != *hi || identity->id.value().lo != *lo) {
    decoder.poison();
    return Err<SubjectIdentity>(StatusCode::Corrupt,
                                "canonical subject identity disagrees with its typed text");
  }
  return identity;
}

Result<SubjectIdentity> decode_entity_ref(const JsonValue& json) {
  if (!json.is_text()) {
    return Err<SubjectIdentity>(StatusCode::MalformedInput,
                                "subject must be a text typed identity");
  }
  return SubjectIdentity::from_text(json.as_text());
}

std::string u64_text(std::uint64_t value) { return std::to_string(value); }

Result<std::uint64_t> parse_u64_text(const JsonValue* json, std::string_view field) {
  if (json == nullptr || !json->is_text()) {
    return Err<std::uint64_t>(StatusCode::MalformedInput,
                              std::string(field) + " must be a decimal string");
  }
  const std::string& text = json->as_text();
  std::uint64_t value = 0;
  const std::from_chars_result result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return Err<std::uint64_t>(StatusCode::MalformedInput,
                              std::string(field) + " is not a valid unsigned decimal string");
  }
  return Ok(value);
}

Result<std::int64_t> parse_i64_text(const JsonValue* json, std::string_view field) {
  if (json == nullptr || !json->is_text()) {
    return Err<std::int64_t>(StatusCode::MalformedInput,
                             std::string(field) + " must be a decimal string");
  }
  const std::string& text = json->as_text();
  std::int64_t value = 0;
  const std::from_chars_result result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return Err<std::int64_t>(StatusCode::MalformedInput,
                             std::string(field) + " is not a valid signed decimal string");
  }
  return Ok(value);
}

JsonValue encode_entity_ref_json(const SubjectIdentity& subject) {
  return JsonValue::text(subject.typed_text());
}

}  // namespace

bool SourceDescriptor::declares(std::string_view aspect) const noexcept {
  for (const AspectId& declared : aspects) {
    if (declared.view() == aspect) {
      return true;
    }
  }
  return false;
}

Result<Metadata> Metadata::make(std::vector<std::pair<std::string, std::string>> entries,
                                const Limits& limits) {
  if (entries.size() > limits.max_metadata_entries) {
    return Err<Metadata>(StatusCode::TooLarge, "metadata exceeds the configured entry count");
  }
  for (const auto& entry : entries) {
    if (entry.first.empty()) {
      return Err<Metadata>(StatusCode::InvalidArgument, "metadata keys must not be empty");
    }
    if (entry.first.size() > limits.max_metadata_key_bytes) {
      return Err<Metadata>(StatusCode::TooLarge, "metadata key exceeds the configured length");
    }
    if (!util::is_valid_identifier(entry.first, limits.max_metadata_key_bytes)) {
      return Err<Metadata>(StatusCode::InvalidArgument,
                           "metadata keys are restricted to [a-z0-9._-]");
    }
    if (entry.second.size() > limits.max_metadata_value_bytes) {
      return Err<Metadata>(StatusCode::TooLarge, "metadata value exceeds the configured length");
    }
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
  for (std::size_t index = 1; index < entries.size(); ++index) {
    if (entries[index - 1].first == entries[index].first) {
      return Err<Metadata>(StatusCode::InvalidArgument, "metadata contains a duplicate key");
    }
  }
  Metadata metadata;
  metadata.entries_ = std::move(entries);
  return Ok(std::move(metadata));
}

const std::string* Metadata::find(std::string_view key) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.first == key) {
      return &entry.second;
    }
  }
  return nullptr;
}

std::string_view to_string(CausalStrength strength) noexcept {
  const auto index = static_cast<std::size_t>(strength);
  return index < kCausalStrengthCount ? kCausalNames[index] : std::string_view("correlates-with");
}

std::optional<CausalStrength> causal_strength_from_string(std::string_view text) noexcept {
  for (std::size_t index = 0; index < kCausalStrengthCount; ++index) {
    if (kCausalNames[index] == text) {
      return static_cast<CausalStrength>(index);
    }
  }
  return std::nullopt;
}

std::string_view causality_disclaimer(CausalStrength strength) noexcept {
  const auto index = static_cast<std::size_t>(strength);
  return index < kCausalStrengthCount ? kCausalDisclaimers[index]
                                      : kCausalDisclaimers[0];
}

void Observation::canonicalize() {
  std::sort(claims.begin(), claims.end(), [](const Claim& lhs, const Claim& rhs) {
    if (lhs.subject != rhs.subject) {
      return lhs.subject < rhs.subject;
    }
    return lhs.aspect < rhs.aspect;
  });
  std::sort(causal.begin(), causal.end(), [](const CausalRef& lhs, const CausalRef& rhs) {
    if (lhs.antecedent != rhs.antecedent) {
      return lhs.antecedent < rhs.antecedent;
    }
    if (lhs.strength != rhs.strength) {
      return lhs.strength < rhs.strength;
    }
    return lhs.basis < rhs.basis;
  });
}

void Observation::encode(CanonicalEncoder& encoder) const {
  encoder.tag("observation");
  encoder.u8(1);  // encoding revision
  encoder.text(schema);
  encoder.u64(fabric.value().hi);
  encoder.u64(fabric.value().lo);
  encoder.u64(source.value().hi);
  encoder.u64(source.value().lo);
  encoder.u64(incarnation.value().hi);
  encoder.u64(incarnation.value().lo);
  encoder.u64(sequence.value());
  encoder.u64(generation.value());
  encoder.u64(epoch.value());
  encoder.i64(observed_at.nanos);

  encoder.u64(static_cast<std::uint64_t>(claims.size()));
  for (const Claim& claim : claims) {
    encode_entity_ref(encoder, claim.subject);
    encoder.text(claim.aspect.view());
    claim.value.encode(encoder);
    encoder.boolean(claim.supported);
    encoder.u64(claim.revision.value());
  }

  encoder.u64(static_cast<std::uint64_t>(causal.size()));
  for (const CausalRef& reference : causal) {
    encoder.bytes(as_bytes(reference.antecedent));
    encoder.u8(static_cast<std::uint8_t>(reference.strength));
    encoder.text(reference.basis);
  }

  encoder.u64(static_cast<std::uint64_t>(metadata.entries().size()));
  for (const auto& entry : metadata.entries()) {
    encoder.text(entry.first);
    encoder.text(entry.second);
  }
}

Result<Observation> Observation::decode(CanonicalDecoder& decoder, const Limits& limits) {
  Result<void> tag = decoder.tag("observation");
  if (!tag) {
    return Err<Observation>(tag.status().code(), tag.status().message());
  }
  Result<void> revision = decoder.expect_u8(1);
  if (!revision) {
    return Err<Observation>(revision.status().code(), revision.status().message());
  }

  Observation observation;

  Result<std::string_view> schema = decoder.text();
  if (!schema) {
    return Err<Observation>(schema.status().code(), schema.status().message());
  }
  observation.schema = std::string(*schema);

  Result<std::uint64_t> fabric_hi = decoder.u64();
  Result<std::uint64_t> fabric_lo = decoder.u64();
  Result<std::uint64_t> source_hi = decoder.u64();
  Result<std::uint64_t> source_lo = decoder.u64();
  Result<std::uint64_t> incarnation_hi = decoder.u64();
  Result<std::uint64_t> incarnation_lo = decoder.u64();
  if (!fabric_hi || !fabric_lo || !source_hi || !source_lo || !incarnation_hi || !incarnation_lo) {
    return Err<Observation>(StatusCode::Corrupt, "canonical observation identity is truncated");
  }
  observation.fabric = FabricId::from_u128(Uint128{*fabric_hi, *fabric_lo});
  observation.source = SourceId::from_u128(Uint128{*source_hi, *source_lo});
  observation.incarnation = IncarnationId::from_u128(Uint128{*incarnation_hi, *incarnation_lo});

  Result<std::uint64_t> sequence = decoder.u64();
  Result<std::uint64_t> generation = decoder.u64();
  Result<std::uint64_t> epoch = decoder.u64();
  Result<std::int64_t> observed_at = decoder.i64();
  if (!sequence || !generation || !epoch || !observed_at) {
    return Err<Observation>(StatusCode::Corrupt, "canonical observation header is truncated");
  }
  observation.sequence = SourceSequence(*sequence);
  observation.generation = GenerationId(*generation);
  observation.epoch = EpochId(*epoch);
  observation.observed_at = TimePoint{*observed_at};

  Result<std::uint64_t> claim_count = decoder.u64();
  if (!claim_count) {
    return Err<Observation>(claim_count.status().code(), claim_count.status().message());
  }
  if (*claim_count > static_cast<std::uint64_t>(limits.max_claims_per_observation)) {
    decoder.poison();
    return Err<Observation>(StatusCode::TooLarge,
                            "canonical observation carries more claims than the maximum");
  }
  observation.claims.reserve(static_cast<std::size_t>(*claim_count));
  for (std::uint64_t index = 0; index < *claim_count; ++index) {
    Claim claim;
    Result<SubjectIdentity> subject = decode_entity_ref(decoder);
    if (!subject) {
      return Err<Observation>(subject.status().code(), subject.status().message());
    }
    claim.subject = *subject;
    Result<std::string_view> aspect = decoder.text();
    if (!aspect) {
      return Err<Observation>(aspect.status().code(), aspect.status().message());
    }
    Result<AspectId> aspect_id = AspectId::parse(*aspect);
    if (!aspect_id) {
      return Err<Observation>(aspect_id.status().code(), aspect_id.status().message());
    }
    claim.aspect = *aspect_id;
    Result<Value> value = Value::decode(decoder, limits.values);
    if (!value) {
      return Err<Observation>(value.status().code(), value.status().message());
    }
    claim.value = std::move(*value);
    Result<bool> supported = decoder.boolean();
    if (!supported) {
      return Err<Observation>(supported.status().code(), supported.status().message());
    }
    claim.supported = *supported;
    Result<std::uint64_t> claim_revision = decoder.u64();
    if (!claim_revision) {
      return Err<Observation>(claim_revision.status().code(), claim_revision.status().message());
    }
    claim.revision = ClaimRevision(*claim_revision);
    observation.claims.push_back(std::move(claim));
  }

  Result<std::uint64_t> causal_count = decoder.u64();
  if (!causal_count) {
    return Err<Observation>(causal_count.status().code(), causal_count.status().message());
  }
  if (*causal_count > static_cast<std::uint64_t>(limits.max_causal_refs)) {
    decoder.poison();
    return Err<Observation>(StatusCode::TooLarge,
                            "canonical observation carries more causal references than the maximum");
  }
  observation.causal.reserve(static_cast<std::size_t>(*causal_count));
  for (std::uint64_t index = 0; index < *causal_count; ++index) {
    CausalRef reference;
    Result<ByteSpan> antecedent = decoder.bytes();
    if (!antecedent) {
      return Err<Observation>(antecedent.status().code(), antecedent.status().message());
    }
    if (antecedent->size() != reference.antecedent.bytes.size()) {
      decoder.poison();
      return Err<Observation>(StatusCode::Corrupt, "canonical causal antecedent is not 32 bytes");
    }
    std::memcpy(reference.antecedent.bytes.data(), antecedent->data(), 32);
    Result<std::uint8_t> strength = decoder.u8();
    if (!strength) {
      return Err<Observation>(strength.status().code(), strength.status().message());
    }
    if (*strength >= kCausalStrengthCount) {
      decoder.poison();
      return Err<Observation>(StatusCode::Corrupt, "canonical causal strength is out of range");
    }
    reference.strength = static_cast<CausalStrength>(*strength);
    Result<std::string_view> basis = decoder.text();
    if (!basis) {
      return Err<Observation>(basis.status().code(), basis.status().message());
    }
    reference.basis = std::string(*basis);
    observation.causal.push_back(std::move(reference));
  }

  Result<std::uint64_t> metadata_count = decoder.u64();
  if (!metadata_count) {
    return Err<Observation>(metadata_count.status().code(), metadata_count.status().message());
  }
  if (*metadata_count > static_cast<std::uint64_t>(limits.max_metadata_entries)) {
    decoder.poison();
    return Err<Observation>(StatusCode::TooLarge,
                            "canonical observation carries more metadata than the maximum");
  }
  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(static_cast<std::size_t>(*metadata_count));
  for (std::uint64_t index = 0; index < *metadata_count; ++index) {
    Result<std::string_view> key = decoder.text();
    Result<std::string_view> value = decoder.text();
    if (!key || !value) {
      return Err<Observation>(StatusCode::Corrupt, "canonical observation metadata is truncated");
    }
    entries.emplace_back(std::string(*key), std::string(*value));
  }
  Result<Metadata> metadata = Metadata::make(std::move(entries), limits);
  if (!metadata) {
    return Err<Observation>(metadata.status().code(), metadata.status().message());
  }
  observation.metadata = std::move(*metadata);

  // The observation is a nested structure: an outer container (for example a
  // journal record) may carry more fields after it, so the end of the input is
  // checked by whoever owns the container, never here.
  observation.canonicalize();
  Result<void> valid = observation.validate(limits);
  if (!valid) {
    return Err<Observation>(valid.status().code(), valid.status().message());
  }
  return Ok(std::move(observation));
}

Digest256 Observation::content_digest() const {
  CanonicalEncoder encoder(256 + claims.size() * 64);
  encode(encoder);
  return encoder.digest();
}

Result<void> Observation::validate(const Limits& limits) const {
  if (fabric.is_nil()) {
    return Err(StatusCode::InvalidArgument, "observation fabric identity must be set");
  }
  if (source.is_nil()) {
    return Err(StatusCode::InvalidArgument, "observation source identity must be set");
  }
  if (incarnation.is_nil()) {
    return Err(StatusCode::InvalidArgument, "observation source incarnation must be set");
  }
  if (sequence.is_zero()) {
    return Err(StatusCode::InvalidArgument,
               "observation sequence must be non-zero; zero is reserved for 'unset'");
  }
  if (claims.size() > limits.max_claims_per_observation) {
    return Err(StatusCode::TooLarge,
               "observation carries more claims than the configured maximum");
  }
  if (causal.size() > limits.max_causal_refs) {
    return Err(StatusCode::TooLarge,
               "observation carries more causal references than the configured maximum");
  }
  if (schema.empty()) {
    return Err(StatusCode::InvalidArgument, "observation schema must be declared");
  }
  if (metadata.entries().size() > limits.max_metadata_entries) {
    return Err(StatusCode::TooLarge, "observation carries more metadata than the configured maximum");
  }
  for (const auto& entry : metadata.entries()) {
    if (entry.first.size() > limits.max_metadata_key_bytes) {
      return Err(StatusCode::TooLarge, "metadata key exceeds the configured length");
    }
    if (entry.second.size() > limits.max_metadata_value_bytes) {
      return Err(StatusCode::TooLarge, "metadata value exceeds the configured length");
    }
  }
  for (const Claim& claim : claims) {
    if (claim.subject.id.is_nil()) {
      return Err(StatusCode::InvalidArgument, "claim subject identity must be set");
    }
    if (claim.aspect.view().empty()) {
      return Err(StatusCode::InvalidArgument, "claim aspect must be set");
    }
    // Values are bounded where they are constructed, and bounded again here
    // against the limits that are actually in force.
    Result<void> bounded = claim.value.validate(limits.values);
    if (!bounded) {
      return bounded;
    }
  }
  for (std::size_t index = 1; index < claims.size(); ++index) {
    if (claims[index - 1].subject == claims[index].subject &&
        claims[index - 1].aspect == claims[index].aspect) {
      return Err(StatusCode::InvalidArgument,
                 "observation asserts the same subject and aspect twice");
    }
  }
  for (const CausalRef& reference : causal) {
    if (reference.antecedent.is_zero()) {
      return Err(StatusCode::InvalidArgument, "causal reference antecedent must be set");
    }
  }
  return VoidResult{};
}

JsonValue Observation::to_json() const {
  std::vector<std::pair<std::string, JsonValue>> root;
  root.emplace_back("schema", JsonValue::text(schema));
  root.emplace_back("fabric", JsonValue::text(fabric.to_text()));
  root.emplace_back("source", JsonValue::text(source.to_text()));
  root.emplace_back("incarnation", JsonValue::text(incarnation.to_text()));
  root.emplace_back("sequence", JsonValue::text(u64_text(sequence.value())));
  root.emplace_back("generation", JsonValue::text(u64_text(generation.value())));
  root.emplace_back("epoch", JsonValue::text(u64_text(epoch.value())));
  root.emplace_back("observed_at", JsonValue::text(std::to_string(observed_at.nanos)));

  std::vector<JsonValue> claim_items;
  claim_items.reserve(claims.size());
  for (const Claim& claim : claims) {
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("subject", encode_entity_ref_json(claim.subject));
    members.emplace_back("aspect", JsonValue::text(claim.aspect.value()));
    members.emplace_back("value", value_to_json(claim.value));
    members.emplace_back("supported", JsonValue::boolean(claim.supported));
    members.emplace_back("revision", JsonValue::text(u64_text(claim.revision.value())));
    claim_items.push_back(JsonValue::object(std::move(members)));
  }
  root.emplace_back("claims", JsonValue::array(std::move(claim_items)));

  std::vector<JsonValue> causal_items;
  causal_items.reserve(causal.size());
  for (const CausalRef& reference : causal) {
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("antecedent", JsonValue::text(reference.antecedent.to_hex()));
    members.emplace_back("strength", JsonValue::text(std::string(to_string(reference.strength))));
    members.emplace_back("basis", JsonValue::text(reference.basis));
    causal_items.push_back(JsonValue::object(std::move(members)));
  }
  root.emplace_back("causal", JsonValue::array(std::move(causal_items)));

  std::vector<JsonValue> metadata_items;
  metadata_items.reserve(metadata.entries().size());
  for (const auto& entry : metadata.entries()) {
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("k", JsonValue::text(entry.first));
    members.emplace_back("v", JsonValue::text(entry.second));
    metadata_items.push_back(JsonValue::object(std::move(members)));
  }
  root.emplace_back("metadata", JsonValue::array(std::move(metadata_items)));
  return JsonValue::object(std::move(root));
}

std::string Observation::to_json_text(bool pretty) const { return to_json().to_text(pretty); }

Result<Observation> Observation::from_json(const JsonValue& json, const JsonLimits& json_limits,
                                           const Limits& limits) {
  (void)json_limits;
  if (!json.is_object()) {
    return Err<Observation>(StatusCode::MalformedInput, "an observation must be a JSON object");
  }

  Observation observation;

  const JsonValue* schema = json.find("schema");
  if (schema == nullptr || !schema->is_text()) {
    return Err<Observation>(StatusCode::MalformedInput, "observation requires a text 'schema'");
  }
  observation.schema = schema->as_text();

  const JsonValue* fabric = json.find("fabric");
  if (fabric == nullptr || !fabric->is_text()) {
    return Err<Observation>(StatusCode::MalformedInput, "observation requires a text 'fabric'");
  }
  Result<FabricId> fabric_id = FabricId::from_text(fabric->as_text());
  if (!fabric_id) {
    return Err<Observation>(fabric_id.status().code(), fabric_id.status().message());
  }
  observation.fabric = *fabric_id;

  const JsonValue* source = json.find("source");
  if (source == nullptr || !source->is_text()) {
    return Err<Observation>(StatusCode::MalformedInput, "observation requires a text 'source'");
  }
  Result<SourceId> source_id = SourceId::from_text(source->as_text());
  if (!source_id) {
    return Err<Observation>(source_id.status().code(), source_id.status().message());
  }
  observation.source = *source_id;

  const JsonValue* incarnation = json.find("incarnation");
  if (incarnation == nullptr || !incarnation->is_text()) {
    return Err<Observation>(StatusCode::MalformedInput,
                            "observation requires a text 'incarnation'");
  }
  Result<IncarnationId> incarnation_id = IncarnationId::from_text(incarnation->as_text());
  if (!incarnation_id) {
    return Err<Observation>(incarnation_id.status().code(), incarnation_id.status().message());
  }
  observation.incarnation = *incarnation_id;

  Result<std::uint64_t> sequence = parse_u64_text(json.find("sequence"), "sequence");
  if (!sequence) {
    return Err<Observation>(sequence.status().code(), sequence.status().message());
  }
  observation.sequence = SourceSequence(*sequence);

  Result<std::uint64_t> generation = parse_u64_text(json.find("generation"), "generation");
  if (!generation) {
    return Err<Observation>(generation.status().code(), generation.status().message());
  }
  observation.generation = GenerationId(*generation);

  const JsonValue* epoch = json.find("epoch");
  if (epoch != nullptr) {
    Result<std::uint64_t> parsed = parse_u64_text(epoch, "epoch");
    if (!parsed) {
      return Err<Observation>(parsed.status().code(), parsed.status().message());
    }
    observation.epoch = EpochId(*parsed);
  }

  Result<std::int64_t> observed_at = parse_i64_text(json.find("observed_at"), "observed_at");
  if (!observed_at) {
    return Err<Observation>(observed_at.status().code(), observed_at.status().message());
  }
  observation.observed_at = TimePoint{*observed_at};

  const JsonValue* claims = json.find("claims");
  if (claims != nullptr) {
    if (!claims->is_array()) {
      return Err<Observation>(StatusCode::MalformedInput, "'claims' must be a JSON array");
    }
    if (claims->items().size() > limits.max_claims_per_observation) {
      return Err<Observation>(StatusCode::TooLarge,
                              "observation carries more claims than the configured maximum");
    }
    observation.claims.reserve(claims->items().size());
    for (const JsonValue& item : claims->items()) {
      if (!item.is_object()) {
        return Err<Observation>(StatusCode::MalformedInput, "each claim must be a JSON object");
      }
      Claim claim;
      const JsonValue* subject_json = item.find("subject");
      if (subject_json == nullptr) {
        return Err<Observation>(StatusCode::MalformedInput, "each claim requires a 'subject'");
      }
      Result<SubjectIdentity> subject = decode_entity_ref(*subject_json);
      if (!subject) {
        return Err<Observation>(subject.status().code(), subject.status().message());
      }
      claim.subject = *subject;

      const JsonValue* aspect = item.find("aspect");
      if (aspect == nullptr || !aspect->is_text()) {
        return Err<Observation>(StatusCode::MalformedInput, "each claim requires a text 'aspect'");
      }
      Result<AspectId> aspect_id = AspectId::parse(aspect->as_text());
      if (!aspect_id) {
        return Err<Observation>(aspect_id.status().code(), aspect_id.status().message());
      }
      claim.aspect = *aspect_id;

      const JsonValue* value = item.find("value");
      if (value == nullptr) {
        return Err<Observation>(StatusCode::MalformedInput, "each claim requires a 'value'");
      }
      Result<Value> decoded = value_from_json(*value, limits.values);
      if (!decoded) {
        return Err<Observation>(decoded.status().code(), decoded.status().message());
      }
      claim.value = std::move(*decoded);

      const JsonValue* supported = item.find("supported");
      if (supported != nullptr) {
        if (!supported->is_bool()) {
          return Err<Observation>(StatusCode::MalformedInput, "'supported' must be a boolean");
        }
        claim.supported = supported->as_bool();
      }

      const JsonValue* revision = item.find("revision");
      if (revision != nullptr) {
        Result<std::uint64_t> parsed = parse_u64_text(revision, "revision");
        if (!parsed) {
          return Err<Observation>(parsed.status().code(), parsed.status().message());
        }
        claim.revision = ClaimRevision(*parsed);
      }
      observation.claims.push_back(std::move(claim));
    }
  }

  const JsonValue* causal = json.find("causal");
  if (causal != nullptr) {
    if (!causal->is_array()) {
      return Err<Observation>(StatusCode::MalformedInput, "'causal' must be a JSON array");
    }
    if (causal->items().size() > limits.max_causal_refs) {
      return Err<Observation>(StatusCode::TooLarge,
                              "observation carries more causal references than the maximum");
    }
    observation.causal.reserve(causal->items().size());
    for (const JsonValue& item : causal->items()) {
      if (!item.is_object()) {
        return Err<Observation>(StatusCode::MalformedInput,
                                "each causal reference must be a JSON object");
      }
      CausalRef reference;
      const JsonValue* antecedent = item.find("antecedent");
      if (antecedent == nullptr || !antecedent->is_text()) {
        return Err<Observation>(StatusCode::MalformedInput,
                                "causal reference requires a text 'antecedent'");
      }
      Result<Digest256> digest = Digest256::from_hex(antecedent->as_text());
      if (!digest) {
        return Err<Observation>(digest.status().code(), digest.status().message());
      }
      reference.antecedent = *digest;
      const JsonValue* strength = item.find("strength");
      if (strength != nullptr) {
        if (!strength->is_text()) {
          return Err<Observation>(StatusCode::MalformedInput,
                                  "causal strength must be text");
        }
        const std::optional<CausalStrength> parsed =
            causal_strength_from_string(strength->as_text());
        if (!parsed.has_value()) {
          return Err<Observation>(StatusCode::Unsupported, "unknown causal strength");
        }
        reference.strength = *parsed;
      }
      const JsonValue* basis = item.find("basis");
      if (basis != nullptr) {
        if (!basis->is_text()) {
          return Err<Observation>(StatusCode::MalformedInput, "causal basis must be text");
        }
        reference.basis = basis->as_text();
        if (reference.basis.size() > limits.max_metadata_value_bytes) {
          return Err<Observation>(StatusCode::TooLarge, "causal basis exceeds the maximum length");
        }
      }
      observation.causal.push_back(std::move(reference));
    }
  }

  const JsonValue* metadata = json.find("metadata");
  if (metadata != nullptr) {
    if (!metadata->is_array()) {
      return Err<Observation>(StatusCode::MalformedInput, "'metadata' must be a JSON array");
    }
    std::vector<std::pair<std::string, std::string>> entries;
    entries.reserve(metadata->items().size());
    for (const JsonValue& item : metadata->items()) {
      if (!item.is_object()) {
        return Err<Observation>(StatusCode::MalformedInput,
                                "each metadata entry must be a JSON object");
      }
      const JsonValue* key = item.find("k");
      const JsonValue* value = item.find("v");
      if (key == nullptr || !key->is_text() || value == nullptr || !value->is_text()) {
        return Err<Observation>(StatusCode::MalformedInput,
                                "metadata entries require text 'k' and 'v'");
      }
      entries.emplace_back(key->as_text(), value->as_text());
    }
    Result<Metadata> built = Metadata::make(std::move(entries), limits);
    if (!built) {
      return Err<Observation>(built.status().code(), built.status().message());
    }
    observation.metadata = std::move(*built);
  }

  observation.canonicalize();
  Result<void> valid = observation.validate(limits);
  if (!valid) {
    return Err<Observation>(valid.status().code(), valid.status().message());
  }
  return Ok(std::move(observation));
}

Result<Observation> Observation::from_json_text(std::string_view text,
                                                const JsonLimits& json_limits,
                                                const Limits& limits) {
  Result<JsonValue> parsed = parse_json(text, json_limits);
  if (!parsed) {
    return Err<Observation>(parsed.status().code(), parsed.status().message());
  }
  return from_json(*parsed, json_limits, limits);
}

}  // namespace fabric_observatory
