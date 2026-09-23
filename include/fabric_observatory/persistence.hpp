// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_PERSISTENCE_HPP
#define FABRIC_OBSERVATORY_PERSISTENCE_HPP

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "fabric_observatory/limits.hpp"
#include "fabric_observatory/observation.hpp"
#include "fabric_observatory/snapshot.hpp"

namespace fabric_observatory {

// On-disk journal
// --------------
// File header (128 bytes, little-endian-free: every integer is big-endian):
//   0..7    magic "FABOBSJ1"
//   8..11   format version
//   12..15  header size in bytes
//   16..31  fabric identity
//   32..39  creation wall clock (nanoseconds since the epoch)
//   40..71  chain seed for the first record of this file
//   72..123 reserved, zero
//   124..127 CRC-32C of bytes 0..123
//
// Record header (116 bytes):
//   0..3    payload length
//   4..7    payload CRC-32C
//   8       record kind
//   9..11   flags, zero
//   12..19  record sequence within the file
//   20..51  previous chain value
//   52..83  SHA-256 of the payload
//   84..115 chain value = SHA-256(prev_chain || content_digest || kind || sequence)
//
// Recovery verifies the CRC, the content digest, the chain link and the payload
// structure for every record, in that order. A failure stops recovery at the
// first bad record; nothing is repaired, truncated or deleted automatically.
// The valid prefix can be rewritten explicitly with compact().

inline constexpr std::uint32_t kJournalHeaderBytes = 128;
inline constexpr std::uint32_t kJournalRecordHeaderBytes = 116;
inline constexpr std::string_view kJournalMagic = "FABOBSJ1";

enum class JournalRecordKind : std::uint8_t {
  Observation = 1,
  RestartMarker = 2,
  SnapshotMarker = 3,
};

struct JournalOptions {
  // The fabric this journal belongs to. Opening a journal written for another
  // fabric is refused rather than silently merged.
  FabricId fabric{};
  // Hard bound on one journal part. A record that would cross the bound causes
  // a rotation instead.
  std::uint64_t max_bytes{64ull * 1024ull * 1024ull};
  // Total parts including the active one. Older parts are deleted on rotation,
  // which is what bounds persistence growth.
  std::uint32_t max_files{8};
  std::uint64_t max_record_bytes{4ull * 1024ull * 1024ull};
  std::uint32_t max_recovered_observations{1u << 20};
  bool fsync_on_append{true};
  Limits limits{};

  friend bool operator==(const JournalOptions&, const JournalOptions&) = default;
};

struct RecoveryDiagnostic {
  std::uint64_t offset{0};
  std::string code{};
  std::string detail{};

  friend bool operator==(const RecoveryDiagnostic&, const RecoveryDiagnostic&) = default;
  friend auto operator<=>(const RecoveryDiagnostic&, const RecoveryDiagnostic&) = default;
};

struct RecoveryReport {
  bool opened{false};
  bool created{false};
  bool format_supported{false};
  std::uint32_t format_version{0};
  std::uint64_t file_bytes{0};
  std::uint64_t records_read{0};
  std::uint64_t records_accepted{0};
  std::uint64_t records_rejected{0};
  // Bytes at the end of the newest part that could not be formed into a
  // complete, verified record.
  std::uint64_t truncated_tail_bytes{0};
  std::uint64_t snapshot_markers{0};
  std::uint32_t parts_recovered{0};
  std::uint32_t restart_markers{0};
  bool chain_verified{false};
  bool corrupt{false};
  bool truncated{false};
  std::vector<RecoveryDiagnostic> diagnostics{};

  [[nodiscard]] std::string to_text() const;
};

struct SnapshotMarker {
  SnapshotId id{};
  GenerationId generation{};
  EpochId epoch{};
  TimePoint published_at{};

  friend bool operator==(const SnapshotMarker&, const SnapshotMarker&) = default;
};

// Encode and decode an observation record to its canonical payload form. The
// functions are exposed so that tests can exercise the format directly.
void encode_observation_record(CanonicalEncoder& encoder, const ObservationRecord& record);
Result<ObservationRecord> decode_observation_record(ByteSpan payload, const Limits& limits);

class Journal {
 public:
  // Opens (creating if necessary) and immediately recovers. Recovery never
  // modifies the file.
  [[nodiscard]] static Result<std::unique_ptr<Journal>> open(std::string path,
                                                             JournalOptions options);
  ~Journal();

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const std::vector<ObservationRecord>& recovered_records() const noexcept {
    return recovered_records_;
  }
  [[nodiscard]] const std::vector<SnapshotMarker>& recovered_snapshots() const noexcept {
    return recovered_snapshots_;
  }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::uint64_t size_bytes() const noexcept { return current_bytes_; }
  [[nodiscard]] std::uint32_t rotations() const noexcept { return rotations_; }
  [[nodiscard]] std::uint64_t records_written() const noexcept { return records_written_; }
  [[nodiscard]] RestartEpoch recovered_restart_epoch() const noexcept {
    return recovered_restart_epoch_;
  }
  [[nodiscard]] std::uint32_t recovered_restart_count() const noexcept {
    return recovered_restart_count_;
  }

  Result<void> append(const ObservationRecord& record);
  Result<void> append_restart_marker(RestartEpoch epoch, std::uint32_t restart_count,
                                     TimePoint wall_clock);
  Result<void> append_snapshot_marker(const SnapshotMarker& marker);
  Result<void> flush();
  Result<void> close();

  // Rewrites the valid prefix of this journal into target_path. This is the only
  // operation that discards bytes, and it is always explicit.
  Result<std::uint64_t> compact(const std::string& target_path) const;

 private:
  Journal() = default;

  Result<void> open_file(bool create);
  Result<void> write_header();
  Result<void> rotate();
  Result<void> write_record(JournalRecordKind kind, ByteSpan payload);
  Result<void> recover();
  [[nodiscard]] std::string part_path(std::uint32_t index) const;

  std::string path_{};
  JournalOptions options_{};
  std::FILE* file_{nullptr};
  std::uint64_t current_bytes_{0};
  std::uint64_t records_written_{0};
  std::uint64_t file_sequence_{0};
  std::uint32_t rotations_{0};
  std::array<std::uint8_t, 32> chain_{};
  bool chain_initialised_{false};
  FabricId last_fabric_{};
  RestartEpoch recovered_restart_epoch_{};
  std::uint32_t recovered_restart_count_{0};
  RecoveryReport recovery_{};
  std::vector<ObservationRecord> recovered_records_{};
  std::vector<SnapshotMarker> recovered_snapshots_{};
};

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_PERSISTENCE_HPP
