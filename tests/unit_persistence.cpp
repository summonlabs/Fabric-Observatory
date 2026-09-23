// Unit tests: journal format, integrity checking and conservative recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include "runtime_fixture.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabobs_test;

namespace {

std::vector<char> read_bytes(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool write_bytes(const std::string& path, const std::vector<char>& data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return false;
  }
  stream.write(data.data(), static_cast<std::streamsize>(data.size()));
  return stream.good();
}

JournalOptions default_options() {
  JournalOptions options;
  options.fabric = test_fabric();
  options.fsync_on_append = false;
  return options;
}

ObservationRecord make_record(std::uint64_t sequence, const std::string& device) {
  ObservationBuilder builder(source_id("persist"), "persist/boot-1", sequence, 1, 1,
                             static_cast<std::int64_t>(sequence) * 1000);
  builder.claim_text(device_subject(device), "link.state", sequence % 2 == 0 ? "up" : "down");
  ObservationRecord record;
  record.observation = builder.build();
  record.received_at = TimePoint{static_cast<std::int64_t>(sequence) * 1000};
  return record;
}

struct WrittenJournal {
  std::vector<ObservationRecord> records;
  std::uint64_t bytes{0};
};

Result<WrittenJournal> write_journal(const std::string& path, std::uint64_t count) {
  Result<std::unique_ptr<Journal>> opened = Journal::open(path, default_options());
  if (!opened) {
    return Err<WrittenJournal>(opened.status().code(), opened.status().message());
  }
  WrittenJournal written;
  for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
    ObservationRecord record = make_record(sequence, "device-" + std::to_string(sequence));
    const Result<void> status = (*opened)->append(record);
    if (!status) {
      return Err<WrittenJournal>(status.status().code(), status.status().message());
    }
    written.records.push_back(std::move(record));
  }
  (*opened)->flush();
  written.bytes = (*opened)->size_bytes();
  (*opened)->close();
  return Ok(std::move(written));
}

}  // namespace

FABOBS_TEST(persistence, round_trip_preserves_every_field) {
  TempFile file("roundtrip");
  Result<WrittenJournal> written = write_journal(file.path(), 5);
  FABOBS_REQUIRE(written.has_value());

  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(reopened.has_value());
  const RecoveryReport& report = (*reopened)->recovery();
  FABOBS_CHECK(report.opened);
  FABOBS_CHECK(!report.created);
  FABOBS_CHECK(report.format_supported);
  FABOBS_CHECK_EQ(report.format_version, kJournalFormatVersion);
  FABOBS_CHECK(report.chain_verified);
  FABOBS_CHECK(!report.corrupt);
  FABOBS_CHECK(!report.truncated);
  FABOBS_CHECK_EQ(report.records_read, std::uint64_t{5});
  FABOBS_CHECK_EQ(report.records_accepted, std::uint64_t{5});
  FABOBS_CHECK_EQ(report.records_rejected, std::uint64_t{0});

  const std::vector<ObservationRecord>& recovered = (*reopened)->recovered_records();
  FABOBS_REQUIRE(recovered.size() == written->records.size());
  for (std::size_t index = 0; index < recovered.size(); ++index) {
    FABOBS_CHECK_EQ(recovered[index], written->records[index]);
    FABOBS_CHECK_EQ(recovered[index].observation.content_digest(),
                    written->records[index].observation.content_digest());
  }
}

FABOBS_TEST(persistence, a_fresh_journal_is_reported_as_created) {
  TempFile file("created");
  Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(opened.has_value());
  FABOBS_CHECK((*opened)->recovery().created);
  FABOBS_CHECK((*opened)->recovery().opened);
  FABOBS_CHECK((*opened)->recovered_records().empty());
  FABOBS_CHECK((*opened)->size_bytes() >= kJournalHeaderBytes);
}

FABOBS_TEST(persistence, replayed_records_are_refused_by_the_sequence_fence) {
  TempFile file("replay");
  ObservatoryFixture fixture(Policy{}, file.path());
  FABOBS_REQUIRE(fixture.valid());
  const Observation first =
      ObservationBuilder(source_id("s"), "s/boot-1", 1).claim_text(device_subject("d"), "link.state", "up").build();
  FABOBS_CHECK_EQ(fixture.ingest(first).disposition, IngestDisposition::Accepted);
  const IngestOutcome replay = fixture.ingest(first);
  FABOBS_CHECK_EQ(replay.disposition, IngestDisposition::Duplicate);
  FABOBS_CHECK_EQ(replay.code, StatusCode::FencedDuplicateContent);

  // A different observation that reuses the sequence is a replay, not an update.
  const Observation collision =
      ObservationBuilder(source_id("s"), "s/boot-1", 1).claim_text(device_subject("d"), "link.state", "down").build();
  const IngestOutcome fenced = fixture.ingest(collision);
  FABOBS_CHECK_EQ(fenced.disposition, IngestDisposition::Rejected);
  FABOBS_CHECK_EQ(fenced.code, StatusCode::FencedSequence);
}

FABOBS_TEST(persistence, payload_corruption_is_detected) {
  TempFile file("payload-corrupt");
  FABOBS_REQUIRE(write_journal(file.path(), 4).has_value());
  std::vector<char> bytes = read_bytes(file.path());
  FABOBS_REQUIRE(bytes.size() > kJournalHeaderBytes + kJournalRecordHeaderBytes + 8);
  bytes[kJournalHeaderBytes + kJournalRecordHeaderBytes + 6] ^= 0x40;
  FABOBS_REQUIRE(write_bytes(file.path(), bytes));

  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(reopened.has_value());
  const RecoveryReport& report = (*reopened)->recovery();
  FABOBS_CHECK(report.corrupt);
  FABOBS_CHECK(!report.chain_verified);
  FABOBS_CHECK(!report.diagnostics.empty());
  FABOBS_CHECK_EQ((*reopened)->recovered_records().size(), std::size_t{0});
}

FABOBS_TEST(persistence, chain_corruption_is_detected) {
  TempFile file("chain-corrupt");
  FABOBS_REQUIRE(write_journal(file.path(), 4).has_value());
  std::vector<char> bytes = read_bytes(file.path());
  // The chain field of the first record starts 84 bytes into its header.
  bytes[kJournalHeaderBytes + 84] ^= 0x01;
  FABOBS_REQUIRE(write_bytes(file.path(), bytes));

  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(reopened.has_value());
  FABOBS_CHECK((*reopened)->recovery().corrupt);
  FABOBS_CHECK_EQ((*reopened)->recovered_records().size(), std::size_t{0});
}

FABOBS_TEST(persistence, header_corruption_is_detected) {
  TempFile file("header-corrupt");
  FABOBS_REQUIRE(write_journal(file.path(), 2).has_value());
  std::vector<char> bytes = read_bytes(file.path());
  bytes[20] ^= 0xFF;
  FABOBS_REQUIRE(write_bytes(file.path(), bytes));
  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(reopened.has_value());
  FABOBS_CHECK((*reopened)->recovery().corrupt);
  FABOBS_CHECK(!(*reopened)->recovery().diagnostics.empty());
}

FABOBS_TEST(persistence, bad_magic_is_refused) {
  TempFile file("bad-magic");
  FABOBS_REQUIRE(write_journal(file.path(), 2).has_value());
  std::vector<char> bytes = read_bytes(file.path());
  bytes[0] = 'X';
  FABOBS_REQUIRE(write_bytes(file.path(), bytes));
  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(reopened.has_value());
  FABOBS_CHECK((*reopened)->recovery().corrupt);
  // The magic is checked before the checksum, so the diagnostic names the magic.
  FABOBS_REQUIRE(!(*reopened)->recovery().diagnostics.empty());
  FABOBS_CHECK_EQ((*reopened)->recovery().diagnostics.front().code, std::string("bad-magic"));
}

FABOBS_TEST(persistence, unsupported_format_version_is_refused_not_guessed) {
  TempFile file("bad-version");
  FABOBS_REQUIRE(write_journal(file.path(), 3).has_value());
  std::vector<char> bytes = read_bytes(file.path());
  // Bump the format version, then repair the header checksum so that the version
  // is the only thing wrong with the file.
  bytes[11] = static_cast<char>(kJournalFormatVersion + 1);
  const std::uint32_t crc = crc32c(
      ByteSpan(reinterpret_cast<const std::byte*>(bytes.data()), kJournalHeaderBytes - 4));
  bytes[kJournalHeaderBytes - 4] = static_cast<char>((crc >> 24u) & 0xFFu);
  bytes[kJournalHeaderBytes - 3] = static_cast<char>((crc >> 16u) & 0xFFu);
  bytes[kJournalHeaderBytes - 2] = static_cast<char>((crc >> 8u) & 0xFFu);
  bytes[kJournalHeaderBytes - 1] = static_cast<char>(crc & 0xFFu);
  FABOBS_REQUIRE(write_bytes(file.path(), bytes));

  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), default_options());
  FABOBS_CHECK(!reopened.has_value());
  FABOBS_CHECK_EQ(reopened.status().code(), StatusCode::VersionMismatch);
}

FABOBS_TEST(persistence, journal_for_another_fabric_is_refused) {
  TempFile file("fabric-mismatch");
  FABOBS_REQUIRE(write_journal(file.path(), 2).has_value());
  JournalOptions options = default_options();
  options.fabric = FabricId::derive("fabric/other");
  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), options);
  FABOBS_CHECK(!reopened.has_value());
  FABOBS_CHECK_EQ(reopened.status().code(), StatusCode::FabricMismatch);
}

FABOBS_TEST(persistence, a_truncated_tail_is_reported_as_truncation_not_corruption) {
  TempFile file("truncated");
  Result<WrittenJournal> written = write_journal(file.path(), 5);
  FABOBS_REQUIRE(written.has_value());
  std::vector<char> bytes = read_bytes(file.path());
  bytes.resize(bytes.size() - 12);
  FABOBS_REQUIRE(write_bytes(file.path(), bytes));

  Result<std::unique_ptr<Journal>> reopened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(reopened.has_value());
  const RecoveryReport& report = (*reopened)->recovery();
  FABOBS_CHECK(report.truncated);
  FABOBS_CHECK(report.truncated_tail_bytes > 0);
  FABOBS_CHECK_EQ((*reopened)->recovered_records().size(), std::size_t{4});
  FABOBS_CHECK_EQ(report.records_accepted, std::uint64_t{4});
}

FABOBS_TEST(persistence, compact_rewrites_only_the_valid_prefix) {
  TempFile source("compact-source");
  TempFile target("compact-target");
  FABOBS_REQUIRE(write_journal(source.path(), 6).has_value());
  std::vector<char> bytes = read_bytes(source.path());
  bytes.resize(bytes.size() - 20);
  FABOBS_REQUIRE(write_bytes(source.path(), bytes));

  Result<std::unique_ptr<Journal>> opened = Journal::open(source.path(), default_options());
  FABOBS_REQUIRE(opened.has_value());
  FABOBS_CHECK_EQ((*opened)->recovered_records().size(), std::size_t{5});
  const Result<std::uint64_t> written = (*opened)->compact(target.path());
  FABOBS_REQUIRE(written.has_value());
  (*opened)->close();

  Result<std::unique_ptr<Journal>> compacted = Journal::open(target.path(), default_options());
  FABOBS_REQUIRE(compacted.has_value());
  FABOBS_CHECK(!(*compacted)->recovery().corrupt);
  FABOBS_CHECK(!(*compacted)->recovery().truncated);
  FABOBS_CHECK_EQ((*compacted)->recovered_records().size(), std::size_t{5});
}

FABOBS_TEST(persistence, oversized_records_are_refused_before_they_are_written) {
  TempFile file("oversize");
  JournalOptions options = default_options();
  options.max_record_bytes = 64;
  Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), options);
  FABOBS_REQUIRE(opened.has_value());
  const ObservationRecord record = make_record(1, "device-that-makes-the-record-too-long");
  FABOBS_CHECK_EQ((*opened)->append(record).status().code(), StatusCode::TooLarge);
  FABOBS_CHECK_EQ((*opened)->records_written(), std::uint64_t{0});
}

FABOBS_TEST(persistence, recovery_is_bounded_and_says_so) {
  TempFile file("recovery-bound");
  FABOBS_REQUIRE(write_journal(file.path(), 8).has_value());
  JournalOptions options = default_options();
  options.max_recovered_observations = 3;
  Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), options);
  FABOBS_REQUIRE(opened.has_value());
  FABOBS_CHECK_EQ((*opened)->recovered_records().size(), std::size_t{3});
  FABOBS_CHECK((*opened)->recovery().truncated);
}

FABOBS_TEST(persistence, restart_markers_are_counted_across_reopen) {
  TempFile file("restart-markers");
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), default_options());
    FABOBS_REQUIRE(opened.has_value());
    FABOBS_REQUIRE((*opened)->append_restart_marker(RestartEpoch(1), 1, TimePoint{100}).has_value());
    FABOBS_REQUIRE((*opened)->append(make_record(1, "device-1")).has_value());
    (*opened)->close();
  }
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), default_options());
    FABOBS_REQUIRE(opened.has_value());
    FABOBS_CHECK_EQ((*opened)->recovery().restart_markers, 1u);
    FABOBS_CHECK_EQ((*opened)->recovered_restart_count(), 1u);
    FABOBS_REQUIRE((*opened)->append_restart_marker(RestartEpoch(2), 2, TimePoint{200}).has_value());
    (*opened)->close();
  }
  {
    Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), default_options());
    FABOBS_REQUIRE(opened.has_value());
    FABOBS_CHECK_EQ((*opened)->recovery().restart_markers, 2u);
    FABOBS_CHECK_EQ((*opened)->recovered_restart_count(), 2u);
    FABOBS_CHECK_EQ((*opened)->recovered_restart_epoch().value(), std::uint64_t{2});
    FABOBS_CHECK_EQ((*opened)->recovered_records().size(), std::size_t{1});
  }
}

FABOBS_TEST(persistence, record_kind_tampering_is_detected) {
  TempFile file("kind-tamper");
  FABOBS_REQUIRE(write_journal(file.path(), 2).has_value());
  std::vector<char> bytes = read_bytes(file.path());
  // The record kind is part of the hash chain, so rewriting it is corruption
  // rather than a record this build merely does not understand.
  bytes[kJournalHeaderBytes + 8] = static_cast<char>(99);
  FABOBS_REQUIRE(write_bytes(file.path(), bytes));
  Result<std::unique_ptr<Journal>> opened = Journal::open(file.path(), default_options());
  FABOBS_REQUIRE(opened.has_value());
  FABOBS_CHECK((*opened)->recovery().corrupt);
  FABOBS_CHECK_EQ((*opened)->recovered_records().size(), std::size_t{0});
  bool named_chain = false;
  for (const RecoveryDiagnostic& diagnostic : (*opened)->recovery().diagnostics) {
    if (diagnostic.code == "record-chain" || diagnostic.code == "record-content-digest") {
      named_chain = true;
    }
  }
  FABOBS_CHECK(named_chain);
}

FABOBS_TEST(persistence, canonical_observation_payload_round_trips_exactly) {
  ObservationBuilder builder(source_id("payload"), "payload/boot-1", 7, 3, 2, 12345);
  builder.claim_text(device_subject("d1"), "link.state", "up")
      .claim(device_subject("d2"), "capacity.bandwidth_bps", Value::unsigned_integer(100000000000ull))
      .claim(device_subject("d3"), "operational.health", *Value::text("ok", kLimits.values), false)
      .causal("same plane", CausalStrength::TemporallyPrecedes, 0xAB)
      .meta("collector", "unit-test");
  ObservationRecord record;
  record.observation = builder.build();
  record.received_at = TimePoint{999999};
  record.recovered = true;
  record.recovery_epoch = RestartEpoch(4);

  CanonicalEncoder encoder;
  encode_observation_record(encoder, record);
  Result<ObservationRecord> decoded =
      decode_observation_record(ByteSpan(encoder.buffer().data(), encoder.buffer().size()), kLimits);
  FABOBS_REQUIRE(decoded.has_value());
  FABOBS_CHECK_EQ(*decoded, record);
  FABOBS_CHECK_EQ(decoded->observation.content_digest(), record.observation.content_digest());

  // The payload is only one field of a journal record, so it must decode even
  // when more bytes follow it; that is exactly what journal recovery relies on.
  CanonicalEncoder extended;
  encode_observation_record(extended, record);
  const std::size_t prefix = extended.buffer().size();
  extended.u64(0xDEADBEEF);
  Result<ObservationRecord> nested =
      decode_observation_record(ByteSpan(extended.buffer().data(), prefix), kLimits);
  FABOBS_REQUIRE(nested.has_value());
  FABOBS_CHECK_EQ(*nested, record);

  // A payload with a trailing byte after the record is refused.
  CanonicalDecoder trailing(ByteSpan(extended.buffer().data(), extended.buffer().size()));
  Result<ObservationRecord> rejected =
      decode_observation_record(ByteSpan(extended.buffer().data(), extended.buffer().size()), kLimits);
  FABOBS_CHECK(!rejected.has_value());
  FABOBS_CHECK_EQ(rejected.status().code(), StatusCode::Corrupt);
  (void)trailing;
}
