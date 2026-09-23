// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/diff.hpp"

#include "fabric_observatory/canonical.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace fabric_observatory {

namespace {

using FreshnessSignature = std::vector<std::pair<SourceId, FreshnessVerdict>>;

FreshnessSignature freshness_signature(const AspectState& aspect) {
  FreshnessSignature signature;
  signature.reserve(aspect.claims.size());
  for (const ClaimRecord& claim : aspect.claims) {
    signature.emplace_back(claim.source, claim.freshness);
  }
  std::sort(signature.begin(), signature.end());
  signature.erase(std::unique(signature.begin(), signature.end()), signature.end());
  return signature;
}

std::vector<SourceId> source_set(const AspectState& aspect) {
  std::set<SourceId> sources;
  for (const ClaimRecord& claim : aspect.claims) {
    sources.insert(claim.source);
  }
  return std::vector<SourceId>(sources.begin(), sources.end());
}

void encode_change(CanonicalEncoder& encoder, const AspectChange& change) {
  encoder.u64(change.subject.id.value().hi);
  encoder.u64(change.subject.id.value().lo);
  encoder.text(change.subject.typed_text());
  encoder.text(change.aspect.view());
  encoder.u8(static_cast<std::uint8_t>(change.before));
  encoder.u8(static_cast<std::uint8_t>(change.after));
  encoder.boolean(change.value_before.has_value());
  if (change.value_before.has_value()) {
    change.value_before->encode(encoder);
  }
  encoder.boolean(change.value_after.has_value());
  if (change.value_after.has_value()) {
    change.value_after->encode(encoder);
  }
  encoder.u64(static_cast<std::uint64_t>(change.sources_added.size()));
  for (const SourceId& source : change.sources_added) {
    encoder.u64(source.value().hi);
    encoder.u64(source.value().lo);
  }
  encoder.u64(static_cast<std::uint64_t>(change.sources_removed.size()));
  for (const SourceId& source : change.sources_removed) {
    encoder.u64(source.value().hi);
    encoder.u64(source.value().lo);
  }
  encoder.boolean(change.freshness_changed);
}

}  // namespace

std::string SnapshotDiff::canonical_summary() const {
  std::ostringstream out;
  out << "diff " << before_id.to_text() << " -> " << after_id.to_text() << "\n";
  out << "  generation " << before_generation.value() << " -> " << after_generation.value();
  if (generation_changed) {
    out << " (changed)";
  }
  out << "\n";
  out << "  epoch " << before_epoch.value() << " -> " << after_epoch.value();
  if (epoch_changed) {
    out << " (changed)";
  }
  out << "\n";
  if (restart_observed) {
    out << "  restart observed: " << restarts_between << " restart(s) between the snapshots\n";
  }
  out << "  subjects_added " << subjects_added.size() << " subjects_removed "
      << subjects_removed.size() << " aspect_changes " << changes.size() << " truncated "
      << (truncated ? "yes" : "no") << "\n";
  out << "  digest " << digest.to_hex() << "\n";
  for (const AspectChange& change : changes) {
    out << "  " << change.subject.typed_text() << " " << change.aspect.view() << " "
        << to_string(change.before) << " -> " << to_string(change.after);
    if (change.value_before.has_value() || change.value_after.has_value()) {
      out << " value ";
      out << (change.value_before.has_value() ? change.value_before->display_text()
                                              : std::string("<none>"));
      out << " -> ";
      out << (change.value_after.has_value() ? change.value_after->display_text()
                                             : std::string("<none>"));
    }
    out << "\n";
  }
  return out.str();
}

Result<SnapshotDiff> diff_snapshots(const Snapshot& before, const Snapshot& after,
                                    const Limits& limits) {
  SnapshotDiff diff;
  diff.before_id = before.id();
  diff.after_id = after.id();
  diff.before_generation = before.generation();
  diff.after_generation = after.generation();
  diff.before_epoch = before.epoch();
  diff.after_epoch = after.epoch();
  diff.before_evaluation = before.evaluation_time();
  diff.after_evaluation = after.evaluation_time();
  diff.generation_changed = before.generation() != after.generation();
  diff.epoch_changed = before.epoch() != after.epoch();
  diff.restart_observed = before.restart_epoch() != after.restart_epoch();
  if (after.restart_count() >= before.restart_count()) {
    diff.restarts_between = after.restart_count() - before.restart_count();
  }

  std::map<EntityId, const SubjectState*> before_subjects;
  for (const SubjectState& subject : before.subjects()) {
    before_subjects.emplace(subject.identity.id, &subject);
  }

  CanonicalEncoder encoder(2048);
  encoder.tag("snapshot-diff");
  encoder.u64(before.generation().value());
  encoder.u64(after.generation().value());

  for (const SubjectState& subject : after.subjects()) {
    const auto found = before_subjects.find(subject.identity.id);
    if (found == before_subjects.end()) {
      diff.subjects_added.push_back(subject.identity);
      continue;
    }
    const SubjectState& previous = *found->second;
    std::map<std::string, const AspectState*> previous_aspects;
    for (const AspectState& aspect : previous.aspects) {
      previous_aspects.emplace(aspect.aspect.value(), &aspect);
    }
    for (const AspectState& aspect : subject.aspects) {
      const auto previous_aspect = previous_aspects.find(aspect.aspect.value());
      const AspectState* old_aspect =
          previous_aspect == previous_aspects.end() ? nullptr : previous_aspect->second;
      const TruthState before_truth =
          old_aspect != nullptr ? old_aspect->truth : TruthState::Unknown;
      const std::optional<Value> before_value =
          old_aspect != nullptr ? old_aspect->agreed_value : std::nullopt;
      const FreshnessSignature before_signature =
          old_aspect != nullptr ? freshness_signature(*old_aspect) : FreshnessSignature{};
      const FreshnessSignature after_signature = freshness_signature(aspect);
      const std::vector<SourceId> before_sources =
          old_aspect != nullptr ? source_set(*old_aspect) : std::vector<SourceId>{};
      const std::vector<SourceId> after_sources = source_set(aspect);

      const bool truth_changed = before_truth != aspect.truth;
      const bool value_changed = before_value != aspect.agreed_value;
      const bool freshness_changed = before_signature != after_signature;
      if (!truth_changed && !value_changed && !freshness_changed &&
          before_sources == after_sources) {
        continue;
      }
      if (diff.changes.size() >= limits.max_diff_changes) {
        diff.truncated = true;
        break;
      }
      AspectChange change;
      change.subject = subject.identity;
      change.aspect = aspect.aspect;
      change.domain = aspect.domain;
      change.before = before_truth;
      change.after = aspect.truth;
      change.value_before = before_value;
      change.value_after = aspect.agreed_value;
      change.freshness_changed = freshness_changed;
      std::set_difference(after_sources.begin(), after_sources.end(), before_sources.begin(),
                          before_sources.end(), std::back_inserter(change.sources_added));
      std::set_difference(before_sources.begin(), before_sources.end(), after_sources.begin(),
                          after_sources.end(), std::back_inserter(change.sources_removed));
      encode_change(encoder, change);
      diff.changes.push_back(std::move(change));
    }
    if (diff.truncated) {
      break;
    }
  }

  std::map<EntityId, const SubjectState*> after_subjects;
  for (const SubjectState& subject : after.subjects()) {
    after_subjects.emplace(subject.identity.id, &subject);
  }
  for (const SubjectState& subject : before.subjects()) {
    if (after_subjects.find(subject.identity.id) == after_subjects.end()) {
      diff.subjects_removed.push_back(subject.identity);
    }
  }

  std::map<SourceId, const SourceSummary*> before_sources;
  for (const SourceSummary& source : before.sources()) {
    before_sources.emplace(source.id, &source);
  }
  std::map<SourceId, const SourceSummary*> after_sources;
  for (const SourceSummary& source : after.sources()) {
    after_sources.emplace(source.id, &source);
  }
  for (const SourceSummary& source : after.sources()) {
    SourceDelta delta;
    delta.id = source.id;
    const auto found = before_sources.find(source.id);
    if (found == before_sources.end()) {
      delta.added = true;
      delta.after_incarnation = source.incarnation.id;
      delta.after_freshness = source.freshness;
    } else {
      delta.incarnation_changed = !(found->second->incarnation.id == source.incarnation.id);
      delta.before_incarnation = found->second->incarnation.id;
      delta.after_incarnation = source.incarnation.id;
      delta.before_freshness = found->second->freshness;
      delta.after_freshness = source.freshness;
      if (!delta.incarnation_changed && delta.before_freshness == delta.after_freshness) {
        continue;
      }
    }
    diff.source_changes.push_back(delta);
  }
  for (const SourceSummary& source : before.sources()) {
    if (after_sources.find(source.id) == after_sources.end()) {
      SourceDelta delta;
      delta.id = source.id;
      delta.removed = true;
      delta.before_incarnation = source.incarnation.id;
      delta.before_freshness = source.freshness;
      diff.source_changes.push_back(delta);
    }
  }
  std::sort(diff.source_changes.begin(), diff.source_changes.end());

  encoder.u64(static_cast<std::uint64_t>(diff.changes.size()));
  encoder.u64(static_cast<std::uint64_t>(diff.subjects_added.size()));
  encoder.u64(static_cast<std::uint64_t>(diff.subjects_removed.size()));
  diff.digest = encoder.digest();
  return Ok(std::move(diff));
}

}  // namespace fabric_observatory
