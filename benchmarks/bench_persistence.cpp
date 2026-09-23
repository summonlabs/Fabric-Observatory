// Benchmark: journal append and integrity checked recovery.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The benchmark writes to a file under the current directory and removes it
// before returning, so it leaves no debris behind.

#include "benchmark_support.hpp"

#include "fabric_observatory/persistence.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace fabric_observatory;
using namespace fabric_observatory::bench;

namespace {

constexpr std::uint32_t kRecords = 4096;

const Limits kLimits{};

ObservationRecord make_record(FabricId fabric, std::uint32_t index) {
  Observation observation;
  observation.schema = std::string(observation_schema());
  observation.fabric = fabric;
  observation.source = SourceId::derive("source/" + std::to_string(index % 4));
  observation.incarnation = IncarnationId::derive("source/boot-1");
  observation.sequence = SourceSequence(index + 1);
  observation.generation = GenerationId(2);
  observation.epoch = EpochId(1);
  observation.observed_at = TimePoint{7000000000000};

  Claim claim;
  claim.subject = SubjectIdentity::of(DeviceId::derive("device/" + std::to_string(index % 512)));
  claim.aspect = *well_known_aspect("congestion.level");
  claim.value = Value::unsigned_integer(index % 100);
  observation.claims.push_back(std::move(claim));
  observation.canonicalize();

  ObservationRecord record;
  record.observation = std::move(observation);
  record.received_at = TimePoint{7000000000000 + index};
  return record;
}

}  // namespace

int main() {
  const FabricId fabric = FabricId::derive("fabric/benchmark");
  const std::string path = "fabobs-bench-journal.log";
  std::remove(path.c_str());

  JournalOptions options;
  options.fabric = fabric;
  options.fsync_on_append = false;  // the benchmark measures format work, not disk flush
  options.limits = kLimits;
  options.max_bytes = 512ull * 1024ull * 1024ull;

  Result<std::unique_ptr<Journal>> opened = Journal::open(path, options);
  if (!opened) {
    std::fprintf(stderr, "bench_persistence: %s\n", opened.status().to_string().c_str());
    return 1;
  }
  Journal& journal = **opened;

  std::uint64_t checksum = 0;
  std::uint64_t appended = 0;
  std::vector<ObservationRecord> records;
  records.reserve(kRecords);
  for (std::uint32_t index = 0; index < kRecords; ++index) {
    records.push_back(make_record(fabric, index));
  }

  Timer append_timer;
  for (const ObservationRecord& record : records) {
    const Result<void> status = journal.append(record);
    if (status) {
      ++appended;
      checksum += record.observation.content_digest().bytes[0];
    }
  }
  journal.flush();
  report("journal_append", "records", appended, append_timer.elapsed_nanos(), checksum);
  const std::uint64_t bytes = journal.size_bytes();
  journal.close();

  Timer recover_timer;
  std::uint64_t recovered = 0;
  JournalOptions read_options = options;
  Result<std::unique_ptr<Journal>> reopened = Journal::open(path, read_options);
  if (reopened) {
    recovered = static_cast<std::uint64_t>((*reopened)->recovered_records().size());
    checksum += (*reopened)->recovery().records_accepted;
    (*reopened)->close();
  }
  report("journal_recover", "records", recovered, recover_timer.elapsed_nanos(), checksum);
  std::printf("  journal_bytes=%llu bytes_per_record=%.1f recovered=%llu\n",
              static_cast<unsigned long long>(bytes),
              appended == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(appended),
              static_cast<unsigned long long>(recovered));

  std::remove(path.c_str());
  return 0;
}
