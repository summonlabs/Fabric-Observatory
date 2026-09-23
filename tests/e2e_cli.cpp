// End to end tests: the fabobs tool as an operator uses it.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "process_runner.hpp"
#include "runtime_fixture.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

#ifdef FABOBS_TEST_BINARY
namespace {

const SubjectIdentity kDeviceA = SubjectIdentity::of(DeviceId::derive("device/cli-a"));
const SubjectIdentity kDeviceB = SubjectIdentity::of(DeviceId::derive("device/cli-b"));

std::string observation_line(const char* device_typed, const char* aspect, const char* value,
                             std::uint64_t sequence) {
  return "{\"schema\":\"fabric-observatory/observation/1\","
         "\"fabric\":\"" + FabricId::derive("fabric/cli").to_text() + "\","
         "\"source\":\"" + source_id("cli").to_text() + "\","
         "\"incarnation\":\"" + incarnation("cli/boot-1").to_text() + "\","
         "\"sequence\":\"" + std::to_string(sequence) + "\",\"generation\":\"1\",\"epoch\":\"1\","
         "\"observed_at\":\"" + std::to_string(kFixtureTime) + "\","
         "\"claims\":[{\"subject\":\"" + device_typed +
         "\",\"aspect\":\"" + aspect + "\",\"value\":{\"k\":\"text\",\"v\":\"" + value + "\"}}]}";
}

class ObservationFile {
 public:
  explicit ObservationFile(const std::string& name) : path_("fabobs-test-" + name + ".ndjson") {
    std::remove(path_.c_str());
  }
  ~ObservationFile() { std::remove(path_.c_str()); }
  ObservationFile(const ObservationFile&) = delete;
  ObservationFile& operator=(const ObservationFile&) = delete;

  void write(const std::vector<std::string>& lines) {
    std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
    for (const std::string& line : lines) {
      stream << line << "\n";
    }
  }

  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_;
};

std::string cli(const std::string& arguments) {
  return quote(FABOBS_TEST_BINARY) + " " + arguments;
}

}  // namespace

FABOBS_TEST(cli, version_reports_the_runtime_identity) {
  const ProcessResult result = run_process(cli("version"));
  FABOBS_REQUIRE(result.started);
  FABOBS_CHECK_EQ(result.exit_code, 0);
  FABOBS_CHECK(result.output.find("fabobs 1.0.0") != std::string::npos);
  FABOBS_CHECK(result.output.find("fabric-observatory/observation/1") != std::string::npos);
  FABOBS_CHECK(result.output.find("journal format version: 1") != std::string::npos);
}

FABOBS_TEST(cli, policy_is_printable_and_deterministic) {
  const ProcessResult first = run_process(cli("policy"));
  const ProcessResult second = run_process(cli("policy"));
  FABOBS_REQUIRE(first.started);
  FABOBS_CHECK_EQ(first.exit_code, 0);
  FABOBS_CHECK_EQ(first.output, second.output);
  FABOBS_CHECK(first.output.find("recovered evidence may be fresh: no") != std::string::npos);
  FABOBS_CHECK(first.output.find("reachability.state") != std::string::npos);
}

FABOBS_TEST(cli, ingest_snapshot_query_diff_history_explain) {
  TempFile journal("cli-flow");
  ObservationFile observations("cli-flow");
  observations.write({
      observation_line(kDeviceA.typed_text().c_str(), "link.state", "up", 1),
      observation_line(kDeviceB.typed_text().c_str(), "operational.health", "ok", 2),
  });

  const std::string journal_argument = " --journal " + quote(journal.path()) + " --fabric cli";

  const ProcessResult ingest =
      run_process(cli("ingest --observations " + quote(observations.path()) + journal_argument));
  FABOBS_REQUIRE(ingest.started);
  FABOBS_CHECK_EQ(ingest.exit_code, 0);
  FABOBS_CHECK(ingest.output.find("accepted 2") != std::string::npos);
  FABOBS_CHECK(ingest.output.find("snapshot snap:") != std::string::npos);

  // Every command runs in its own process, so every command is a restart: the
  // evidence is recovered from the journal and is never presented as fresh.
  // That is the conservative restart guarantee, observed end to end.
  const ProcessResult snapshot =
      run_process(cli("snapshot" + journal_argument + " --evaluation-time " +
                      std::to_string(kFixtureTime)));
  FABOBS_REQUIRE(snapshot.started);
  FABOBS_CHECK_EQ(snapshot.exit_code, 0);
  FABOBS_CHECK(snapshot.output.find("\"generation\":\"1\"") != std::string::npos);
  FABOBS_CHECK(snapshot.output.find("\"restart_count\":\"1\"") != std::string::npos);
  FABOBS_CHECK(snapshot.output.find("\"recovered_claims\":\"2\"") != std::string::npos);
  FABOBS_CHECK(snapshot.output.find("\"truth\":\"stale\"") != std::string::npos);
  FABOBS_CHECK(snapshot.output.find("\"freshness\":\"aging\"") != std::string::npos);

  const ProcessResult query = run_process(cli("query" + journal_argument + " --kind device"));
  FABOBS_REQUIRE(query.started);
  FABOBS_CHECK_EQ(query.exit_code, 0);
  FABOBS_CHECK(query.output.find("\"matched_subjects\":\"2\"") != std::string::npos);

  const ProcessResult filtered =
      run_process(cli("query" + journal_argument + " --truth stale --domain link"));
  FABOBS_CHECK(filtered.output.find("\"matched_aspects\":\"1\"") != std::string::npos);
  const ProcessResult not_known =
      run_process(cli("query" + journal_argument + " --truth known"));
  FABOBS_CHECK(not_known.output.find("\"matched_aspects\":\"0\"") != std::string::npos);

  const ProcessResult history = run_process(cli("history" + journal_argument + " --max 5"));
  FABOBS_REQUIRE(history.started);
  FABOBS_CHECK_EQ(history.exit_code, 0);
  FABOBS_CHECK(history.output.find("\"entries\"") != std::string::npos);
  // Snapshot history is per process: this run has published only its opening
  // view, and the journal records that earlier runs published snapshots too.
  FABOBS_CHECK(history.output.find("\"index\":\"1\"") != std::string::npos);

  const ProcessResult hierarchy = run_process(cli("hierarchy" + journal_argument));
  FABOBS_REQUIRE(hierarchy.started);
  FABOBS_CHECK_EQ(hierarchy.exit_code, 0);
  FABOBS_CHECK(hierarchy.output.find("\"nodes\"") != std::string::npos);

  const ProcessResult sources = run_process(cli("sources" + journal_argument));
  FABOBS_REQUIRE(sources.started);
  FABOBS_CHECK_EQ(sources.exit_code, 0);
  FABOBS_CHECK(sources.output.find("\"retained_records\":\"2\"") != std::string::npos);

  const ProcessResult explain = run_process(cli("explain" + journal_argument + " --subject " +
                                                quote(kDeviceA.typed_text()) +
                                                " --aspect link.state --text"));
  FABOBS_REQUIRE(explain.started);
  FABOBS_CHECK_EQ(explain.exit_code, 0);
  FABOBS_CHECK(explain.output.find("truth stale") != std::string::npos);
  FABOBS_CHECK(explain.output.find("evidence:") != std::string::npos);
  FABOBS_CHECK(explain.output.find("recovered from persistence") != std::string::npos);

  const ProcessResult verify = run_process(cli("verify" + journal_argument));
  FABOBS_REQUIRE(verify.started);
  FABOBS_CHECK_EQ(verify.exit_code, 0);
  FABOBS_CHECK(verify.output.find("chain_verified=yes") != std::string::npos);
  FABOBS_CHECK(verify.output.find("records_accepted=2") != std::string::npos);
}

FABOBS_TEST(cli, diff_explains_that_history_is_per_process) {
  TempFile journal("cli-diff");
  ObservationFile first("cli-diff-a");
  ObservationFile second("cli-diff-b");
  first.write({observation_line(kDeviceA.typed_text().c_str(), "link.state", "up", 1)});
  second.write({observation_line(kDeviceA.typed_text().c_str(), "link.state", "down", 2)});

  const std::string journal_argument = " --journal " + quote(journal.path()) + " --fabric cli";
  FABOBS_REQUIRE(run_process(cli("ingest --observation " + quote(first.path()) + journal_argument))
                     .exit_code == 0);
  FABOBS_REQUIRE(run_process(cli("ingest --observation " + quote(second.path()) + journal_argument))
                     .exit_code == 0);

  // A snapshot cannot be rebuilt after a restart, because recovered evidence is
  // never presented as fresh. The tool says exactly that instead of failing
  // silently or inventing a diff.
  const ProcessResult diff = run_process(cli("diff" + journal_argument + " --lookback 1 --text"));
  FABOBS_REQUIRE(diff.started);
  FABOBS_CHECK_EQ(diff.exit_code, 2);
  FABOBS_CHECK(diff.output.find("history is per process") != std::string::npos);

  const ProcessResult by_id =
      run_process(cli("diff" + journal_argument + " --before snap:" + std::string(64, '0') +
                      " --after snap:" + std::string(64, '0')));
  FABOBS_CHECK_EQ(by_id.exit_code, 2);
  FABOBS_CHECK(by_id.output.find("history is per process") != std::string::npos);
}

FABOBS_TEST(cli, diff_within_one_process_reports_the_transition) {
  TempFile journal("cli-diff-live");
  ObservationFile batch("cli-diff-live");
  batch.write({
      observation_line(kDeviceA.typed_text().c_str(), "link.state", "up", 1),
  });

  // A single process that ingests, publishes and then diffs has both snapshots
  // in its own bounded history.
  const ProcessResult result = run_process(
      cli("serve --stdio --journal " + quote(journal.path()) + " --fabric cli --clock manual:" +
          std::to_string(kFixtureTime)),
      "{\"op\":\"ingest\",\"observation\":" +
          observation_line(kDeviceA.typed_text().c_str(), "link.state", "up", 1) + "}\n" +
          "{\"op\":\"ingest\",\"observation\":" +
          observation_line(kDeviceA.typed_text().c_str(), "link.state", "down", 2) + "}\n" +
          "{\"op\":\"diff\",\"lookback\":\"1\"}\n" + "{\"op\":\"shutdown\"}\n");
  FABOBS_REQUIRE(result.started);
  FABOBS_CHECK_EQ(result.exit_code, 0);
  FABOBS_CHECK(result.output.find("\"op\":\"diff\"") != std::string::npos);
  FABOBS_CHECK(result.output.find("\"ok\":true") != std::string::npos);
  FABOBS_CHECK(result.output.find("\"value_after\"") != std::string::npos);
}

FABOBS_TEST(cli, a_rejected_observation_has_a_non_zero_exit_and_a_reason) {
  TempFile journal("cli-reject");
  ObservationFile observations("cli-reject");
  observations.write({observation_line(kDeviceA.typed_text().c_str(), "nonsense.aspect", "up", 1)});

  const ProcessResult result = run_process(cli("ingest --observations " +
                                               quote(observations.path()) + " --journal " +
                                               quote(journal.path()) + " --fabric cli"));
  FABOBS_REQUIRE(result.started);
  FABOBS_CHECK(result.exit_code != 0);
  FABOBS_CHECK(result.output.find("Unsupported") != std::string::npos);
}

FABOBS_TEST(cli, unknown_command_and_missing_options_are_usage_errors) {
  const ProcessResult unknown = run_process(cli("frobnicate"));
  FABOBS_REQUIRE(unknown.started);
  FABOBS_CHECK_EQ(unknown.exit_code, 1);
  FABOBS_CHECK(unknown.output.find("unknown command") != std::string::npos);
  FABOBS_CHECK(unknown.output.find("usage: fabobs") != std::string::npos);

  const ProcessResult missing = run_process(cli("ingest"));
  FABOBS_CHECK_EQ(missing.exit_code, 1);
  FABOBS_CHECK(missing.output.find("requires --observation") != std::string::npos);

  const ProcessResult help = run_process(cli("help"));
  FABOBS_CHECK_EQ(help.exit_code, 0);
  FABOBS_CHECK(help.output.find("commands:") != std::string::npos);
}

FABOBS_TEST(cli, verify_reports_corruption_and_fails) {
  TempFile journal("cli-verify-corrupt");
  ObservationFile observations("cli-verify-corrupt");
  observations.write({observation_line(kDeviceA.typed_text().c_str(), "link.state", "up", 1)});
  const std::string journal_argument = " --journal " + quote(journal.path()) + " --fabric cli";
  FABOBS_REQUIRE(run_process(cli("ingest --observation " + quote(observations.path()) +
                                 journal_argument))
                     .exit_code == 0);
  FABOBS_CHECK_EQ(run_process(cli("verify" + journal_argument)).exit_code, 0);

  // Damage a byte inside the first record's payload.
  {
    std::fstream stream(journal.path(), std::ios::binary | std::ios::in | std::ios::out);
    FABOBS_REQUIRE(stream.good());
    stream.seekg(static_cast<std::streamoff>(kJournalHeaderBytes + kJournalRecordHeaderBytes + 4));
    char byte = 0;
    stream.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x33);
    stream.seekp(static_cast<std::streamoff>(kJournalHeaderBytes + kJournalRecordHeaderBytes + 4));
    stream.write(&byte, 1);
  }

  const ProcessResult damaged = run_process(cli("verify" + journal_argument));
  FABOBS_REQUIRE(damaged.started);
  FABOBS_CHECK(damaged.exit_code != 0);
  FABOBS_CHECK(damaged.output.find("corrupt=yes") != std::string::npos);
  FABOBS_CHECK(damaged.output.find("diagnostic at") != std::string::npos);
}

#else
FABOBS_TEST(cli, unavailable_without_the_tool) {
  FABOBS_FAIL("the fabobs tool is required for the end to end tests");
}
#endif  // FABOBS_TEST_BINARY
