// Fabric Observatory command line tool.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// fabobs is the inspection and ingestion entry point for the runtime. It opens
// an observatory (recovering persisted evidence when a journal is configured),
// performs exactly one operation and writes a deterministic report.

#include "fabric_observatory/ingest.hpp"
#include "fabric_observatory/json_io.hpp"
#include "fabric_observatory/observatory.hpp"
#include "fabric_observatory/transport.hpp"
#include "fabric_observatory/util.hpp"
#include "fabric_observatory/version.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace fabric_observatory;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailure = 2;

struct Arguments {
  std::string command{};
  std::map<std::string, std::vector<std::string>> options{};
  std::vector<std::string> positional{};

  [[nodiscard]] bool has(std::string_view name) const {
    return options.find(std::string(name)) != options.end();
  }
  [[nodiscard]] std::optional<std::string> single(std::string_view name) const {
    const auto found = options.find(std::string(name));
    if (found == options.end() || found->second.empty()) {
      return std::nullopt;
    }
    return found->second.back();
  }
  [[nodiscard]] std::string single_or(std::string_view name, std::string fallback) const {
    const std::optional<std::string> value = single(name);
    return value.has_value() ? *value : std::move(fallback);
  }
  [[nodiscard]] const std::vector<std::string>& many(std::string_view name) const {
    static const std::vector<std::string> empty;
    const auto found = options.find(std::string(name));
    return found == options.end() ? empty : found->second;
  }
};

void print_usage() {
  std::printf(
      "fabobs %s - Fabric Observatory runtime tooling\n"
      "\n"
      "usage: fabobs <command> [options]\n"
      "\n"
      "commands:\n"
      "  version                       print version, schema and build identity\n"
      "  serve                         run the newline delimited JSON protocol on stdio\n"
      "  ingest                        ingest one observation file or a batch file\n"
      "  snapshot                      print the current snapshot\n"
      "  query                         query the current snapshot\n"
      "  hierarchy                     print the observed topology hierarchy\n"
      "  diff                          diff two retained snapshots\n"
      "  history                       list retained snapshots\n"
      "  explain                       explain the truth state of a subject or aspect\n"
      "  sources                       list observed sources\n"
      "  verify                        verify and report journal recovery\n"
      "  policy                        print the effective deterministic policy\n"
      "\n"
      "common options:\n"
      "  --journal PATH                journal file (enables persistence and recovery)\n"
      "  --fabric NAME|fab:HEX         fabric identity (default: derived from 'default')\n"
      "  --pretty                      pretty print JSON output\n"
      "  --no-claims                   omit per claim evidence from JSON output\n"
      "  --out PATH                    write output to a file instead of stdout\n"
      "  --evaluation-time NANOS       explicit evaluation time for freshness\n"
      "  --clock system|manual[:NANOS] clock source (manual makes a replay reproducible)\n"
      "\n"
      "ingest options:\n"
      "  --observation PATH            one observation as JSON\n"
      "  --observations PATH           newline delimited observations as JSON\n"
      "\n"
      "query options:\n"
      "  --kind KIND --domain DOMAIN --truth STATE --aspect ASPECT\n"
      "  --subtree ENTITY --limit N --max-depth N\n"
      "\n"
      "diff options:\n"
      "  --before SNAPSHOT --after SNAPSHOT   or   --lookback N\n"
      "  --batch                               emit every diff up to lookback\n"
      "\n"
      "explain options:\n"
      "  --subject TYPED_IDENTITY --aspect ASPECT\n"
      "  --all-aspects                         explain every aspect of the subject\n"
      "\n"
      "serve options:\n"
      "  --stdio                               required; the transport is stdin/stdout\n",
      std::string(version_string()).c_str());
}

Result<Arguments> parse_arguments(int argc, char** argv) {
  Arguments arguments;
  if (argc < 2) {
    return Err<Arguments>(StatusCode::InvalidArgument, "a command is required");
  }
  arguments.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    std::string token = argv[index];
    if (util::starts_with(token, "--")) {
      std::string name = token.substr(2);
      std::string value;
      const std::size_t equals = name.find('=');
      if (equals != std::string::npos) {
        value = name.substr(equals + 1);
        name = name.substr(0, equals);
      } else if (index + 1 < argc && !util::starts_with(std::string(argv[index + 1]), "--")) {
        value = argv[++index];
      }
      if (name.empty()) {
        return Err<Arguments>(StatusCode::InvalidArgument, "empty option name");
      }
      arguments.options[name].push_back(value);
    } else {
      arguments.positional.push_back(std::move(token));
    }
  }
  return Ok(std::move(arguments));
}

Result<FabricId> resolve_fabric(const Arguments& arguments) {
  const std::string name = arguments.single_or("fabric", "default");
  if (util::starts_with(name, "fab:") || name.size() == 32) {
    return FabricId::from_text(name);
  }
  if (!util::is_valid_identifier(name, 64)) {
    return Err<FabricId>(StatusCode::InvalidArgument,
                         "fabric name is restricted to [a-z0-9._-] or must be a fab:<hex> identity");
  }
  return Ok(FabricId::derive("fabric/" + name));
}

Result<std::string> read_file(const std::string& path, std::uint64_t max_bytes) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return Err<std::string>(StatusCode::IoError, "cannot open '" + path + "' for reading");
  }
  stream.seekg(0, std::ios::end);
  const std::streamoff size = stream.tellg();
  if (size < 0) {
    return Err<std::string>(StatusCode::IoError, "cannot determine the size of '" + path + "'");
  }
  if (static_cast<std::uint64_t>(size) > max_bytes) {
    return Err<std::string>(StatusCode::TooLarge,
                            "'" + path + "' is larger than the configured maximum input size");
  }
  stream.seekg(0, std::ios::beg);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return Ok(buffer.str());
}

// One bounded line per observation file line: a file without newlines must not
// be able to grow this process's memory.
enum class LineRead { Line, TooLong, End };

LineRead read_bounded_line(std::istream& in, std::string& out, std::size_t max_bytes) {
  out.clear();
  char character = 0;
  bool any = false;
  while (in.get(character)) {
    any = true;
    if (character == '\n') {
      return LineRead::Line;
    }
    if (out.size() < max_bytes) {
      out.push_back(character);
      continue;
    }
    while (in.get(character) && character != '\n') {
    }
    return LineRead::TooLong;
  }
  return any ? LineRead::Line : LineRead::End;
}

Result<std::vector<Observation>> load_observations(const std::string& path, bool line_delimited,
                                                   const Limits& limits) {
  JsonLimits json_limits;
  std::vector<Observation> observations;

  if (!line_delimited) {
    // The JSON envelope is a few times larger than the observation it carries,
    // so the file bound is derived from the observation bound rather than
    // invented.
    const std::uint64_t file_bound =
        static_cast<std::uint64_t>(limits.max_observation_bytes) * 4u + 65536u;
    Result<std::string> content = read_file(path, file_bound);
    if (!content) {
      return Err<std::vector<Observation>>(content.status().code(), content.status().message());
    }
    Result<Observation> observation = Observation::from_json_text(*content, json_limits, limits);
    if (!observation) {
      return Err<std::vector<Observation>>(observation.status().code(),
                                           observation.status().message());
    }
    observations.push_back(std::move(*observation));
    return Ok(std::move(observations));
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Err<std::vector<Observation>>(StatusCode::IoError,
                                         "cannot open '" + path + "' for reading");
  }
  std::string line;
  std::size_t line_number = 0;
  while (true) {
    const LineRead read = read_bounded_line(input, line, limits.max_observation_bytes);
    if (read == LineRead::End) {
      break;
    }
    ++line_number;
    if (read == LineRead::TooLong) {
      return Err<std::vector<Observation>>(
          StatusCode::TooLarge,
          "line " + std::to_string(line_number) + " is larger than the configured maximum");
    }
    const std::string_view trimmed = util::trim(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (observations.size() >= limits.max_batch_observations) {
      return Err<std::vector<Observation>>(
          StatusCode::TooLarge, "the input carries more observations than the configured maximum");
    }
    Result<Observation> observation = Observation::from_json_text(trimmed, json_limits, limits);
    if (!observation) {
      return Err<std::vector<Observation>>(
          observation.status().code(),
          "line " + std::to_string(line_number) + ": " + observation.status().message());
    }
    observations.push_back(std::move(*observation));
  }
  return Ok(std::move(observations));
}

void emit(const Arguments& arguments, const std::string& text) {
  const std::optional<std::string> out = arguments.single("out");
  if (!out.has_value()) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
    return;
  }
  std::ofstream stream(*out, std::ios::binary | std::ios::trunc);
  stream << text;
  std::printf("wrote %zu bytes to %s\n", text.size(), out->c_str());
}

struct OpenResult {
  std::unique_ptr<Observatory> observatory{};
  bool pretty{false};
  bool include_claims{true};
};

Result<OpenResult> open_observatory(const Arguments& arguments) {
  Result<FabricId> fabric = resolve_fabric(arguments);
  if (!fabric) {
    return Err<OpenResult>(fabric.status().code(), fabric.status().message());
  }
  ObservatoryConfig config;
  config.fabric = *fabric;
  // The clock is selectable so that a replay can be made bit for bit
  // reproducible across processes: receive times are part of the evidence, so
  // two runs only agree if they agree on when they received.
  const std::string clock_option = arguments.single_or("clock", "system");
  if (clock_option == "system") {
    config.clock = std::make_shared<SystemClock>();
  } else if (clock_option == "manual") {
    config.clock = std::make_shared<ManualClock>(TimePoint{0});
  } else if (util::starts_with(clock_option, "manual:")) {
    std::int64_t start = 0;
    try {
      start = std::stoll(clock_option.substr(7));
    } catch (const std::exception&) {
      return Err<OpenResult>(StatusCode::InvalidArgument,
                             "--clock manual:<nanoseconds> requires a decimal value");
    }
    config.clock = std::make_shared<ManualClock>(TimePoint{start});
  } else {
    return Err<OpenResult>(StatusCode::InvalidArgument,
                           "--clock must be 'system', 'manual' or 'manual:<nanoseconds>'");
  }
  config.policy.name = arguments.single_or("policy", "default");
  const std::optional<std::string> journal = arguments.single("journal");
  if (journal.has_value()) {
    config.journal_path = *journal;
  }
  Result<std::unique_ptr<Observatory>> observatory = Observatory::open(config);
  if (!observatory) {
    return Err<OpenResult>(observatory.status().code(), observatory.status().message());
  }
  OpenResult result;
  result.observatory = std::move(*observatory);
  result.pretty = arguments.has("pretty");
  result.include_claims = !arguments.has("no-claims");
  return Ok(std::move(result));
}

Result<TimePoint> evaluation_time(const Arguments& arguments, Observatory& observatory) {
  const std::optional<std::string> raw = arguments.single("evaluation-time");
  if (raw.has_value()) {
    std::int64_t value = 0;
    try {
      value = std::stoll(*raw);
    } catch (const std::exception&) {
      return Err<TimePoint>(StatusCode::InvalidArgument, "evaluation time must be an integer");
    }
    return Ok(TimePoint{value});
  }
  std::shared_ptr<const Snapshot> snapshot = observatory.current();
  if (snapshot != nullptr) {
    return Ok(snapshot->evaluation_time());
  }
  return Ok(TimePoint{0});
}

int command_version() {
  std::printf("fabobs %s\n", std::string(version_string()).c_str());
  std::printf("observation schema: %s\n", std::string(observation_schema()).c_str());
  std::printf("journal format version: %u\n", kJournalFormatVersion);
  std::printf("c++ standard: %ld\n", static_cast<long>(__cplusplus));
  std::printf("build: %s %s\n", __DATE__, __TIME__);
  return kExitOk;
}

int command_policy(const Arguments& arguments) {
  Policy policy;
  policy.name = arguments.single_or("policy", "default");
  Result<void> valid = policy.validate();
  if (!valid) {
    std::fprintf(stderr, "invalid policy: %s\n", valid.status().to_string().c_str());
    return kExitFailure;
  }
  std::printf("%s\n", policy.canonical_text().c_str());
  std::printf("digest: %s\n", policy.digest().to_hex().c_str());
  std::printf("fresh window: %lld ns\n", static_cast<long long>(policy.freshness.fresh_window.nanos));
  std::printf("aging window: %lld ns\n", static_cast<long long>(policy.freshness.aging_window.nanos));
  std::printf("max clock skew: %lld ns\n",
              static_cast<long long>(policy.freshness.max_clock_skew.nanos));
  std::printf("recovered evidence may be fresh: %s\n",
              policy.freshness.allow_recovered_as_fresh ? "yes" : "no");
  std::printf("strict generation fence: %s\n",
              policy.ingest.strict_generation_fence ? "yes" : "no");
  std::printf("default coverage: %u fresh source(s), authority >= %s\n",
              policy.truth.default_coverage.min_distinct_fresh_sources,
              std::string(to_string(policy.truth.default_coverage.required_authority)).c_str());
  for (const AspectDescriptor& descriptor : well_known_aspects()) {
    std::printf("  %-28s %-18s sources>=%u authority>=%s\n", std::string(descriptor.name).c_str(),
                std::string(to_string(descriptor.domain)).c_str(),
                descriptor.required_distinct_fresh_sources,
                std::string(to_string(descriptor.required_authority)).c_str());
  }
  return kExitOk;
}

int command_serve(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  TransportOptions options;
  options.pretty = opened->pretty;
  options.include_claims = opened->include_claims;
  TransportStats stats;
  const int code = run_stdio_server(*opened->observatory, std::cin, std::cout, options, &stats);
  opened->observatory->shutdown();
  return code;
}

int command_ingest(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  const std::optional<std::string> single = arguments.single("observation");
  const std::optional<std::string> batch = arguments.single("observations");
  if (!single.has_value() && !batch.has_value()) {
    std::fprintf(stderr, "fabobs: ingest requires --observation PATH or --observations PATH\n");
    return kExitUsage;
  }
  const std::string path = single.has_value() ? *single : *batch;
  Result<std::vector<Observation>> observations =
      load_observations(path, batch.has_value(), opened->observatory->policy().limits);
  if (!observations) {
    std::fprintf(stderr, "fabobs: %s\n", observations.status().to_string().c_str());
    return kExitFailure;
  }

  IngestBatchReport report;
  const Status status = opened->observatory->ingest_batch(*observations, report,
                                                          IngestOptions::PublishSnapshot);
  if (!status.ok()) {
    std::fprintf(stderr, "fabobs: %s\n", status.to_string().c_str());
    return kExitFailure;
  }

  std::ostringstream out;
  out << "accepted " << report.accepted << " duplicates " << report.duplicates << " fenced "
      << report.fenced << " rejected " << report.rejected << "\n";
  out << "snapshot " << report.snapshot.to_text() << "\n";
  for (const IngestOutcome& outcome : report.outcomes) {
    if (outcome.disposition == IngestDisposition::Accepted) {
      continue;
    }
    out << "  " << to_string(outcome.disposition) << " " << to_string(outcome.code) << ": "
        << outcome.explanation << "\n";
  }
  emit(arguments, out.str());
  opened->observatory->shutdown();
  return report.rejected > 0 ? kExitFailure : kExitOk;
}

int command_snapshot(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  Result<TimePoint> at = evaluation_time(arguments, *opened->observatory);
  std::shared_ptr<const Snapshot> snapshot;
  const Status status = opened->observatory->refresh(*at, snapshot);
  if (!status.ok() || snapshot == nullptr) {
    std::fprintf(stderr, "fabobs: %s\n", status.to_string().c_str());
    return kExitFailure;
  }
  emit(arguments, snapshot_to_json(*snapshot, opened->include_claims).to_text(opened->pretty) + "\n");
  opened->observatory->shutdown();
  return kExitOk;
}

int command_query(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  QueryFilter filter;
  if (const std::optional<std::string> kind = arguments.single("kind"); kind.has_value()) {
    Result<EntityKind> parsed = entity_kind_from_string(*kind);
    if (!parsed) {
      std::fprintf(stderr, "fabobs: %s\n", parsed.status().to_string().c_str());
      return kExitUsage;
    }
    filter.kind = *parsed;
  }
  for (const std::string& domain : arguments.many("domain")) {
    const std::optional<Domain> parsed = domain_from_string(domain);
    if (!parsed.has_value()) {
      std::fprintf(stderr, "fabobs: unknown domain '%s'\n", domain.c_str());
      return kExitUsage;
    }
    filter.domains.push_back(*parsed);
  }
  for (const std::string& truth : arguments.many("truth")) {
    const std::optional<TruthState> parsed = truth_state_from_string(truth);
    if (!parsed.has_value()) {
      std::fprintf(stderr, "fabobs: unknown truth state '%s'\n", truth.c_str());
      return kExitUsage;
    }
    filter.truths.push_back(*parsed);
  }
  if (const std::optional<std::string> aspect = arguments.single("aspect"); aspect.has_value()) {
    Result<AspectId> parsed = AspectId::parse(*aspect);
    if (!parsed) {
      std::fprintf(stderr, "fabobs: %s\n", parsed.status().to_string().c_str());
      return kExitUsage;
    }
    filter.aspect = *parsed;
  }
  if (const std::optional<std::string> subtree = arguments.single("subtree"); subtree.has_value()) {
    Result<EntityId> parsed = EntityId::from_text(*subtree);
    if (!parsed) {
      std::fprintf(stderr, "fabobs: %s\n", parsed.status().to_string().c_str());
      return kExitUsage;
    }
    filter.subtree_root = *parsed;
  }
  if (const std::optional<std::string> limit = arguments.single("limit"); limit.has_value()) {
    filter.limit = static_cast<std::uint32_t>(std::stoul(*limit));
  }
  if (const std::optional<std::string> depth = arguments.single("max-depth"); depth.has_value()) {
    filter.max_depth = static_cast<std::uint32_t>(std::stoul(*depth));
  }
  filter.include_claims = opened->include_claims;
  filter.include_conflicts = true;

  Result<QueryResult> result = opened->observatory->query(filter);
  if (!result) {
    std::fprintf(stderr, "fabobs: %s\n", result.status().to_string().c_str());
    return kExitFailure;
  }
  if (arguments.has("text")) {
    emit(arguments, result->canonical_summary());
  } else {
    emit(arguments,
         query_result_to_json(*result).to_text(opened->pretty) + "\n");
  }
  opened->observatory->shutdown();
  return kExitOk;
}

int command_hierarchy(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  HierarchyQuery query;
  if (const std::optional<std::string> kind = arguments.single("root-kind"); kind.has_value()) {
    Result<EntityKind> parsed = entity_kind_from_string(*kind);
    if (!parsed) {
      std::fprintf(stderr, "fabobs: %s\n", parsed.status().to_string().c_str());
      return kExitUsage;
    }
    query.root_kind = *parsed;
  }
  if (const std::optional<std::string> root = arguments.single("root"); root.has_value()) {
    Result<EntityId> parsed = EntityId::from_text(*root);
    if (!parsed) {
      std::fprintf(stderr, "fabobs: %s\n", parsed.status().to_string().c_str());
      return kExitUsage;
    }
    query.root = *parsed;
  }
  if (const std::optional<std::string> depth = arguments.single("max-depth"); depth.has_value()) {
    query.max_depth = static_cast<std::uint32_t>(std::stoul(*depth));
  }
  Result<HierarchyResult> result = opened->observatory->hierarchy(query);
  if (!result) {
    std::fprintf(stderr, "fabobs: %s\n", result.status().to_string().c_str());
    return kExitFailure;
  }
  emit(arguments, hierarchy_result_to_json(*result).to_text(opened->pretty) + "\n");
  opened->observatory->shutdown();
  return kExitOk;
}

int command_diff(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  const std::optional<std::string> before = arguments.single("before");
  const std::optional<std::string> after = arguments.single("after");
  Result<SnapshotDiff> diff =
      before.has_value() && after.has_value()
          ? [&]() -> Result<SnapshotDiff> {
              Result<SnapshotId> first = SnapshotId::from_text(*before);
              Result<SnapshotId> second = SnapshotId::from_text(*after);
              if (!first) {
                return Err<SnapshotDiff>(first.status().code(), first.status().message());
              }
              if (!second) {
                return Err<SnapshotDiff>(second.status().code(), second.status().message());
              }
              return opened->observatory->diff(*first, *second);
            }()
          : opened->observatory->diff_latest(
                static_cast<std::uint32_t>(std::stoul(arguments.single_or("lookback", "1"))));
  if (!diff) {
    std::fprintf(stderr, "fabobs: %s\n", diff.status().to_string().c_str());
    return kExitFailure;
  }
  if (arguments.has("text")) {
    emit(arguments, diff->canonical_summary());
  } else {
    emit(arguments, diff_to_json(*diff).to_text(opened->pretty) + "\n");
  }
  opened->observatory->shutdown();
  return kExitOk;
}

int command_history(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  Result<std::vector<HistoryEntry>> entries = opened->observatory->history(
      static_cast<std::uint32_t>(std::stoul(arguments.single_or("max", "16"))));
  if (!entries) {
    std::fprintf(stderr, "fabobs: %s\n", entries.status().to_string().c_str());
    return kExitFailure;
  }
  std::vector<JsonValue> items;
  items.reserve(entries->size());
  for (const HistoryEntry& entry : *entries) {
    items.push_back(history_entry_to_json(entry));
  }
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("entries", JsonValue::array(std::move(items)));
  emit(arguments, JsonValue::object(std::move(members)).to_text(opened->pretty) + "\n");
  opened->observatory->shutdown();
  return kExitOk;
}

int command_explain(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  const std::optional<std::string> subject = arguments.single("subject");
  if (!subject.has_value()) {
    std::fprintf(stderr, "fabobs: explain requires --subject TYPED_IDENTITY\n");
    return kExitUsage;
  }
  Result<SubjectIdentity> identity = SubjectIdentity::from_text(*subject);
  if (!identity) {
    std::fprintf(stderr, "fabobs: %s\n", identity.status().to_string().c_str());
    return kExitUsage;
  }
  std::optional<AspectId> aspect;
  if (const std::optional<std::string> raw = arguments.single("aspect"); raw.has_value()) {
    Result<AspectId> parsed = AspectId::parse(*raw);
    if (!parsed) {
      std::fprintf(stderr, "fabobs: %s\n", parsed.status().to_string().c_str());
      return kExitUsage;
    }
    aspect = *parsed;
  }
  Result<Explanation> explanation = opened->observatory->explain(identity->ref(), aspect);
  if (!explanation) {
    std::fprintf(stderr, "fabobs: %s\n", explanation.status().to_string().c_str());
    return kExitFailure;
  }
  if (arguments.has("text")) {
    emit(arguments, explanation->to_text());
  } else {
    emit(arguments, explanation_to_json(*explanation).to_text(opened->pretty) + "\n");
  }
  opened->observatory->shutdown();
  return kExitOk;
}

int command_sources(const Arguments& arguments) {
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  Result<std::vector<SourceSummary>> sources = opened->observatory->sources();
  if (!sources) {
    std::fprintf(stderr, "fabobs: %s\n", sources.status().to_string().c_str());
    return kExitFailure;
  }
  std::vector<JsonValue> items;
  items.reserve(sources->size());
  for (const SourceSummary& source : *sources) {
    items.push_back(source_summary_to_json(source));
  }
  std::vector<std::pair<std::string, JsonValue>> members;
  members.emplace_back("sources", JsonValue::array(std::move(items)));
  emit(arguments, JsonValue::object(std::move(members)).to_text(opened->pretty) + "\n");
  opened->observatory->shutdown();
  return kExitOk;
}

int command_verify(const Arguments& arguments) {
  const std::optional<std::string> journal = arguments.single("journal");
  if (!journal.has_value()) {
    std::fprintf(stderr, "fabobs: verify requires --journal PATH\n");
    return kExitUsage;
  }
  Result<OpenResult> opened = open_observatory(arguments);
  if (!opened) {
    std::fprintf(stderr, "fabobs: %s\n", opened.status().to_string().c_str());
    return kExitFailure;
  }
  Result<RecoveryReport> report = opened->observatory->recovery();
  if (!report) {
    std::fprintf(stderr, "fabobs: %s\n", report.status().to_string().c_str());
    return kExitFailure;
  }
  emit(arguments, report->to_text());
  const bool clean = report->format_supported && !report->corrupt && !report->truncated;
  opened->observatory->shutdown();
  return clean ? kExitOk : kExitFailure;
}

}  // namespace

int main(int argc, char** argv) {
  Result<Arguments> arguments = parse_arguments(argc, argv);
  if (!arguments) {
    print_usage();
    return kExitUsage;
  }

  const std::string& command = arguments->command;
  if (command == "version" || command == "--version" || command == "-v") {
    return command_version();
  }
  if (command == "help" || command == "--help" || command == "-h") {
    print_usage();
    return kExitOk;
  }
  if (command == "policy") {
    return command_policy(*arguments);
  }
  if (command == "serve") {
    return command_serve(*arguments);
  }
  if (command == "ingest") {
    return command_ingest(*arguments);
  }
  if (command == "snapshot") {
    return command_snapshot(*arguments);
  }
  if (command == "query") {
    return command_query(*arguments);
  }
  if (command == "hierarchy") {
    return command_hierarchy(*arguments);
  }
  if (command == "diff") {
    return command_diff(*arguments);
  }
  if (command == "history") {
    return command_history(*arguments);
  }
  if (command == "explain") {
    return command_explain(*arguments);
  }
  if (command == "sources") {
    return command_sources(*arguments);
  }
  if (command == "verify") {
    return command_verify(*arguments);
  }

  std::fprintf(stderr, "fabobs: unknown command '%s'\n", command.c_str());
  print_usage();
  return kExitUsage;
}
