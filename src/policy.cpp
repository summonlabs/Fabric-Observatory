// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/policy.hpp"

#include "fabric_observatory/canonical.hpp"
#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/util.hpp"

#include <algorithm>

namespace fabric_observatory {

CoveragePolicy TruthPolicy::coverage_for(std::string_view aspect) const {
  for (const auto& entry : overrides) {
    if (entry.first.view() == aspect) {
      return entry.second;
    }
  }
  if (const AspectDescriptor* descriptor = find_aspect_descriptor(aspect); descriptor != nullptr) {
    CoveragePolicy policy;
    policy.min_distinct_fresh_sources = descriptor->required_distinct_fresh_sources;
    policy.required_authority = descriptor->required_authority;
    return policy;
  }
  return default_coverage;
}

Result<void> TruthPolicy::validate() const {
  for (std::size_t index = 0; index < overrides.size(); ++index) {
    if (overrides[index].second.min_distinct_fresh_sources == 0) {
      return Err(StatusCode::InvalidArgument,
                 "coverage policy requires at least one fresh source");
    }
    if (index > 0 && !(overrides[index - 1].first < overrides[index].first)) {
      return Err(StatusCode::InvalidArgument,
                 "truth policy overrides must be sorted and unique by aspect");
    }
  }
  if (default_coverage.min_distinct_fresh_sources == 0) {
    return Err(StatusCode::InvalidArgument,
               "default coverage policy requires at least one fresh source");
  }
  return VoidResult{};
}

Result<void> Policy::validate() const {
  if (name.empty() || name.size() > 64) {
    return Err(StatusCode::InvalidArgument, "policy name must be 1 to 64 characters");
  }
  if (!util::is_valid_identifier(name, 64)) {
    return Err(StatusCode::InvalidArgument, "policy name is restricted to [a-z0-9._-]");
  }
  if (freshness.fresh_window.is_negative() || freshness.aging_window.is_negative()) {
    return Err(StatusCode::InvalidArgument, "freshness windows must not be negative");
  }
  if (freshness.aging_window < freshness.fresh_window) {
    return Err(StatusCode::InvalidArgument,
               "freshness aging window must not be shorter than the fresh window");
  }
  if (freshness.max_clock_skew.is_negative()) {
    return Err(StatusCode::InvalidArgument, "clock skew tolerance must not be negative");
  }
  if (limits.max_workers == 0 || limits.max_ingest_queue == 0) {
    return Err(StatusCode::InvalidArgument, "worker and queue bounds must be non-zero");
  }
  if (limits.max_history_snapshots == 0) {
    return Err(StatusCode::InvalidArgument, "history bound must be non-zero");
  }
  if (limits.max_journal_bytes == 0 || limits.max_journal_files == 0) {
    return Err(StatusCode::InvalidArgument, "journal bounds must be non-zero");
  }
  if (limits.values.max_depth == 0 || limits.values.max_total_nodes == 0) {
    return Err(StatusCode::InvalidArgument, "value bounds must be non-zero");
  }
  return truth.validate();
}

void Policy::encode(CanonicalEncoder& encoder) const {
  encoder.tag("policy");
  encoder.u8(1);
  encoder.text(name);
  encoder.u32(revision);

  encoder.i64(freshness.fresh_window.nanos);
  encoder.i64(freshness.aging_window.nanos);
  encoder.i64(freshness.max_clock_skew.nanos);
  encoder.boolean(freshness.allow_recovered_as_fresh);

  encoder.u32(truth.default_coverage.min_distinct_fresh_sources);
  encoder.u8(static_cast<std::uint8_t>(truth.default_coverage.required_authority));
  encoder.u32(static_cast<std::uint32_t>(truth.overrides.size()));
  for (const auto& entry : truth.overrides) {
    encoder.text(entry.first.view());
    encoder.u32(entry.second.min_distinct_fresh_sources);
    encoder.u8(static_cast<std::uint8_t>(entry.second.required_authority));
  }

  encoder.boolean(ingest.strict_generation_fence);
  encoder.u64(ingest.max_generation_advance);
  encoder.boolean(ingest.auto_register_sources);
  encoder.u8(static_cast<std::uint8_t>(ingest.auto_register_authority));
  encoder.boolean(ingest.reject_unknown_aspects);
  encoder.boolean(ingest.retain_rejections);

  encoder.u32(limits.max_sources);
  encoder.u32(limits.max_subjects);
  encoder.u32(limits.max_aspects_per_subject);
  encoder.u32(limits.max_claims_per_subject_aspect);
  encoder.u32(limits.max_incarnations_per_source);
  encoder.u32(limits.max_evidence_records);
  encoder.u32(limits.max_claims_per_observation);
  encoder.u32(limits.max_causal_refs);
  encoder.u64(limits.max_metadata_entries);
  encoder.u64(limits.max_metadata_key_bytes);
  encoder.u64(limits.max_metadata_value_bytes);
  encoder.u64(limits.max_observation_bytes);
  encoder.u32(limits.max_history_snapshots);
  encoder.u32(limits.max_query_results);
  encoder.u32(limits.max_hierarchy_depth);
  encoder.u32(limits.max_diff_changes);
  encoder.u32(limits.max_explanation_lines);
  encoder.u32(limits.max_rejections_retained);
  encoder.u32(limits.max_ingest_queue);
  encoder.u32(limits.max_workers);
  encoder.u32(limits.max_batch_observations);
  encoder.u64(limits.max_journal_bytes);
  encoder.u32(limits.max_journal_files);
  encoder.u64(limits.max_journal_record_bytes);
  encoder.u32(limits.max_recovered_observations);

  encoder.u64(limits.values.max_depth);
  encoder.u64(limits.values.max_list_elements);
  encoder.u64(limits.values.max_map_entries);
  encoder.u64(limits.values.max_text_bytes);
  encoder.u64(limits.values.max_blob_bytes);
  encoder.u64(limits.values.max_total_nodes);
}

Digest256 Policy::digest() const {
  CanonicalEncoder encoder(512);
  encode(encoder);
  return encoder.digest();
}

std::string Policy::canonical_text() const {
  std::string out;
  out += "policy " + name + " r" + std::to_string(revision);
  out += " fresh=" + std::to_string(freshness.fresh_window.nanos);
  out += " aging=" + std::to_string(freshness.aging_window.nanos);
  out += " skew=" + std::to_string(freshness.max_clock_skew.nanos);
  out += " recovered_as_fresh=" + std::string(freshness.allow_recovered_as_fresh ? "yes" : "no");
  out += " digest=" + digest().to_short_hex();
  return out;
}

}  // namespace fabric_observatory
