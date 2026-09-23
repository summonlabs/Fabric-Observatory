// Unit tests: freshness and truth state evaluation.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The truth model is the single most important behaviour of the runtime, so it
// is tested directly against evaluate_truth rather than only through ingestion.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

ClaimRecord make_claim(const char* source_name, const char* value_text, FreshnessVerdict freshness,
                       SourceAuthority authority = SourceAuthority::Reported,
                       bool supported = true) {
  ClaimRecord claim;
  claim.source = source_id(source_name);
  claim.incarnation = incarnation("boot-1");
  claim.authority = authority;
  claim.sequence = SourceSequence(1);
  claim.value = *Value::text(value_text, kLimits.values);
  claim.supported = supported;
  claim.freshness = freshness;
  claim.observed_at = TimePoint{900};
  claim.received_at = TimePoint{1000};
  claim.observation_id = sha256(std::string(source_name) + value_text);
  return claim;
}

AspectId aspect(const char* name) { return *AspectId::parse(name); }

}  // namespace

FABOBS_TEST(truth, no_evidence_is_unknown_not_absent) {
  TruthPolicy policy;
  const TruthEvaluation evaluation = evaluate_truth(aspect("link.state"), {}, policy);
  FABOBS_CHECK_EQ(evaluation.truth, TruthState::Unknown);
  FABOBS_CHECK(!evaluation.agreed_value.has_value());
  FABOBS_CHECK_EQ(evaluation.conflicts.size(), std::size_t{0});
  FABOBS_CHECK(!is_positive(evaluation.truth));
}

FABOBS_TEST(truth, all_claims_unsupported_is_unsupported) {
  TruthPolicy policy;
  std::vector<ClaimRecord> claims = {make_claim("a", "", FreshnessVerdict::Fresh,
                                                SourceAuthority::Reported, false),
                                     make_claim("b", "", FreshnessVerdict::Fresh,
                                                SourceAuthority::Reported, false)};
  const TruthEvaluation evaluation = evaluate_truth(aspect("link.state"), claims, policy);
  FABOBS_CHECK_EQ(evaluation.truth, TruthState::Unsupported);
  FABOBS_CHECK(!is_positive(evaluation.truth));
}

FABOBS_TEST(truth, fresh_agreement_with_coverage_is_known) {
  TruthPolicy policy;
  std::vector<ClaimRecord> claims = {make_claim("a", "up", FreshnessVerdict::Fresh)};
  const TruthEvaluation evaluation = evaluate_truth(aspect("link.state"), claims, policy);
  FABOBS_CHECK_EQ(evaluation.truth, TruthState::Known);
  FABOBS_REQUIRE(evaluation.agreed_value.has_value());
  FABOBS_CHECK_EQ(evaluation.agreed_value->as_text(), std::string("up"));
  FABOBS_CHECK(evaluation.coverage.satisfied);
  FABOBS_CHECK(is_positive(evaluation.truth));
}

FABOBS_TEST(truth, fresh_disagreement_is_conflicting_and_both_values_survive) {
  TruthPolicy policy;
  std::vector<ClaimRecord> claims = {make_claim("a", "up", FreshnessVerdict::Fresh),
                                     make_claim("b", "down", FreshnessVerdict::Fresh)};
  const TruthEvaluation evaluation = evaluate_truth(aspect("link.state"), claims, policy);
  FABOBS_CHECK_EQ(evaluation.truth, TruthState::Conflicting);
  FABOBS_CHECK(!evaluation.agreed_value.has_value());
  FABOBS_CHECK_EQ(evaluation.conflicts.size(), std::size_t{2});
  FABOBS_CHECK(!is_positive(evaluation.truth));
}

FABOBS_TEST(truth, evidence_that_is_not_fresh_is_stale) {
  TruthPolicy policy;
  std::vector<ClaimRecord> claims = {make_claim("a", "up", FreshnessVerdict::Aging),
                                     make_claim("b", "up", FreshnessVerdict::Stale)};
  const TruthEvaluation evaluation = evaluate_truth(aspect("link.state"), claims, policy);
  FABOBS_CHECK_EQ(evaluation.truth, TruthState::Stale);
  FABOBS_CHECK(!is_positive(evaluation.truth));
}

FABOBS_TEST(truth, agreement_without_coverage_is_incomplete) {
  TruthPolicy policy;
  // reachability.state requires two distinct fresh sources at corroborated
  // authority. One fresh source agrees with itself and is still not enough.
  std::vector<ClaimRecord> claims = {make_claim("a", "reachable", FreshnessVerdict::Fresh,
                                                SourceAuthority::Corroborated)};
  const TruthEvaluation one_source = evaluate_truth(aspect("reachability.state"), claims, policy);
  FABOBS_CHECK_EQ(one_source.truth, TruthState::Incomplete);
  FABOBS_CHECK_EQ(one_source.coverage.distinct_fresh_sources, 1u);
  FABOBS_CHECK_EQ(one_source.coverage.required_distinct_fresh_sources, 2u);
  FABOBS_CHECK(!one_source.coverage.satisfied);
  FABOBS_CHECK(!one_source.agreed_value.has_value());

  // Authority alone is not enough either.
  claims = {make_claim("a", "reachable", FreshnessVerdict::Fresh, SourceAuthority::Reported),
            make_claim("b", "reachable", FreshnessVerdict::Fresh, SourceAuthority::Reported)};
  const TruthEvaluation low_authority = evaluate_truth(aspect("reachability.state"), claims, policy);
  FABOBS_CHECK_EQ(low_authority.truth, TruthState::Incomplete);
  FABOBS_CHECK_EQ(low_authority.coverage.distinct_fresh_sources, 2u);
  FABOBS_CHECK_EQ(low_authority.coverage.best_fresh_authority, SourceAuthority::Reported);

  claims = {make_claim("a", "reachable", FreshnessVerdict::Fresh, SourceAuthority::Corroborated),
            make_claim("b", "reachable", FreshnessVerdict::Fresh, SourceAuthority::Reported)};
  const TruthEvaluation satisfied = evaluate_truth(aspect("reachability.state"), claims, policy);
  FABOBS_CHECK_EQ(satisfied.truth, TruthState::Known);
  FABOBS_CHECK_EQ(satisfied.coverage.best_fresh_authority, SourceAuthority::Corroborated);
}

FABOBS_TEST(truth, superseded_evidence_cannot_assert) {
  TruthPolicy policy;
  ClaimRecord claim = make_claim("a", "up", FreshnessVerdict::Fresh);
  claim.superseded_generation = true;
  const TruthEvaluation generation = evaluate_truth(aspect("link.state"), {claim}, policy);
  FABOBS_CHECK_EQ(generation.truth, TruthState::Stale);

  ClaimRecord epoch_claim = make_claim("a", "up", FreshnessVerdict::Fresh);
  epoch_claim.superseded_epoch = true;
  FABOBS_CHECK_EQ(evaluate_truth(aspect("link.state"), {epoch_claim}, policy).truth,
                  TruthState::Stale);

  ClaimRecord incarnation_claim = make_claim("a", "up", FreshnessVerdict::Fresh);
  incarnation_claim.superseded_incarnation = true;
  FABOBS_CHECK_EQ(evaluate_truth(aspect("link.state"), {incarnation_claim}, policy).truth,
                  TruthState::Stale);

  ClaimRecord fenced_claim = make_claim("a", "up", FreshnessVerdict::Fresh);
  fenced_claim.fenced = true;
  fenced_claim.fence = StatusCode::FencedAuthority;
  FABOBS_CHECK_EQ(evaluate_truth(aspect("link.state"), {fenced_claim}, policy).truth,
                  TruthState::Stale);
}

FABOBS_TEST(truth, one_unsupported_source_does_not_block_the_others) {
  TruthPolicy policy;
  std::vector<ClaimRecord> claims = {
      make_claim("a", "", FreshnessVerdict::Fresh, SourceAuthority::Reported, false),
      make_claim("b", "up", FreshnessVerdict::Fresh)};
  FABOBS_CHECK_EQ(evaluate_truth(aspect("link.state"), claims, policy).truth, TruthState::Known);
}

FABOBS_TEST(truth, freshness_is_a_pure_function_of_the_inputs) {
  FreshnessPolicy policy;
  policy.fresh_window = Duration::from_seconds(10);
  policy.aging_window = Duration::from_seconds(20);
  const TimePoint received{1000000000};

  FABOBS_CHECK_EQ(evaluate_freshness(received, received, policy, false), FreshnessVerdict::Fresh);
  FABOBS_CHECK_EQ(evaluate_freshness(received, received + Duration::from_seconds(10), policy, false),
                  FreshnessVerdict::Fresh);
  FABOBS_CHECK_EQ(evaluate_freshness(received, received + Duration::from_seconds(11), policy, false),
                  FreshnessVerdict::Aging);
  FABOBS_CHECK_EQ(evaluate_freshness(received, received + Duration::from_seconds(20), policy, false),
                  FreshnessVerdict::Aging);
  FABOBS_CHECK_EQ(evaluate_freshness(received, received + Duration::from_seconds(21), policy, false),
                  FreshnessVerdict::Stale);
  // Evidence received after the evaluation instant is clamped rather than
  // producing a negative age.
  FABOBS_CHECK_EQ(evaluate_freshness(received + Duration::from_seconds(5), received, policy, false),
                  FreshnessVerdict::Fresh);
}

FABOBS_TEST(truth, recovered_evidence_is_never_fresh_by_default) {
  FreshnessPolicy policy;
  policy.fresh_window = Duration::from_seconds(60);
  policy.aging_window = Duration::from_seconds(120);
  FABOBS_CHECK(!policy.allow_recovered_as_fresh);
  const TimePoint received{5000};
  FABOBS_CHECK_EQ(evaluate_freshness(received, received, policy, false), FreshnessVerdict::Fresh);
  FABOBS_CHECK_EQ(evaluate_freshness(received, received, policy, true), FreshnessVerdict::Aging);
  FABOBS_CHECK_EQ(
      evaluate_freshness(received, received + Duration::from_seconds(90), policy, true),
      FreshnessVerdict::Aging);
  FABOBS_CHECK_EQ(
      evaluate_freshness(received, received + Duration::from_seconds(200), policy, true),
      FreshnessVerdict::Stale);

  policy.allow_recovered_as_fresh = true;
  FABOBS_CHECK_EQ(evaluate_freshness(received, received, policy, true), FreshnessVerdict::Fresh);
}

FABOBS_TEST(truth, coverage_policy_resolution_order) {
  Policy policy;
  policy.truth.default_coverage.min_distinct_fresh_sources = 3;
  policy.truth.default_coverage.required_authority = SourceAuthority::Inferred;

  // A dynamic aspect with no well-known descriptor falls back to the default.
  const CoveragePolicy dynamic = policy.truth.coverage_for("link.custom_metric");
  FABOBS_CHECK_EQ(dynamic.min_distinct_fresh_sources, 3u);
  FABOBS_CHECK_EQ(dynamic.required_authority, SourceAuthority::Inferred);

  // A well-known aspect uses its descriptor.
  const CoveragePolicy well_known = policy.truth.coverage_for("reachability.state");
  FABOBS_CHECK_EQ(well_known.min_distinct_fresh_sources, 2u);
  FABOBS_CHECK_EQ(well_known.required_authority, SourceAuthority::Corroborated);

  // An explicit override wins over both, and the table must stay sorted.
  CoveragePolicy override_policy;
  override_policy.min_distinct_fresh_sources = 5;
  override_policy.required_authority = SourceAuthority::Authoritative;
  policy.truth.overrides.emplace_back(aspect("link.state"), override_policy);
  const CoveragePolicy overridden = policy.truth.coverage_for("link.state");
  FABOBS_CHECK_EQ(overridden.min_distinct_fresh_sources, 5u);
  FABOBS_CHECK_EQ(overridden.required_authority, SourceAuthority::Authoritative);
  FABOBS_CHECK(policy.validate().has_value());

  policy.truth.overrides.emplace_back(aspect("link.state"), override_policy);
  FABOBS_CHECK_EQ(policy.validate().status().code(), StatusCode::InvalidArgument);
}

FABOBS_TEST(truth, policy_validation_rejects_impossible_windows) {
  Policy policy;
  policy.freshness.fresh_window = Duration::from_seconds(100);
  policy.freshness.aging_window = Duration::from_seconds(10);
  FABOBS_CHECK_EQ(policy.validate().status().code(), StatusCode::InvalidArgument);

  Policy negative;
  negative.freshness.fresh_window = Duration::from_seconds(-1);
  FABOBS_CHECK_EQ(negative.validate().status().code(), StatusCode::InvalidArgument);

  Policy bad_name;
  bad_name.name = "Not Allowed";
  FABOBS_CHECK_EQ(bad_name.validate().status().code(), StatusCode::InvalidArgument);

  Policy zero_workers;
  zero_workers.limits.max_workers = 0;
  FABOBS_CHECK_EQ(zero_workers.validate().status().code(), StatusCode::InvalidArgument);

  Policy ok;
  FABOBS_CHECK(ok.validate().has_value());
  FABOBS_CHECK_EQ(ok.digest().is_zero(), false);
}

FABOBS_TEST(truth, policy_digest_is_stable_and_sensitive) {
  Policy first;
  Policy second;
  FABOBS_CHECK_EQ(first.digest(), second.digest());
  second.freshness.fresh_window = Duration::from_seconds(31);
  FABOBS_CHECK(first.digest() != second.digest());
  // The digest is a pure function: recomputing it does not drift.
  FABOBS_CHECK_EQ(second.digest(), second.digest());
}
