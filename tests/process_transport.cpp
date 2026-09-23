// Real independent-process transport tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime claims behaviour across a process boundary: the newline delimited
// JSON protocol the fabobs tool speaks on stdio. These tests drive a real child
// process over real pipes - no in-process stand-in, no loopback socket, no
// timing assumptions.

#include "testing.hpp"

#include "process_runner.hpp"
#include "runtime_fixture.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

#ifdef FABOBS_TEST_BINARY
namespace {

const SubjectIdentity kDevice = SubjectIdentity::of(DeviceId::derive("device/remote"));

std::string fabric_name(const char* suffix) { return std::string("transport-") + suffix; }

std::string observation_json(std::uint64_t sequence, const std::string& value,
                            const char* fabric_suffix = "a") {
  return "{\"schema\":\"fabric-observatory/observation/1\","
         "\"fabric\":\"" + FabricId::derive("fabric/" + fabric_name(fabric_suffix)).to_text() + "\","
         "\"source\":\"" + source_id("remote").to_text() + "\","
         "\"incarnation\":\"" + incarnation("remote/boot-1").to_text() + "\","
         "\"sequence\":\"" + std::to_string(sequence) + "\",\"generation\":\"1\",\"epoch\":\"1\","
         "\"observed_at\":\"" + std::to_string(kFixtureTime) + "\","
         "\"claims\":[{\"subject\":\"" + kDevice.typed_text() +
         "\",\"aspect\":\"link.state\",\"value\":{\"k\":\"text\",\"v\":\"" + value + "\"}}]}";
}

std::string serve_command(const std::string& journal, const char* fabric_suffix) {
  // A manual clock makes the child's receive times a function of the input
  // rather than of when it happened to run, which is what lets two independent
  // processes produce the same snapshot identity.
  return quote(FABOBS_TEST_BINARY) + " serve --stdio --journal " + quote(journal) + " --fabric " +
         quote(fabric_name(fabric_suffix)) + " --clock manual:" + std::to_string(kFixtureTime);
}

struct Response {
  bool found{false};
  bool ok{false};
  std::string text{};
};

Response read_response(ChildProcess& child) {
  Response response;
  const std::optional<std::string> line = child.read_line();
  if (!line.has_value()) {
    return response;
  }
  response.found = true;
  response.text = *line;
  response.ok = line->find("\"ok\":true") != std::string::npos;
  return response;
}

std::string extract_member(const std::string& json, const std::string& member) {
  const std::string needle = "\"" + member + "\":\"";
  const std::size_t start = json.find(needle);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t value_start = start + needle.size();
  const std::size_t end = json.find('"', value_start);
  if (end == std::string::npos) {
    return {};
  }
  return json.substr(value_start, end - value_start);
}

}  // namespace

FABOBS_TEST(transport, a_real_child_process_answers_every_operation) {
  TempFile journal("transport-basic");
  std::optional<ChildProcess> child = ChildProcess::start(serve_command(journal.path(), "a"));
  FABOBS_REQUIRE(child.has_value());
  ChildProcess process = std::move(*child);

  const Response ping = [&]() -> Response {
    if (!process.write_line("{\"op\":\"ping\"}")) {
      return Response{};
    }
    return read_response(process);
  }();
  FABOBS_REQUIRE(ping.found);
  FABOBS_CHECK(ping.ok);
  FABOBS_CHECK(ping.text.find("fabric-observatory/observation/1") != std::string::npos);
  FABOBS_CHECK(ping.text.find("lock_audit") != std::string::npos);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"snapshot\"}"));
  const Response empty = read_response(process);
  FABOBS_REQUIRE(empty.found);
  FABOBS_CHECK(empty.ok);
  FABOBS_CHECK(empty.text.find("\"subjects\":\"0\"") != std::string::npos);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"ingest\",\"observation\":" +
                                    observation_json(1, "up") + "}"));
  const Response ingested = read_response(process);
  FABOBS_REQUIRE(ingested.found);
  FABOBS_CHECK(ingested.ok);
  FABOBS_CHECK(ingested.text.find("\"disposition\":\"accepted\"") != std::string::npos);
  const std::string first_snapshot = extract_member(ingested.text, "snapshot");
  FABOBS_CHECK(!first_snapshot.empty());

  // A replayed sequence is reported as fenced, and the response is still an ok
  // response carrying the disposition: disagreement is data, not a failure.
  FABOBS_REQUIRE(process.write_line("{\"op\":\"ingest\",\"observation\":" +
                                    observation_json(1, "down") + "}"));
  const Response fenced = read_response(process);
  FABOBS_REQUIRE(fenced.found);
  FABOBS_CHECK(fenced.ok);
  FABOBS_CHECK(fenced.text.find("FencedSequence") != std::string::npos);

  FABOBS_REQUIRE(process.write_line(
      "{\"op\":\"query\",\"filter\":{\"kind\":\"device\",\"include_claims\":true}}"));
  const Response queried = read_response(process);
  FABOBS_REQUIRE(queried.found);
  FABOBS_CHECK(queried.ok);
  FABOBS_CHECK(queried.text.find("\"matched_subjects\":\"1\"") != std::string::npos);

  // Freshness is evaluated at an instant the caller chooses. Pinning it makes
  // the child's verdict a function of the evidence rather than of how long the
  // test took to reach this line, and it is what makes the result reproducible
  // across machines.
  FABOBS_REQUIRE(process.write_line("{\"op\":\"refresh\",\"at\":\"" +
                                    std::to_string(kFixtureTime) + "\"}"));
  const Response refreshed = read_response(process);
  FABOBS_REQUIRE(refreshed.found);
  FABOBS_CHECK(refreshed.ok);
  FABOBS_CHECK(refreshed.text.find("\"truth\":\"known\"") != std::string::npos);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"hierarchy\"}"));
  FABOBS_CHECK(read_response(process).ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"sources\"}"));
  FABOBS_CHECK(read_response(process).ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"history\",\"max\":\"10\"}"));
  FABOBS_CHECK(read_response(process).ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"diff\",\"lookback\":\"1\"}"));
  FABOBS_CHECK(read_response(process).ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"stats\"}"));
  FABOBS_CHECK(read_response(process).ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"recovery\"}"));
  FABOBS_CHECK(read_response(process).ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"policy\"}"));
  FABOBS_CHECK(read_response(process).ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"explain\",\"subject\":\"" + kDevice.typed_text() +
                                    "\",\"aspect\":\"link.state\"}"));
  const Response explained = read_response(process);
  FABOBS_REQUIRE(explained.found);
  FABOBS_CHECK(explained.ok);
  FABOBS_CHECK(explained.text.find("\"truth\":\"known\"") != std::string::npos);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"nonsense\"}"));
  const Response unknown = read_response(process);
  FABOBS_REQUIRE(unknown.found);
  FABOBS_CHECK(!unknown.ok);
  FABOBS_CHECK(unknown.text.find("Unsupported") != std::string::npos);

  FABOBS_REQUIRE(process.write_line("this is not json"));
  const Response garbage = read_response(process);
  FABOBS_REQUIRE(garbage.found);
  FABOBS_CHECK(!garbage.ok);

  FABOBS_REQUIRE(process.write_line("{\"op\":\"shutdown\"}"));
  const Response shutting_down = read_response(process);
  FABOBS_REQUIRE(shutting_down.found);
  FABOBS_CHECK(shutting_down.ok);
  FABOBS_CHECK_EQ(process.wait(), 0);
}

FABOBS_TEST(transport, one_response_per_request_with_no_interleaving) {
  TempFile journal("transport-burst");
  std::optional<ChildProcess> child = ChildProcess::start(serve_command(journal.path(), "b"));
  FABOBS_REQUIRE(child.has_value());
  ChildProcess process = std::move(*child);

  constexpr int kRequests = 200;
  std::string burst;
  for (int index = 0; index < kRequests; ++index) {
    burst += "{\"op\":\"ping\"}\n";
  }
  FABOBS_REQUIRE(process.write_bytes(burst));

  std::vector<std::string> lines;
  for (int index = 0; index < kRequests; ++index) {
    const std::optional<std::string> line = process.read_line();
    FABOBS_REQUIRE(line.has_value());
    lines.push_back(*line);
  }
  for (const std::string& line : lines) {
    FABOBS_CHECK(line.find("\"ok\":true") != std::string::npos);
    // Exactly one JSON object per line: no partial writes, no interleaving.
    FABOBS_CHECK_EQ(line.front(), '{');
    FABOBS_CHECK_EQ(line.back(), '}');
  }
  FABOBS_REQUIRE(process.write_line("{\"op\":\"shutdown\"}"));
  FABOBS_CHECK(read_response(process).ok);
  FABOBS_CHECK_EQ(process.wait(), 0);
}

FABOBS_TEST(transport, two_processes_agree_on_snapshot_identity) {
  TempFile first_journal("transport-determinism-a");
  TempFile second_journal("transport-determinism-b");

  const auto run = [](const std::string& journal) {
    std::string identity;
    std::optional<ChildProcess> child = ChildProcess::start(serve_command(journal, "c"));
    if (!child.has_value()) {
      return identity;
    }
    ChildProcess process = std::move(*child);
    for (std::uint64_t sequence = 1; sequence <= 6; ++sequence) {
      if (!process.write_line("{\"op\":\"ingest\",\"observation\":" +
                              observation_json(sequence, sequence % 2 == 0 ? "up" : "down", "c") +
                              "}")) {
        return identity;
      }
      const Response response = read_response(process);
      if (!response.found || !response.ok) {
        return identity;
      }
    }
    // Pin the evaluation instant so that the identity is a function of the
    // evidence alone and not of when the two runs happened to execute.
    if (!process.write_line("{\"op\":\"refresh\",\"at\":\"" + std::to_string(kFixtureTime) +
                            "\"}")) {
      return identity;
    }
    const Response snapshot = read_response(process);
    if (!snapshot.found || !snapshot.ok) {
      return identity;
    }
    identity = extract_member(snapshot.text, "id");
    (void)process.write_line("{\"op\":\"shutdown\"}");
    read_response(process);
    process.wait();
    return identity;
  };

  const std::string first = run(first_journal.path());
  const std::string second = run(second_journal.path());
  FABOBS_CHECK(!first.empty());
  FABOBS_CHECK_EQ(first, second);
}

FABOBS_TEST(transport, restart_across_processes_degrades_freshness) {
  TempFile journal("transport-restart");
  {
    std::optional<ChildProcess> child = ChildProcess::start(serve_command(journal.path(), "d"));
    FABOBS_REQUIRE(child.has_value());
    ChildProcess process = std::move(*child);
    FABOBS_REQUIRE(process.write_line("{\"op\":\"ingest\",\"observation\":" +
                                      observation_json(1, "up", "d") + "}"));
    const Response response = read_response(process);
    FABOBS_REQUIRE(response.ok);
    FABOBS_CHECK(response.text.find("FencedDuplicateContent") == std::string::npos);
    FABOBS_REQUIRE(process.write_line("{\"op\":\"shutdown\"}"));
    read_response(process);
    FABOBS_CHECK_EQ(process.wait(), 0);
  }

  {
    std::optional<ChildProcess> child = ChildProcess::start(serve_command(journal.path(), "d"));
    FABOBS_REQUIRE(child.has_value());
    ChildProcess process = std::move(*child);

    FABOBS_REQUIRE(process.write_line("{\"op\":\"recovery\"}"));
    const Response recovery = read_response(process);
    FABOBS_REQUIRE(recovery.found);
    FABOBS_CHECK(recovery.ok);
    FABOBS_CHECK(recovery.text.find("\"records_accepted\":\"1\"") != std::string::npos);
    FABOBS_CHECK(recovery.text.find("\"chain_verified\":true") != std::string::npos);

    FABOBS_REQUIRE(process.write_line("{\"op\":\"refresh\",\"at\":\"" +
                                      std::to_string(kFixtureTime) + "\"}"));
    const Response snapshot = read_response(process);
    FABOBS_REQUIRE(snapshot.found);
    FABOBS_CHECK(snapshot.ok);
    FABOBS_CHECK(snapshot.text.find("\"restart_count\":\"1\"") != std::string::npos);
    FABOBS_CHECK(snapshot.text.find("\"recovered_claims\":\"1\"") != std::string::npos);
    // The persisted evidence came back as stale, not as fresh.
    FABOBS_CHECK(snapshot.text.find("\"truth\":\"stale\"") != std::string::npos);
    FABOBS_CHECK(snapshot.text.find("\"freshness\":\"aging\"") != std::string::npos);

    FABOBS_REQUIRE(process.write_line("{\"op\":\"shutdown\"}"));
    read_response(process);
    FABOBS_CHECK_EQ(process.wait(), 0);
  }
}

FABOBS_TEST(transport, an_unterminated_line_cannot_grow_the_server) {
  TempFile journal("transport-hostile");
  std::optional<ChildProcess> child = ChildProcess::start(serve_command(journal.path(), "e"));
  FABOBS_REQUIRE(child.has_value());
  ChildProcess process = std::move(*child);

  // A client that never sends a newline must be refused, not buffered. The
  // server answers as soon as the bound is crossed and then waits for the real
  // newline.
  std::string hostile(4u << 20, 'x');
  FABOBS_REQUIRE(process.write_bytes(hostile));
  FABOBS_REQUIRE(process.write_line(""));
  const Response refused = read_response(process);
  FABOBS_REQUIRE(refused.found);
  FABOBS_CHECK(!refused.ok);
  FABOBS_CHECK(refused.text.find("TooLarge") != std::string::npos);

  // The stream is still framed: the next request is answered normally.
  FABOBS_REQUIRE(process.write_line("{\"op\":\"ping\"}"));
  FABOBS_CHECK(read_response(process).ok);
  FABOBS_REQUIRE(process.write_line("{\"op\":\"shutdown\"}"));
  FABOBS_CHECK(read_response(process).ok);
  FABOBS_CHECK_EQ(process.wait(), 0);
}

#else
FABOBS_TEST(transport, unavailable_without_the_tool) {
  FABOBS_FAIL("the fabobs tool is required for the process transport tests");
}
#endif  // FABOBS_TEST_BINARY
