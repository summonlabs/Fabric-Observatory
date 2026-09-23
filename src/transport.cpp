// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/transport.hpp"

#include "fabric_observatory/json_io.hpp"
#include "fabric_observatory/lock_audit.hpp"
#include "fabric_observatory/util.hpp"
#include "fabric_observatory/version.hpp"

#include <algorithm>
#include <charconv>
#include <istream>
#include <ostream>

namespace fabric_observatory {

namespace {

enum class LineResult { Line, TooLong, End };

// Reads one line without ever buffering more than max_bytes. A client that
// never sends a newline must not be able to grow this process's memory.
LineResult read_bounded_line(std::istream& in, std::string& out, std::size_t max_bytes) {
  out.clear();
  char character = 0;
  bool any = false;
  while (in.get(character)) {
    any = true;
    if (character == '\n') {
      return LineResult::Line;
    }
    if (out.size() < max_bytes) {
      out.push_back(character);
      continue;
    }
    // Discard the remainder of the oversized line so the stream stays framed.
    while (in.get(character) && character != '\n') {
    }
    return LineResult::TooLong;
  }
  return any ? LineResult::Line : LineResult::End;
}

JsonValue error_response(std::string_view op, StatusCode code, std::string message) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("ok", JsonValue::boolean(false));
  members.emplace_back("op", JsonValue::text(std::string(op)));
  members.emplace_back("code", JsonValue::text(std::string(to_string(code))));
  members.emplace_back("message", JsonValue::text(std::move(message)));
  return JsonValue::object(std::move(members));
}

JsonValue ok_response(std::string_view op, JsonValue result) {
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("ok", JsonValue::boolean(true));
  members.emplace_back("op", JsonValue::text(std::string(op)));
  members.emplace_back("result", std::move(result));
  return JsonValue::object(std::move(members));
}

std::string generic_error(std::string_view op, std::string_view what) {
  return error_response(op, StatusCode::MalformedInput, std::string(what))
      .to_text(false);
}

Result<std::uint64_t> read_count(const JsonValue& json, std::string_view member,
                                 std::uint64_t fallback) {
  const JsonValue* value = json.find(member);
  if (value == nullptr || value->is_null()) {
    return Ok(fallback);
  }
  if (!value->is_text()) {
    return Err<std::uint64_t>(StatusCode::InvalidArgument,
                              std::string(member) + " must be a decimal string");
  }
  std::uint64_t parsed = 0;
  const std::string& digits = value->as_text();
  const std::from_chars_result result =
      std::from_chars(digits.data(), digits.data() + digits.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) {
    return Err<std::uint64_t>(StatusCode::MalformedInput,
                              std::string(member) + " is not a valid decimal string");
  }
  return Ok(parsed);
}

}  // namespace

std::string handle_request_line(Observatory& observatory, std::string_view line,
                                const TransportOptions& options) {
  const std::string_view trimmed = util::trim(line);
  if (trimmed.size() > options.max_line_bytes) {
    return error_response("-", StatusCode::TooLarge, "request line exceeds the configured maximum")
        .to_text(options.pretty);
  }
  if (trimmed.empty()) {
    return error_response("-", StatusCode::MalformedInput, "empty request line")
        .to_text(options.pretty);
  }

  Result<JsonValue> parsed = parse_json(trimmed, options.json);
  if (!parsed) {
    return error_response("-", parsed.status().code(), parsed.status().message())
        .to_text(options.pretty);
  }
  if (!parsed->is_object()) {
    return error_response("-", StatusCode::MalformedInput, "a request must be a JSON object")
        .to_text(options.pretty);
  }

  const JsonValue* op_value = parsed->find("op");
  if (op_value == nullptr || !op_value->is_text()) {
    return error_response("-", StatusCode::MalformedInput, "a request requires a text 'op' member")
        .to_text(options.pretty);
  }
  const std::string& op = op_value->as_text();

  if (op == "ping") {
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("version", JsonValue::text(std::string(version_string())));
    members.emplace_back("schema", JsonValue::text(std::string(observation_schema())));
    members.emplace_back("fabric", JsonValue::text(observatory.fabric().to_text()));
    const LockAuditReport audit = lock_audit_report();
    members.emplace_back("lock_audit", JsonValue::text(audit.to_text()));
    return ok_response(op, JsonValue::object(std::move(members))).to_text(options.pretty);
  }

  if (op == "ingest" || op == "ingest_batch") {
    const bool batch = op == "ingest_batch";
    const JsonValue* payload = parsed->find(batch ? "observations" : "observation");
    if (payload == nullptr) {
      return generic_error(op, batch ? "'observations' is required" : "'observation' is required");
    }
    if (batch) {
      if (!payload->is_array()) {
        return generic_error(op, "'observations' must be an array");
      }
      std::vector<Observation> observations;
      observations.reserve(payload->items().size());
      for (const JsonValue& item : payload->items()) {
        Result<Observation> observation =
            Observation::from_json(item, options.json, observatory.policy().limits);
        if (!observation) {
          return generic_error(op, observation.status().message());
        }
        observations.push_back(std::move(*observation));
      }
      IngestBatchReport report;
      const Status status = observatory.ingest_batch(observations, report,
                                                     IngestOptions::PublishSnapshot);
      if (!status.ok()) {
        return error_response(op, status.code(), status.message()).to_text(options.pretty);
      }
      std::vector<std::pair<std::string, JsonValue>> members;
      members.emplace_back("accepted", JsonValue::text(std::to_string(report.accepted)));
      members.emplace_back("duplicates", JsonValue::text(std::to_string(report.duplicates)));
      members.emplace_back("fenced", JsonValue::text(std::to_string(report.fenced)));
      members.emplace_back("rejected", JsonValue::text(std::to_string(report.rejected)));
      members.emplace_back("snapshot", JsonValue::text(report.snapshot.to_text()));
      std::vector<JsonValue> outcomes;
      outcomes.reserve(report.outcomes.size());
      for (const IngestOutcome& outcome : report.outcomes) {
        outcomes.push_back(ingest_outcome_to_json(outcome));
      }
      members.emplace_back("outcomes", JsonValue::array(std::move(outcomes)));
      return ok_response(op, JsonValue::object(std::move(members))).to_text(options.pretty);
    }
    Result<Observation> observation =
        Observation::from_json(*payload, options.json, observatory.policy().limits);
    if (!observation) {
      return generic_error(op, observation.status().message());
    }
    IngestOutcome outcome;
    const Status status = observatory.ingest(*observation, outcome, IngestOptions::PublishSnapshot);
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("outcome", ingest_outcome_to_json(outcome));
    members.emplace_back("status", JsonValue::text(status.to_string()));
    // A fenced or rejected observation is a normal, reported outcome rather than
    // a protocol failure: the response is still an ok response carrying the
    // disposition, which is what makes disagreement visible to the caller.
    return ok_response(op, JsonValue::object(std::move(members))).to_text(options.pretty);
  }

  if (op == "snapshot") {
    std::shared_ptr<const Snapshot> snapshot = observatory.current();
    if (snapshot == nullptr) {
      return error_response(op, StatusCode::NotOpen, "no snapshot has been published")
          .to_text(options.pretty);
    }
    return ok_response(op, snapshot_to_json(*snapshot, options.include_claims))
        .to_text(options.pretty);
  }

  if (op == "refresh") {
    Result<std::uint64_t> at = read_count(*parsed, "at", 0);
    if (!at) {
      return error_response(op, at.status().code(), at.status().message()).to_text(options.pretty);
    }
    std::shared_ptr<const Snapshot> snapshot;
    const Status status = observatory.refresh(TimePoint{static_cast<std::int64_t>(*at)}, snapshot);
    if (!status.ok()) {
      return error_response(op, status.code(), status.message()).to_text(options.pretty);
    }
    return ok_response(op, snapshot_to_json(*snapshot, options.include_claims))
        .to_text(options.pretty);
  }

  if (op == "query") {
    const JsonValue* filter_json = parsed->find("filter");
    Result<QueryFilter> filter =
        query_filter_from_json(filter_json == nullptr ? JsonValue::null() : *filter_json);
    if (!filter) {
      return error_response(op, filter.status().code(), filter.status().message())
          .to_text(options.pretty);
    }
    Result<QueryResult> result = observatory.query(*filter);
    if (!result) {
      return error_response(op, result.status().code(), result.status().message())
          .to_text(options.pretty);
    }
    return ok_response(op, query_result_to_json(*result)).to_text(options.pretty);
  }

  if (op == "hierarchy") {
    const JsonValue* query_json = parsed->find("query");
    Result<HierarchyQuery> query =
        hierarchy_query_from_json(query_json == nullptr ? JsonValue::null() : *query_json);
    if (!query) {
      return error_response(op, query.status().code(), query.status().message())
          .to_text(options.pretty);
    }
    Result<HierarchyResult> result = observatory.hierarchy(*query);
    if (!result) {
      return error_response(op, result.status().code(), result.status().message())
          .to_text(options.pretty);
    }
    return ok_response(op, hierarchy_result_to_json(*result)).to_text(options.pretty);
  }

  if (op == "diff") {
    const JsonValue* before = parsed->find("before");
    const JsonValue* after = parsed->find("after");
    if (before != nullptr && after != nullptr && before->is_text() && after->is_text()) {
      Result<SnapshotId> first = SnapshotId::from_text(before->as_text());
      Result<SnapshotId> second = SnapshotId::from_text(after->as_text());
      if (!first || !second) {
        return generic_error(op, "snapshot identifiers must be snap:<64 hex characters>");
      }
      Result<SnapshotDiff> diff = observatory.diff(*first, *second);
      if (!diff) {
        return error_response(op, diff.status().code(), diff.status().message())
            .to_text(options.pretty);
      }
      return ok_response(op, diff_to_json(*diff)).to_text(options.pretty);
    }
    Result<std::uint64_t> lookback = read_count(*parsed, "lookback", 1);
    if (!lookback) {
      return error_response(op, lookback.status().code(), lookback.status().message())
          .to_text(options.pretty);
    }
    if (*lookback == 0 || *lookback > 0xFFFFFFFFull) {
      return generic_error(op, "lookback must be between 1 and 4294967295");
    }
    Result<SnapshotDiff> diff = observatory.diff_latest(static_cast<std::uint32_t>(*lookback));
    if (!diff) {
      return error_response(op, diff.status().code(), diff.status().message())
          .to_text(options.pretty);
    }
    return ok_response(op, diff_to_json(*diff)).to_text(options.pretty);
  }

  if (op == "history") {
    Result<std::uint64_t> max_entries = read_count(*parsed, "max", 16);
    if (!max_entries) {
      return error_response(op, max_entries.status().code(), max_entries.status().message())
          .to_text(options.pretty);
    }
    if (*max_entries > 0xFFFFFFFFull) {
      return generic_error(op, "max is out of range");
    }
    Result<std::vector<HistoryEntry>> entries =
        observatory.history(static_cast<std::uint32_t>(*max_entries));
    if (!entries) {
      return error_response(op, entries.status().code(), entries.status().message())
          .to_text(options.pretty);
    }
    std::vector<JsonValue> items;
    items.reserve(entries->size());
    for (const HistoryEntry& entry : *entries) {
      items.push_back(history_entry_to_json(entry));
    }
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("entries", JsonValue::array(std::move(items)));
    return ok_response(op, JsonValue::object(std::move(members))).to_text(options.pretty);
  }

  if (op == "explain") {
    const JsonValue* subject = parsed->find("subject");
    if (subject == nullptr || !subject->is_text()) {
      return generic_error(op, "'subject' must be a typed identity string");
    }
    Result<SubjectIdentity> identity = SubjectIdentity::from_text(subject->as_text());
    if (!identity) {
      return generic_error(op, identity.status().message());
    }
    std::optional<AspectId> aspect;
    if (const JsonValue* aspect_json = parsed->find("aspect");
        aspect_json != nullptr && !aspect_json->is_null()) {
      if (!aspect_json->is_text()) {
        return generic_error(op, "'aspect' must be text");
      }
      Result<AspectId> parsed_aspect = AspectId::parse(aspect_json->as_text());
      if (!parsed_aspect) {
        return generic_error(op, parsed_aspect.status().message());
      }
      aspect = *parsed_aspect;
    }
    Result<Explanation> explanation = observatory.explain(identity->ref(), aspect);
    if (!explanation) {
      return error_response(op, explanation.status().code(), explanation.status().message())
          .to_text(options.pretty);
    }
    return ok_response(op, explanation_to_json(*explanation)).to_text(options.pretty);
  }

  if (op == "sources") {
    Result<std::vector<SourceSummary>> sources = observatory.sources();
    if (!sources) {
      return error_response(op, sources.status().code(), sources.status().message())
          .to_text(options.pretty);
    }
    std::vector<JsonValue> items;
    items.reserve(sources->size());
    for (const SourceSummary& source : *sources) {
      items.push_back(source_summary_to_json(source));
    }
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("sources", JsonValue::array(std::move(items)));
    return ok_response(op, JsonValue::object(std::move(members))).to_text(options.pretty);
  }

  if (op == "stats") {
    return ok_response(op, observatory_stats_to_json(observatory.stats()))
        .to_text(options.pretty);
  }

  if (op == "recovery") {
    Result<RecoveryReport> report = observatory.recovery();
    if (!report) {
      return error_response(op, report.status().code(), report.status().message())
          .to_text(options.pretty);
    }
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("report", recovery_report_to_json(*report));
    members.emplace_back("text", JsonValue::text(report->to_text()));
    return ok_response(op, JsonValue::object(std::move(members))).to_text(options.pretty);
  }

  if (op == "summary") {
    return ok_response(op, JsonValue::text(observatory.canonical_summary())).to_text(options.pretty);
  }

  if (op == "policy") {
    const Policy& policy = observatory.policy();
    std::vector<std::pair<std::string, JsonValue>> members;
    members.emplace_back("name", JsonValue::text(policy.name));
    members.emplace_back("revision", JsonValue::text(std::to_string(policy.revision)));
    members.emplace_back("digest", JsonValue::text(policy.digest().to_hex()));
    members.emplace_back("canonical", JsonValue::text(policy.canonical_text()));
    members.emplace_back("strict_generation_fence",
                         JsonValue::boolean(policy.ingest.strict_generation_fence));
    members.emplace_back("allow_recovered_as_fresh",
                         JsonValue::boolean(policy.freshness.allow_recovered_as_fresh));
    return ok_response(op, JsonValue::object(std::move(members))).to_text(options.pretty);
  }

  if (op == "shutdown") {
    return ok_response(op, JsonValue::boolean(true)).to_text(options.pretty);
  }

  return error_response(op, StatusCode::Unsupported, "unknown operation: " + op)
      .to_text(options.pretty);
}

int run_stdio_server(Observatory& observatory, std::istream& in, std::ostream& out,
                     const TransportOptions& options, TransportStats* stats) {
  std::string line;
  while (true) {
    const LineResult read = read_bounded_line(in, line, options.max_line_bytes);
    if (read == LineResult::End) {
      break;
    }
    bool shutdown_requested = false;
    if (read == LineResult::TooLong) {
      if (stats != nullptr) {
        ++stats->oversized;
      }
      out << error_response("-", StatusCode::TooLarge,
                            "request line exceeds the configured maximum")
                 .to_text(options.pretty)
          << "\n";
      out.flush();
      continue;
    }

    std::string response;
    {
      // The response is produced from the request line only; nothing else is
      // read until it has been fully written, so responses cannot interleave.
      Result<JsonValue> parsed = parse_json(util::trim(line), options.json);
      if (parsed && parsed->is_object()) {
        const JsonValue* op = parsed->find("op");
        shutdown_requested = op != nullptr && op->is_text() && op->as_text() == "shutdown";
      }
    }
    response = handle_request_line(observatory, line, options);
    out << response << "\n";
    out.flush();
    if (stats != nullptr) {
      ++stats->requests;
      if (response.find("\"ok\":false") != std::string::npos) {
        ++stats->failures;
      }
    }
    if (shutdown_requested) {
      if (stats != nullptr) {
        stats->shutdown_requested = true;
      }
      return 0;
    }
  }
  return 0;
}

}  // namespace fabric_observatory
