// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/json_io.hpp"

#include "fabric_observatory/util.hpp"

#include <algorithm>
#include <charconv>

namespace fabric_observatory {

namespace {

JsonValue u64_json(std::uint64_t value) { return JsonValue::text(std::to_string(value)); }
JsonValue i64_json(std::int64_t value) { return JsonValue::text(std::to_string(value)); }
JsonValue bool_json(bool value) { return JsonValue::boolean(value); }
JsonValue text_json(std::string value) { return JsonValue::text(std::move(value)); }
JsonValue strings_json(const std::vector<std::string>& values) {
  std::vector<JsonValue> items;
  items.reserve(values.size());
  for (const std::string& value : values) {
    items.push_back(JsonValue::text(value));
  }
  return JsonValue::array(std::move(items));
}

JsonValue source_ids_json(const std::vector<SourceId>& sources) {
  std::vector<JsonValue> items;
  items.reserve(sources.size());
  for (const SourceId& source : sources) {
    items.push_back(JsonValue::text(source.to_text()));
  }
  return JsonValue::array(std::move(items));
}

Result<std::uint64_t> read_u64(const JsonValue& json, std::string_view member) {
  const JsonValue* value = json.find(member);
  if (value == nullptr) {
    return Err<std::uint64_t>(StatusCode::InvalidArgument,
                              std::string(member) + " is required");
  }
  if (value->is_text()) {
    std::uint64_t parsed = 0;
    const std::string& digits = value->as_text();
    const std::from_chars_result result =
        std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
      return Err<std::uint64_t>(StatusCode::MalformedInput,
                                std::string(member) + " must be a decimal string");
    }
    return Ok(parsed);
  }
  if (value->is_number()) {
    const double raw = value->as_number();
    if (raw < 0.0 || raw > 9007199254740992.0) {
      return Err<std::uint64_t>(StatusCode::OutOfRange,
                                std::string(member) + " is outside the exactly representable range");
    }
    return Ok(static_cast<std::uint64_t>(raw));
  }
  return Err<std::uint64_t>(StatusCode::MalformedInput,
                            std::string(member) + " must be a decimal string");
}

}  // namespace

JsonValue claim_record_to_json(const ClaimRecord& claim) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("source", text_json(claim.source.to_text()));
  members.emplace_back("incarnation", text_json(claim.incarnation.to_text()));
  members.emplace_back("authority", text_json(std::string(to_string(claim.authority))));
  members.emplace_back("sequence", u64_json(claim.sequence.value()));
  members.emplace_back("revision", u64_json(claim.revision.value()));
  members.emplace_back("value", value_to_json(claim.value));
  members.emplace_back("supported", bool_json(claim.supported));
  members.emplace_back("freshness", text_json(std::string(to_string(claim.freshness))));
  members.emplace_back("observed_at", i64_json(claim.observed_at.nanos));
  members.emplace_back("observed_at_utc", text_json(format_utc(claim.observed_at)));
  members.emplace_back("received_at", i64_json(claim.received_at.nanos));
  members.emplace_back("received_at_utc", text_json(format_utc(claim.received_at)));
  members.emplace_back("recovered", bool_json(claim.recovered));
  members.emplace_back("recovery_epoch", u64_json(claim.recovery_epoch.value()));
  members.emplace_back("superseded_generation", bool_json(claim.superseded_generation));
  members.emplace_back("superseded_epoch", bool_json(claim.superseded_epoch));
  members.emplace_back("superseded_incarnation", bool_json(claim.superseded_incarnation));
  members.emplace_back("superseded_by_later_sequence",
                       bool_json(claim.superseded_by_later_sequence));
  members.emplace_back("fenced", bool_json(claim.fenced));
  members.emplace_back("fence", text_json(std::string(to_string(claim.fence))));
  members.emplace_back("observation_id", text_json(claim.observation_id.to_hex()));
  return JsonValue::object(std::move(members));
}

JsonValue aspect_state_to_json(const AspectState& aspect, bool include_claims) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("aspect", text_json(aspect.aspect.value()));
  members.emplace_back("domain", text_json(std::string(to_string(aspect.domain))));
  members.emplace_back("truth", text_json(std::string(to_string(aspect.truth))));
  members.emplace_back("agreed_value",
                       aspect.agreed_value.has_value() ? value_to_json(*aspect.agreed_value)
                                                       : JsonValue::null());

  std::vector<std::pair<std::string, JsonValue>> coverage;
  coverage.emplace_back("distinct_fresh_sources", u64_json(aspect.coverage.distinct_fresh_sources));
  coverage.emplace_back("required_distinct_fresh_sources",
                        u64_json(aspect.coverage.required_distinct_fresh_sources));
  coverage.emplace_back("best_fresh_authority",
                        text_json(std::string(to_string(aspect.coverage.best_fresh_authority))));
  coverage.emplace_back("required_authority",
                        text_json(std::string(to_string(aspect.coverage.required_authority))));
  coverage.emplace_back("satisfied", bool_json(aspect.coverage.satisfied));
  members.emplace_back("coverage", JsonValue::object(std::move(coverage)));

  std::vector<JsonValue> conflicts;
  conflicts.reserve(aspect.conflicts.size());
  for (const ConflictGroup& conflict : aspect.conflicts) {
    std::vector<std::pair<std::string, JsonValue>> entry;
    entry.emplace_back("value", value_to_json(conflict.value));
    entry.emplace_back("sources", source_ids_json(conflict.sources));
    entry.emplace_back("fresh_sources", u64_json(conflict.fresh_sources));
    conflicts.push_back(JsonValue::object(std::move(entry)));
  }
  members.emplace_back("conflicts", JsonValue::array(std::move(conflicts)));
  members.emplace_back("claims_truncated", bool_json(aspect.claims_truncated));

  if (include_claims) {
    std::vector<JsonValue> claims;
    claims.reserve(aspect.claims.size());
    for (const ClaimRecord& claim : aspect.claims) {
      claims.push_back(claim_record_to_json(claim));
    }
    members.emplace_back("claims", JsonValue::array(std::move(claims)));
  }
  return JsonValue::object(std::move(members));
}

JsonValue subject_state_to_json(const SubjectState& subject, bool include_claims) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("subject", text_json(subject.identity.typed_text()));
  members.emplace_back("kind", text_json(std::string(to_string(subject.identity.kind))));
  members.emplace_back("id", text_json(subject.identity.id.to_text()));
  members.emplace_back("parent", subject.parent.has_value()
                                     ? text_json(subject.parent->to_text())
                                     : JsonValue::null());
  members.emplace_back("topology_truth", text_json(std::string(to_string(subject.topology_truth))));
  members.emplace_back("worst_truth", text_json(std::string(to_string(subject.worst_truth))));
  std::vector<JsonValue> aspects;
  aspects.reserve(subject.aspects.size());
  for (const AspectState& aspect : subject.aspects) {
    aspects.push_back(aspect_state_to_json(aspect, include_claims));
  }
  members.emplace_back("aspects", JsonValue::array(std::move(aspects)));
  return JsonValue::object(std::move(members));
}

JsonValue source_summary_to_json(const SourceSummary& source) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("source", text_json(source.id.to_text()));
  members.emplace_back("name", text_json(source.name));
  members.emplace_back("authority_name", text_json(source.authority_name));
  members.emplace_back("max_authority", text_json(std::string(to_string(source.max_authority))));
  members.emplace_back("incarnation", text_json(source.incarnation.id.to_text()));
  members.emplace_back("incarnation_boot_time", i64_json(source.incarnation.boot_time.nanos));
  members.emplace_back("incarnation_boot_counter", u64_json(source.incarnation.boot_counter));
  members.emplace_back("last_sequence", u64_json(source.last_sequence.value()));
  members.emplace_back("first_receive", i64_json(source.first_receive.nanos));
  members.emplace_back("last_receive", i64_json(source.last_receive.nanos));
  members.emplace_back("last_observation", i64_json(source.last_observation.nanos));
  members.emplace_back("last_generation", u64_json(source.last_generation.value()));
  members.emplace_back("last_epoch", u64_json(source.last_epoch.value()));
  members.emplace_back("accepted", u64_json(source.accepted));
  members.emplace_back("retained_records", u64_json(source.retained_records));
  members.emplace_back("freshness", text_json(std::string(to_string(source.freshness))));
  members.emplace_back("recovered", bool_json(source.recovered));
  return JsonValue::object(std::move(members));
}

JsonValue snapshot_stats_to_json(const SnapshotStats& stats) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("subjects", u64_json(stats.subjects));
  members.emplace_back("aspects", u64_json(stats.aspects));
  members.emplace_back("claims", u64_json(stats.claims));
  members.emplace_back("conflicts", u64_json(stats.conflicts));
  members.emplace_back("sources", u64_json(stats.sources));
  members.emplace_back("rejected_observations", u64_json(stats.rejected_observations));
  members.emplace_back("fenced_claims", u64_json(stats.fenced_claims));
  members.emplace_back("superseded_claims", u64_json(stats.superseded_claims));
  members.emplace_back("recovered_claims", u64_json(stats.recovered_claims));
  members.emplace_back("unsupported_claims", u64_json(stats.unsupported_claims));
  std::vector<std::pair<std::string, JsonValue>> by_kind;
  for (std::size_t index = 0; index < kEntityKindCount; ++index) {
    by_kind.emplace_back(std::string(to_string(static_cast<EntityKind>(index))),
                         u64_json(stats.subjects_by_kind[index]));
  }
  members.emplace_back("subjects_by_kind", JsonValue::object(std::move(by_kind)));
  std::vector<std::pair<std::string, JsonValue>> by_truth;
  for (std::size_t index = 0; index < kTruthStateCount; ++index) {
    by_truth.emplace_back(std::string(to_string(static_cast<TruthState>(index))),
                          u64_json(stats.aspects_by_truth[index]));
  }
  members.emplace_back("aspects_by_truth", JsonValue::object(std::move(by_truth)));
  return JsonValue::object(std::move(members));
}

JsonValue rejection_to_json(const RejectedObservation& rejection) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("observation_id", text_json(rejection.observation_id.to_hex()));
  members.emplace_back("source", text_json(rejection.source.to_text()));
  members.emplace_back("incarnation", text_json(rejection.incarnation.to_text()));
  members.emplace_back("sequence", u64_json(rejection.sequence.value()));
  members.emplace_back("generation", u64_json(rejection.generation.value()));
  members.emplace_back("epoch", u64_json(rejection.epoch.value()));
  members.emplace_back("received_at", i64_json(rejection.received_at.nanos));
  members.emplace_back("fence", text_json(std::string(to_string(rejection.fence))));
  members.emplace_back("explanation", text_json(rejection.explanation));
  return JsonValue::object(std::move(members));
}

JsonValue fenced_claim_to_json(const FencedClaim& claim) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("observation_id", text_json(claim.observation_id.to_hex()));
  members.emplace_back("subject", text_json(claim.subject.typed_text()));
  members.emplace_back("aspect", text_json(claim.aspect.value()));
  members.emplace_back("source", text_json(claim.source.to_text()));
  members.emplace_back("sequence", u64_json(claim.sequence.value()));
  members.emplace_back("received_at", i64_json(claim.received_at.nanos));
  members.emplace_back("fence", text_json(std::string(to_string(claim.fence))));
  members.emplace_back("explanation", text_json(claim.explanation));
  return JsonValue::object(std::move(members));
}

JsonValue snapshot_to_json(const Snapshot& snapshot, bool include_claims) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("id", text_json(snapshot.id().to_text()));
  members.emplace_back("digest", text_json(snapshot.digest().to_hex()));
  members.emplace_back("fabric", text_json(snapshot.fabric().to_text()));
  members.emplace_back("generation", u64_json(snapshot.generation().value()));
  members.emplace_back("epoch", u64_json(snapshot.epoch().value()));
  members.emplace_back("evaluation_time", i64_json(snapshot.evaluation_time().nanos));
  members.emplace_back("evaluation_time_utc", text_json(format_utc(snapshot.evaluation_time())));
  members.emplace_back("observed_high_water", i64_json(snapshot.observed_high_water().nanos));
  members.emplace_back("received_high_water", i64_json(snapshot.received_high_water().nanos));
  members.emplace_back("received_low_water", i64_json(snapshot.received_low_water().nanos));
  members.emplace_back("policy_name", text_json(snapshot.policy_name()));
  members.emplace_back("policy_digest", text_json(snapshot.policy_digest().to_hex()));
  members.emplace_back("restart_epoch", u64_json(snapshot.restart_epoch().value()));
  members.emplace_back("restart_count", u64_json(snapshot.restart_count()));
  members.emplace_back("history_index", u64_json(snapshot.history_index().value()));
  members.emplace_back("published_at", i64_json(snapshot.published_at().nanos));
  members.emplace_back("evidence_records", u64_json(snapshot.evidence_records()));
  members.emplace_back("evidence_truncated", bool_json(snapshot.evidence_truncated()));
  members.emplace_back("rejected_truncated", bool_json(snapshot.rejected_truncated()));
  members.emplace_back("stats", snapshot_stats_to_json(snapshot.stats()));

  std::vector<JsonValue> sources;
  sources.reserve(snapshot.sources().size());
  for (const SourceSummary& source : snapshot.sources()) {
    sources.push_back(source_summary_to_json(source));
  }
  members.emplace_back("sources", JsonValue::array(std::move(sources)));

  std::vector<JsonValue> subjects;
  subjects.reserve(snapshot.subjects().size());
  for (const SubjectState& subject : snapshot.subjects()) {
    subjects.push_back(subject_state_to_json(subject, include_claims));
  }
  members.emplace_back("subjects", JsonValue::array(std::move(subjects)));

  std::vector<JsonValue> rejections;
  rejections.reserve(snapshot.rejected().size());
  for (const RejectedObservation& rejection : snapshot.rejected()) {
    rejections.push_back(rejection_to_json(rejection));
  }
  members.emplace_back("rejected", JsonValue::array(std::move(rejections)));

  std::vector<JsonValue> fenced;
  fenced.reserve(snapshot.fenced_claims().size());
  for (const FencedClaim& claim : snapshot.fenced_claims()) {
    fenced.push_back(fenced_claim_to_json(claim));
  }
  members.emplace_back("fenced_claims", JsonValue::array(std::move(fenced)));
  return JsonValue::object(std::move(members));
}

JsonValue query_result_to_json(const QueryResult& result) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("snapshot", text_json(result.snapshot.to_text()));
  members.emplace_back("generation", u64_json(result.generation.value()));
  members.emplace_back("epoch", u64_json(result.epoch.value()));
  members.emplace_back("scanned_subjects", u64_json(result.scanned_subjects));
  members.emplace_back("matched_subjects", u64_json(result.matched_subjects));
  members.emplace_back("matched_aspects", u64_json(result.matched_aspects));
  members.emplace_back("truncated", bool_json(result.truncated));
  members.emplace_back("digest", text_json(result.digest.to_hex()));
  std::vector<JsonValue> subjects;
  subjects.reserve(result.subjects.size());
  for (const SubjectState& subject : result.subjects) {
    subjects.push_back(subject_state_to_json(subject, true));
  }
  members.emplace_back("subjects", JsonValue::array(std::move(subjects)));
  return JsonValue::object(std::move(members));
}

JsonValue hierarchy_result_to_json(const HierarchyResult& result) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("snapshot", text_json(result.snapshot.to_text()));
  members.emplace_back("max_depth_reached", u64_json(result.max_depth_reached));
  members.emplace_back("cycles_detected", u64_json(result.cycles_detected));
  members.emplace_back("depth_limited", u64_json(result.depth_limited));
  members.emplace_back("children_of_absent_parents", u64_json(result.children_of_absent_parents));
  members.emplace_back("truncated", bool_json(result.truncated));
  members.emplace_back("digest", text_json(result.digest.to_hex()));
  std::vector<JsonValue> nodes;
  nodes.reserve(result.nodes.size());
  for (const HierarchyNode& node : result.nodes) {
    std::vector<std::pair<std::string, JsonValue>> entry;
    entry.emplace_back("subject", text_json(node.identity.typed_text()));
    entry.emplace_back("kind", text_json(std::string(to_string(node.identity.kind))));
    entry.emplace_back("id", text_json(node.identity.id.to_text()));
    entry.emplace_back("parent", node.parent.has_value() ? text_json(node.parent->to_text())
                                                         : JsonValue::null());
    entry.emplace_back("depth", u64_json(node.depth));
    entry.emplace_back("topology_truth", text_json(std::string(to_string(node.topology_truth))));
    entry.emplace_back("parent_absent", bool_json(node.parent_absent));
    nodes.push_back(JsonValue::object(std::move(entry)));
  }
  members.emplace_back("nodes", JsonValue::array(std::move(nodes)));
  std::vector<JsonValue> unattached;
  unattached.reserve(result.unattached.size());
  for (const EntityId& id : result.unattached) {
    unattached.push_back(JsonValue::text(id.to_text()));
  }
  members.emplace_back("unattached", JsonValue::array(std::move(unattached)));
  return JsonValue::object(std::move(members));
}

JsonValue diff_to_json(const SnapshotDiff& diff) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("before", text_json(diff.before_id.to_text()));
  members.emplace_back("after", text_json(diff.after_id.to_text()));
  members.emplace_back("before_generation", u64_json(diff.before_generation.value()));
  members.emplace_back("after_generation", u64_json(diff.after_generation.value()));
  members.emplace_back("before_epoch", u64_json(diff.before_epoch.value()));
  members.emplace_back("after_epoch", u64_json(diff.after_epoch.value()));
  members.emplace_back("generation_changed", bool_json(diff.generation_changed));
  members.emplace_back("epoch_changed", bool_json(diff.epoch_changed));
  members.emplace_back("restart_observed", bool_json(diff.restart_observed));
  members.emplace_back("restarts_between", u64_json(diff.restarts_between));
  members.emplace_back("truncated", bool_json(diff.truncated));
  members.emplace_back("digest", text_json(diff.digest.to_hex()));
  std::vector<JsonValue> added;
  added.reserve(diff.subjects_added.size());
  for (const SubjectIdentity& identity : diff.subjects_added) {
    added.push_back(JsonValue::text(identity.typed_text()));
  }
  members.emplace_back("subjects_added", JsonValue::array(std::move(added)));
  std::vector<JsonValue> removed;
  removed.reserve(diff.subjects_removed.size());
  for (const SubjectIdentity& identity : diff.subjects_removed) {
    removed.push_back(JsonValue::text(identity.typed_text()));
  }
  members.emplace_back("subjects_removed", JsonValue::array(std::move(removed)));
  std::vector<JsonValue> changes;
  changes.reserve(diff.changes.size());
  for (const AspectChange& change : diff.changes) {
    std::vector<std::pair<std::string, JsonValue>> entry;
    entry.emplace_back("subject", text_json(change.subject.typed_text()));
    entry.emplace_back("aspect", text_json(change.aspect.value()));
    entry.emplace_back("domain", text_json(std::string(to_string(change.domain))));
    entry.emplace_back("before", text_json(std::string(to_string(change.before))));
    entry.emplace_back("after", text_json(std::string(to_string(change.after))));
    entry.emplace_back("value_before", change.value_before.has_value()
                                           ? value_to_json(*change.value_before)
                                           : JsonValue::null());
    entry.emplace_back("value_after", change.value_after.has_value()
                                          ? value_to_json(*change.value_after)
                                          : JsonValue::null());
    entry.emplace_back("sources_added", source_ids_json(change.sources_added));
    entry.emplace_back("sources_removed", source_ids_json(change.sources_removed));
    entry.emplace_back("freshness_changed", bool_json(change.freshness_changed));
    changes.push_back(JsonValue::object(std::move(entry)));
  }
  members.emplace_back("changes", JsonValue::array(std::move(changes)));
  std::vector<JsonValue> sources;
  sources.reserve(diff.source_changes.size());
  for (const SourceDelta& delta : diff.source_changes) {
    std::vector<std::pair<std::string, JsonValue>> entry;
    entry.emplace_back("source", text_json(delta.id.to_text()));
    entry.emplace_back("added", bool_json(delta.added));
    entry.emplace_back("removed", bool_json(delta.removed));
    entry.emplace_back("incarnation_changed", bool_json(delta.incarnation_changed));
    entry.emplace_back("before_incarnation", text_json(delta.before_incarnation.to_text()));
    entry.emplace_back("after_incarnation", text_json(delta.after_incarnation.to_text()));
    entry.emplace_back("before_freshness",
                       text_json(std::string(to_string(delta.before_freshness))));
    entry.emplace_back("after_freshness", text_json(std::string(to_string(delta.after_freshness))));
    sources.push_back(JsonValue::object(std::move(entry)));
  }
  members.emplace_back("source_changes", JsonValue::array(std::move(sources)));
  return JsonValue::object(std::move(members));
}

JsonValue history_entry_to_json(const HistoryEntry& entry) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("index", u64_json(entry.index.value()));
  members.emplace_back("id", text_json(entry.id.to_text()));
  members.emplace_back("published_at", i64_json(entry.published_at.nanos));
  members.emplace_back("published_at_utc", text_json(format_utc(entry.published_at)));
  members.emplace_back("generation", u64_json(entry.generation.value()));
  members.emplace_back("epoch", u64_json(entry.epoch.value()));
  members.emplace_back("evaluation_time", i64_json(entry.evaluation_time.nanos));
  members.emplace_back("stats", snapshot_stats_to_json(entry.stats));
  return JsonValue::object(std::move(members));
}

JsonValue explanation_to_json(const Explanation& explanation) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("snapshot", text_json(explanation.snapshot.to_text()));
  members.emplace_back("subject", text_json(explanation.subject.typed_text()));
  members.emplace_back("aspect", explanation.aspect.has_value()
                                     ? text_json(explanation.aspect->value())
                                     : JsonValue::null());
  members.emplace_back("truth", text_json(std::string(to_string(explanation.truth))));
  members.emplace_back("reasons", strings_json(explanation.reasons));
  members.emplace_back("evidence", strings_json(explanation.evidence));
  members.emplace_back("fences", strings_json(explanation.fences));
  members.emplace_back("causality", strings_json(explanation.causality));
  return JsonValue::object(std::move(members));
}

JsonValue ingest_outcome_to_json(const IngestOutcome& outcome) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("disposition", text_json(std::string(to_string(outcome.disposition))));
  members.emplace_back("code", text_json(std::string(to_string(outcome.code))));
  members.emplace_back("explanation", text_json(outcome.explanation));
  members.emplace_back("observation_id", text_json(outcome.observation_id.to_hex()));
  members.emplace_back("snapshot", text_json(outcome.snapshot.to_text()));
  members.emplace_back("claims_accepted", u64_json(outcome.claims_accepted));
  members.emplace_back("claims_fenced", u64_json(outcome.claims_fenced));
  return JsonValue::object(std::move(members));
}

JsonValue recovery_report_to_json(const RecoveryReport& report) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("opened", bool_json(report.opened));
  members.emplace_back("created", bool_json(report.created));
  members.emplace_back("format_version", u64_json(report.format_version));
  members.emplace_back("format_supported", bool_json(report.format_supported));
  members.emplace_back("file_bytes", u64_json(report.file_bytes));
  members.emplace_back("records_read", u64_json(report.records_read));
  members.emplace_back("records_accepted", u64_json(report.records_accepted));
  members.emplace_back("records_rejected", u64_json(report.records_rejected));
  members.emplace_back("truncated_tail_bytes", u64_json(report.truncated_tail_bytes));
  members.emplace_back("parts_recovered", u64_json(report.parts_recovered));
  members.emplace_back("restart_markers", u64_json(report.restart_markers));
  members.emplace_back("snapshot_markers", u64_json(report.snapshot_markers));
  members.emplace_back("chain_verified", bool_json(report.chain_verified));
  members.emplace_back("corrupt", bool_json(report.corrupt));
  members.emplace_back("truncated", bool_json(report.truncated));
  std::vector<JsonValue> diagnostics;
  diagnostics.reserve(report.diagnostics.size());
  for (const RecoveryDiagnostic& diagnostic : report.diagnostics) {
    std::vector<std::pair<std::string, JsonValue>> entry;
    entry.emplace_back("offset", u64_json(diagnostic.offset));
    entry.emplace_back("code", text_json(diagnostic.code));
    entry.emplace_back("detail", text_json(diagnostic.detail));
    diagnostics.push_back(JsonValue::object(std::move(entry)));
  }
  members.emplace_back("diagnostics", JsonValue::array(std::move(diagnostics)));
  return JsonValue::object(std::move(members));
}

JsonValue observatory_stats_to_json(const ObservatoryStats& stats) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("observations_accepted", u64_json(stats.observations_accepted));
  members.emplace_back("observations_duplicate", u64_json(stats.observations_duplicate));
  members.emplace_back("observations_fenced", u64_json(stats.observations_fenced));
  members.emplace_back("observations_rejected", u64_json(stats.observations_rejected));
  members.emplace_back("claims_fenced", u64_json(stats.claims_fenced));
  members.emplace_back("evidence_evicted", u64_json(stats.evidence_evicted));
  members.emplace_back("snapshots_published", u64_json(stats.snapshots_published));
  members.emplace_back("publications_skipped_unchanged",
                       u64_json(stats.publications_skipped_unchanged));
  members.emplace_back("journal_records", u64_json(stats.journal_records));
  members.emplace_back("journal_rotations", u64_json(stats.journal_rotations));
  members.emplace_back("sources", u64_json(stats.sources));
  members.emplace_back("evidence_records", u64_json(stats.evidence_records));
  members.emplace_back("restart_count", u64_json(stats.restart_count));
  members.emplace_back("published", bool_json(stats.published));
  members.emplace_back("dirty", bool_json(stats.dirty));
  return JsonValue::object(std::move(members));
}

Result<QueryFilter> query_filter_from_json(const JsonValue& json) {
  QueryFilter filter;
  if (json.is_null()) {
    return Ok(filter);
  }
  if (!json.is_object()) {
    return Err<QueryFilter>(StatusCode::InvalidArgument, "filter must be a JSON object");
  }
  if (const JsonValue* kind = json.find("kind"); kind != nullptr) {
    if (!kind->is_text()) {
      return Err<QueryFilter>(StatusCode::InvalidArgument, "filter kind must be text");
    }
    Result<EntityKind> parsed = entity_kind_from_string(kind->as_text());
    if (!parsed) {
      return Err<QueryFilter>(parsed.status().code(), parsed.status().message());
    }
    filter.kind = *parsed;
  }
  if (const JsonValue* domains = json.find("domains"); domains != nullptr) {
    if (!domains->is_array()) {
      return Err<QueryFilter>(StatusCode::InvalidArgument, "filter domains must be an array");
    }
    for (const JsonValue& item : domains->items()) {
      if (!item.is_text()) {
        return Err<QueryFilter>(StatusCode::InvalidArgument, "domain names must be text");
      }
      const std::optional<Domain> parsed = domain_from_string(item.as_text());
      if (!parsed.has_value()) {
        return Err<QueryFilter>(StatusCode::Unsupported, "unknown domain in filter");
      }
      filter.domains.push_back(*parsed);
    }
    std::sort(filter.domains.begin(), filter.domains.end());
  }
  if (const JsonValue* truths = json.find("truths"); truths != nullptr) {
    if (!truths->is_array()) {
      return Err<QueryFilter>(StatusCode::InvalidArgument, "filter truths must be an array");
    }
    for (const JsonValue& item : truths->items()) {
      if (!item.is_text()) {
        return Err<QueryFilter>(StatusCode::InvalidArgument, "truth names must be text");
      }
      const std::optional<TruthState> parsed = truth_state_from_string(item.as_text());
      if (!parsed.has_value()) {
        return Err<QueryFilter>(StatusCode::Unsupported, "unknown truth state in filter");
      }
      filter.truths.push_back(*parsed);
    }
    std::sort(filter.truths.begin(), filter.truths.end());
  }
  if (const JsonValue* aspect = json.find("aspect"); aspect != nullptr && !aspect->is_null()) {
    if (!aspect->is_text()) {
      return Err<QueryFilter>(StatusCode::InvalidArgument, "filter aspect must be text");
    }
    Result<AspectId> parsed = AspectId::parse(aspect->as_text());
    if (!parsed) {
      return Err<QueryFilter>(parsed.status().code(), parsed.status().message());
    }
    filter.aspect = *parsed;
  }
  if (const JsonValue* root = json.find("subtree_root"); root != nullptr && !root->is_null()) {
    if (!root->is_text()) {
      return Err<QueryFilter>(StatusCode::InvalidArgument, "subtree_root must be text");
    }
    Result<EntityId> parsed = EntityId::from_text(root->as_text());
    if (!parsed) {
      return Err<QueryFilter>(parsed.status().code(), parsed.status().message());
    }
    filter.subtree_root = *parsed;
  }
  if (json.find("max_depth") != nullptr) {
    Result<std::uint64_t> value = read_u64(json, "max_depth");
    if (!value) {
      return Err<QueryFilter>(value.status().code(), value.status().message());
    }
    if (*value > 0xFFFFFFFFull) {
      return Err<QueryFilter>(StatusCode::OutOfRange, "max_depth is out of range");
    }
    filter.max_depth = static_cast<std::uint32_t>(*value);
  }
  if (json.find("limit") != nullptr) {
    Result<std::uint64_t> value = read_u64(json, "limit");
    if (!value) {
      return Err<QueryFilter>(value.status().code(), value.status().message());
    }
    if (*value > 0xFFFFFFFFull) {
      return Err<QueryFilter>(StatusCode::OutOfRange, "limit is out of range");
    }
    filter.limit = static_cast<std::uint32_t>(*value);
  }
  if (const JsonValue* include_claims = json.find("include_claims"); include_claims != nullptr) {
    if (!include_claims->is_bool()) {
      return Err<QueryFilter>(StatusCode::InvalidArgument, "include_claims must be a boolean");
    }
    filter.include_claims = include_claims->as_bool();
  }
  if (const JsonValue* include_conflicts = json.find("include_conflicts");
      include_conflicts != nullptr) {
    if (!include_conflicts->is_bool()) {
      return Err<QueryFilter>(StatusCode::InvalidArgument, "include_conflicts must be a boolean");
    }
    filter.include_conflicts = include_conflicts->as_bool();
  }
  return Ok(std::move(filter));
}

Result<HierarchyQuery> hierarchy_query_from_json(const JsonValue& json) {
  HierarchyQuery query;
  if (json.is_null()) {
    return Ok(query);
  }
  if (!json.is_object()) {
    return Err<HierarchyQuery>(StatusCode::InvalidArgument, "hierarchy query must be an object");
  }
  if (const JsonValue* kind = json.find("root_kind"); kind != nullptr && !kind->is_null()) {
    if (!kind->is_text()) {
      return Err<HierarchyQuery>(StatusCode::InvalidArgument, "root_kind must be text");
    }
    Result<EntityKind> parsed = entity_kind_from_string(kind->as_text());
    if (!parsed) {
      return Err<HierarchyQuery>(parsed.status().code(), parsed.status().message());
    }
    query.root_kind = *parsed;
  }
  if (const JsonValue* root = json.find("root"); root != nullptr && !root->is_null()) {
    if (!root->is_text()) {
      return Err<HierarchyQuery>(StatusCode::InvalidArgument, "root must be text");
    }
    Result<EntityId> parsed = EntityId::from_text(root->as_text());
    if (!parsed) {
      return Err<HierarchyQuery>(parsed.status().code(), parsed.status().message());
    }
    query.root = *parsed;
  }
  if (json.find("max_depth") != nullptr) {
    Result<std::uint64_t> value = read_u64(json, "max_depth");
    if (!value) {
      return Err<HierarchyQuery>(value.status().code(), value.status().message());
    }
    query.max_depth = *value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(*value);
  }
  if (json.find("limit") != nullptr) {
    Result<std::uint64_t> value = read_u64(json, "limit");
    if (!value) {
      return Err<HierarchyQuery>(value.status().code(), value.status().message());
    }
    query.limit = *value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<std::uint32_t>(*value);
  }
  if (const JsonValue* include_unattached = json.find("include_unattached");
      include_unattached != nullptr) {
    if (!include_unattached->is_bool()) {
      return Err<HierarchyQuery>(StatusCode::InvalidArgument,
                                 "include_unattached must be a boolean");
    }
    query.include_unattached = include_unattached->as_bool();
  }
  return Ok(std::move(query));
}

Result<AspectId> optional_aspect_from_json(const JsonValue& json, std::string_view member) {
  const JsonValue* value = json.find(member);
  if (value == nullptr || value->is_null()) {
    return Err<AspectId>(StatusCode::NotFound, "aspect member is absent");
  }
  if (!value->is_text()) {
    return Err<AspectId>(StatusCode::InvalidArgument, "aspect must be text");
  }
  return AspectId::parse(value->as_text());
}

}  // namespace fabric_observatory
