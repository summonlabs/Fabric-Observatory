// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_JSON_IO_HPP
#define FABRIC_OBSERVATORY_JSON_IO_HPP

#include "fabric_observatory/diff.hpp"
#include "fabric_observatory/explanation.hpp"
#include "fabric_observatory/history.hpp"
#include "fabric_observatory/json.hpp"
#include "fabric_observatory/observatory.hpp"
#include "fabric_observatory/query.hpp"
#include "fabric_observatory/snapshot.hpp"

// Serialisation used by the command line tool and by the stdio transport. Every
// member order is fixed by this file, so the same snapshot always serialises to
// the same bytes: the output is diffable and testable.
//
// Integer rule: 64-bit integers are emitted as decimal strings, because JSON
// numbers are IEEE-754 doubles and a nanosecond timestamp does not round-trip
// through one.

namespace fabric_observatory {

JsonValue claim_record_to_json(const ClaimRecord& claim);
JsonValue aspect_state_to_json(const AspectState& aspect, bool include_claims);
JsonValue subject_state_to_json(const SubjectState& subject, bool include_claims);
JsonValue source_summary_to_json(const SourceSummary& source);
JsonValue snapshot_stats_to_json(const SnapshotStats& stats);
JsonValue rejection_to_json(const RejectedObservation& rejection);
JsonValue fenced_claim_to_json(const FencedClaim& claim);
JsonValue snapshot_to_json(const Snapshot& snapshot, bool include_claims);

JsonValue query_result_to_json(const QueryResult& result);
JsonValue hierarchy_result_to_json(const HierarchyResult& result);
JsonValue diff_to_json(const SnapshotDiff& diff);
JsonValue history_entry_to_json(const HistoryEntry& entry);
JsonValue explanation_to_json(const Explanation& explanation);
JsonValue ingest_outcome_to_json(const IngestOutcome& outcome);
JsonValue recovery_report_to_json(const RecoveryReport& report);
JsonValue observatory_stats_to_json(const ObservatoryStats& stats);

Result<QueryFilter> query_filter_from_json(const JsonValue& json);
Result<HierarchyQuery> hierarchy_query_from_json(const JsonValue& json);
Result<AspectId> optional_aspect_from_json(const JsonValue& json, std::string_view member);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_JSON_IO_HPP
