// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/explanation.hpp"

#include "fabric_observatory/canonical.hpp"
#include "fabric_observatory/util.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace fabric_observatory {

std::string truth_state_reason(const AspectState& aspect) {
  const std::string name = aspect.aspect.value();
  switch (aspect.truth) {
    case TruthState::Unknown:
      return "unknown: no evidence for aspect '" + name +
             "' has been accepted; absence of evidence is not positive evidence";
    case TruthState::Unsupported: {
      std::uint32_t unsupported = 0;
      for (const ClaimRecord& claim : aspect.claims) {
        if (!claim.supported) {
          ++unsupported;
        }
      }
      return "unsupported: " + std::to_string(unsupported) + " of " +
             std::to_string(aspect.claims.size()) +
             " retained claim(s) for aspect '" + name +
             "' declare that the reporting source cannot supply it";
    }
    case TruthState::Conflicting: {
      std::string reason = "conflicting: " + std::to_string(aspect.conflicts.size()) +
                           " distinct values are asserted by fresh evidence for aspect '" + name +
                           "' and this runtime does not choose between them";
      for (const ConflictGroup& conflict : aspect.conflicts) {
        reason += "; ";
        reason += conflict.value.display_text();
        reason += " from " + std::to_string(conflict.fresh_sources) + " fresh source(s)";
      }
      return reason;
    }
    case TruthState::Stale: {
      std::uint32_t current = 0;
      for (const ClaimRecord& claim : aspect.claims) {
        if (claim.is_current() && claim.supported) {
          ++current;
        }
      }
      return "stale: " + std::to_string(current) +
             " current claim(s) for aspect '" + name +
             "' exist but none is fresh at the snapshot evaluation time";
    }
    case TruthState::Incomplete:
      return "incomplete: fresh evidence for aspect '" + name + "' agrees but coverage is " +
             std::to_string(aspect.coverage.distinct_fresh_sources) + "/" +
             std::to_string(aspect.coverage.required_distinct_fresh_sources) +
             " distinct fresh source(s) with best authority " +
             std::string(to_string(aspect.coverage.best_fresh_authority)) + " (requires " +
             std::string(to_string(aspect.coverage.required_authority)) + ")";
    case TruthState::Known:
      return "known: " + std::to_string(aspect.coverage.distinct_fresh_sources) +
             " fresh source(s) agree on aspect '" + name + "'" +
             (aspect.agreed_value.has_value() ? " with value " + aspect.agreed_value->display_text()
                                              : std::string());
  }
  return "unknown: unrecognised truth state";
}

std::string causality_line(const CausalRef& reference, const Digest256& subject_observation) {
  std::ostringstream out;
  out << "observation " << subject_observation.to_short_hex() << " "
      << to_string(reference.strength) << " observation "
      << reference.antecedent.to_short_hex();
  if (!reference.basis.empty()) {
    out << " (basis: " << reference.basis << ")";
  }
  out << "; " << causality_disclaimer(reference.strength);
  return out.str();
}

namespace {

std::string claim_line(const ClaimRecord& claim) {
  std::ostringstream out;
  out << "source " << claim.source.to_text() << " incarnation "
      << claim.incarnation.to_text().substr(0, 16) << " sequence " << claim.sequence.value()
      << " authority " << to_string(claim.authority) << " freshness " << to_string(claim.freshness)
      << " observed " << format_utc(claim.observed_at) << " received "
      << format_utc(claim.received_at);
  out << " value " << claim.value.display_text();
  if (!claim.supported) {
    out << " [declared unsupported]";
  }
  if (claim.superseded_generation) {
    out << " [superseded by a later generation]";
  }
  if (claim.superseded_epoch) {
    out << " [superseded by a later epoch of the same generation]";
  }
  if (claim.superseded_incarnation) {
    out << " [superseded by a later incarnation of the same source]";
  }
  if (claim.superseded_by_later_sequence) {
    out << " [superseded by a later sequence from the same source incarnation]";
  }
  if (claim.recovered) {
    out << " [recovered from persistence at restart " << claim.recovery_epoch.value() << "]";
  }
  return out.str();
}

}  // namespace

Result<Explanation> explain_subject(const Snapshot& snapshot, const SubjectIdentity& subject,
                                    std::optional<AspectId> aspect, const Limits& limits) {
  Explanation explanation;
  explanation.snapshot = snapshot.id();
  explanation.subject = subject;
  explanation.aspect = aspect;

  const SubjectState* state = snapshot.find_subject(subject.ref());
  if (state == nullptr) {
    explanation.truth = TruthState::Unknown;
    explanation.reasons.push_back(
        "unknown: subject '" + subject.typed_text() +
        "' has no accepted evidence in this snapshot; absence of evidence is not positive "
        "evidence");
    return Ok(std::move(explanation));
  }

  explanation.subject = state->identity;
  std::vector<const AspectState*> selected;
  for (const AspectState& candidate : state->aspects) {
    if (aspect.has_value() && !(candidate.aspect == *aspect)) {
      continue;
    }
    selected.push_back(&candidate);
  }
  if (selected.empty()) {
    explanation.truth = TruthState::Unknown;
    explanation.reasons.push_back("unknown: no evidence has been accepted for the requested "
                                  "aspect of subject '" +
                                  state->identity.typed_text() + "'");
    return Ok(std::move(explanation));
  }

  TruthState overall = TruthState::Known;
  for (const AspectState* candidate : selected) {
    if (static_cast<std::uint8_t>(candidate->truth) < static_cast<std::uint8_t>(overall)) {
      overall = candidate->truth;
    }
    if (explanation.reasons.size() < limits.max_explanation_lines) {
      explanation.reasons.push_back(candidate->aspect.value() + ": " +
                                    truth_state_reason(*candidate));
    }
    for (const ClaimRecord& claim : candidate->claims) {
      if (explanation.evidence.size() >= limits.max_explanation_lines) {
        break;
      }
      explanation.evidence.push_back(candidate->aspect.value() + ": " + claim_line(claim));
    }
  }
  explanation.truth = overall;

  // A rejection has no subject of its own: it is attributed to the subject only
  // when the rejecting source also makes statements about that subject, which is
  // the strongest attribution the evidence model supports.
  std::set<SourceId> reporting_sources;
  for (const AspectState* candidate : selected) {
    for (const ClaimRecord& claim : candidate->claims) {
      reporting_sources.insert(claim.source);
    }
  }
  for (const RejectedObservation& rejection : snapshot.rejected()) {
    if (explanation.fences.size() >= limits.max_explanation_lines) {
      break;
    }
    if (reporting_sources.find(rejection.source) == reporting_sources.end()) {
      continue;
    }
    std::ostringstream out;
    out << "rejected observation " << rejection.observation_id.to_short_hex() << " from source "
        << rejection.source.to_text() << " sequence " << rejection.sequence.value() << ": "
        << to_string(rejection.fence) << " (" << rejection.explanation << ")";
    explanation.fences.push_back(out.str());
  }
  for (const FencedClaim& fenced : snapshot.fenced_claims()) {
    if (explanation.fences.size() >= limits.max_explanation_lines) {
      break;
    }
    if (!(fenced.subject.id == state->identity.id)) {
      continue;
    }
    if (aspect.has_value() && !(fenced.aspect == *aspect)) {
      continue;
    }
    std::ostringstream out;
    out << "fenced claim for " << fenced.subject.typed_text() << " " << fenced.aspect.value()
        << " from source " << fenced.source.to_text() << " sequence " << fenced.sequence.value()
        << ": " << to_string(fenced.fence) << " (" << fenced.explanation << ")";
    explanation.fences.push_back(out.str());
  }

  return Ok(std::move(explanation));
}

Result<Explanation> explain_absent(const Snapshot& snapshot, const EntityRef& subject,
                                   std::optional<AspectId> aspect) {
  Explanation explanation;
  explanation.snapshot = snapshot.id();
  explanation.subject.kind = subject.kind;
  explanation.subject.id = subject.id;
  explanation.aspect = aspect;
  explanation.truth = TruthState::Unknown;
  explanation.reasons.push_back("unknown: subject " + subject.to_text() +
                                " is not present in this snapshot; absence of evidence is not "
                                "positive evidence");
  return Ok(std::move(explanation));
}

std::string Explanation::to_text() const {
  std::ostringstream out;
  out << "explanation snapshot " << snapshot.to_text() << "\n";
  out << "  subject " << subject.typed_text() << "\n";
  out << "  aspect " << (aspect.has_value() ? aspect->value() : std::string("<all>")) << "\n";
  out << "  truth " << to_string(truth) << "\n";
  for (const std::string& reason : reasons) {
    out << "  reason: " << reason << "\n";
  }
  for (const std::string& line : evidence) {
    out << "  evidence: " << line << "\n";
  }
  for (const std::string& line : fences) {
    out << "  fence: " << line << "\n";
  }
  for (const std::string& line : causality) {
    out << "  causality: " << line << "\n";
  }
  return out.str();
}

}  // namespace fabric_observatory
