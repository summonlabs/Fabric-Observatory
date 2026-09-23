// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/persistence.hpp"

#include "fabric_observatory/canonical.hpp"
#include "fabric_observatory/checked.hpp"
#include "fabric_observatory/digest.hpp"
#include "fabric_observatory/util.hpp"
#include "fabric_observatory/version.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace fabric_observatory {

namespace {

void put_u32(std::uint8_t* out, std::uint32_t value) {
  out[0] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
  out[2] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
  out[3] = static_cast<std::uint8_t>(value & 0xFFu);
}

void put_u64(std::uint8_t* out, std::uint64_t value) {
  put_u32(out, static_cast<std::uint32_t>((value >> 32u) & 0xFFFFFFFFu));
  put_u32(out + 4, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
}

std::uint32_t get_u32(const std::uint8_t* in) {
  return (static_cast<std::uint32_t>(in[0]) << 24u) | (static_cast<std::uint32_t>(in[1]) << 16u) |
         (static_cast<std::uint32_t>(in[2]) << 8u) | static_cast<std::uint32_t>(in[3]);
}

std::uint64_t get_u64(const std::uint8_t* in) {
  return (static_cast<std::uint64_t>(get_u32(in)) << 32u) | static_cast<std::uint64_t>(get_u32(in + 4));
}

bool write_all(std::FILE* file, const void* data, std::size_t size) {
  return size == 0 || std::fwrite(data, 1, size, file) == size;
}

bool read_exact(std::FILE* file, void* data, std::size_t size) {
  return size == 0 || std::fread(data, 1, size, file) == size;
}

// Portable file open. On Windows the journal is opened with explicit read
// sharing: a runtime that held its journal exclusively would block the
// inspection tooling that exists to look at it. On other platforms std::fopen
// already shares.
std::FILE* open_stream(const char* path, const char* mode) {
#if defined(_WIN32)
  return ::_fsopen(path, mode, _SH_DENYNO);
#else
  return std::fopen(path, mode);
#endif
}

std::uint64_t file_size(std::FILE* file) {
  const long current = std::ftell(file);
  if (current < 0) {
    return 0;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    return 0;
  }
  const long end = std::ftell(file);
  std::fseek(file, current, SEEK_SET);
  return end < 0 ? 0 : static_cast<std::uint64_t>(end);
}

std::array<std::uint8_t, 32> compute_chain(const std::array<std::uint8_t, 32>& previous,
                                           const std::array<std::uint8_t, 32>& content,
                                           JournalRecordKind kind, std::uint64_t sequence) {
  Sha256 hasher;
  hasher.update(ByteSpan(reinterpret_cast<const std::byte*>(previous.data()), previous.size()));
  hasher.update(ByteSpan(reinterpret_cast<const std::byte*>(content.data()), content.size()));
  const std::uint8_t kind_byte = static_cast<std::uint8_t>(kind);
  hasher.update(ByteSpan(reinterpret_cast<const std::byte*>(&kind_byte), 1));
  std::array<std::uint8_t, 8> sequence_bytes{};
  put_u64(sequence_bytes.data(), sequence);
  hasher.update(ByteSpan(reinterpret_cast<const std::byte*>(sequence_bytes.data()),
                         sequence_bytes.size()));
  return hasher.finish();
}

std::array<std::uint8_t, 32> chain_seed(FabricId fabric) {
  Sha256 hasher;
  hasher.update(std::string_view("fabric-observatory/journal/1"));
  const std::uint8_t fabric_bytes[16] = {
      static_cast<std::uint8_t>((fabric.value().hi >> 56u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().hi >> 48u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().hi >> 40u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().hi >> 32u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().hi >> 24u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().hi >> 16u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().hi >> 8u) & 0xFFu),
      static_cast<std::uint8_t>(fabric.value().hi & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().lo >> 56u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().lo >> 48u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().lo >> 40u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().lo >> 32u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().lo >> 24u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().lo >> 16u) & 0xFFu),
      static_cast<std::uint8_t>((fabric.value().lo >> 8u) & 0xFFu),
      static_cast<std::uint8_t>(fabric.value().lo & 0xFFu)};
  hasher.update(ByteSpan(reinterpret_cast<const std::byte*>(fabric_bytes), sizeof(fabric_bytes)));
  return hasher.finish();
}

void sync_file(std::FILE* file) {
  std::fflush(file);
#if defined(_WIN32)
  _commit(_fileno(file));
#else
  fsync(fileno(file));
#endif
}

}  // namespace

void encode_observation_record(CanonicalEncoder& encoder, const ObservationRecord& record) {
  encoder.tag("observation-record");
  encoder.u8(1);
  record.observation.encode(encoder);
  encoder.i64(record.received_at.nanos);
  encoder.boolean(record.recovered);
  encoder.u64(record.recovery_epoch.value());
}

Result<ObservationRecord> decode_observation_record(ByteSpan payload, const Limits& limits) {
  CanonicalDecoder decoder(payload);
  Result<void> tag = decoder.tag("observation-record");
  if (!tag) {
    return Err<ObservationRecord>(tag.status().code(), tag.status().message());
  }
  Result<void> revision = decoder.expect_u8(1);
  if (!revision) {
    return Err<ObservationRecord>(revision.status().code(), revision.status().message());
  }
  Result<Observation> observation = Observation::decode(decoder, limits);
  if (!observation) {
    return Err<ObservationRecord>(observation.status().code(), observation.status().message());
  }
  Result<std::int64_t> received_at = decoder.i64();
  if (!received_at) {
    return Err<ObservationRecord>(received_at.status().code(), received_at.status().message());
  }
  Result<bool> recovered = decoder.boolean();
  if (!recovered) {
    return Err<ObservationRecord>(recovered.status().code(), recovered.status().message());
  }
  Result<std::uint64_t> recovery_epoch = decoder.u64();
  if (!recovery_epoch) {
    return Err<ObservationRecord>(recovery_epoch.status().code(), recovery_epoch.status().message());
  }
  Result<void> end = decoder.expect_end();
  if (!end) {
    return Err<ObservationRecord>(end.status().code(), end.status().message());
  }
  ObservationRecord record;
  record.observation = std::move(*observation);
  record.received_at = TimePoint{*received_at};
  record.recovered = *recovered;
  record.recovery_epoch = RestartEpoch(*recovery_epoch);
  return Ok(std::move(record));
}

std::string RecoveryReport::to_text() const {
  std::ostringstream out;
  out << "journal_recovery opened=" << (opened ? "yes" : "no")
      << " created=" << (created ? "yes" : "no")
      << " format_version=" << format_version
      << " format_supported=" << (format_supported ? "yes" : "no") << "\n";
  out << "  file_bytes=" << file_bytes << " records_read=" << records_read
      << " records_accepted=" << records_accepted << " records_rejected=" << records_rejected
      << "\n";
  out << "  parts_recovered=" << parts_recovered << " restart_markers=" << restart_markers
      << " snapshot_markers=" << snapshot_markers << "\n";
  out << "  chain_verified=" << (chain_verified ? "yes" : "no")
      << " truncated=" << (truncated ? "yes" : "no") << " corrupt=" << (corrupt ? "yes" : "no")
      << " truncated_tail_bytes=" << truncated_tail_bytes << "\n";
  for (const RecoveryDiagnostic& diagnostic : diagnostics) {
    out << "  diagnostic at " << diagnostic.offset << ": " << diagnostic.code << " ("
        << diagnostic.detail << ")\n";
  }
  return out.str();
}

Journal::~Journal() {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

std::string Journal::part_path(std::uint32_t index) const {
  return path_ + "." + std::to_string(index);
}

Result<std::unique_ptr<Journal>> Journal::open(std::string path, JournalOptions options) {
  if (path.empty()) {
    return Err<std::unique_ptr<Journal>>(StatusCode::InvalidArgument,
                                         "journal path must not be empty");
  }
  if (options.max_bytes < kJournalHeaderBytes + kJournalRecordHeaderBytes) {
    return Err<std::unique_ptr<Journal>>(StatusCode::InvalidArgument,
                                         "journal max_bytes is too small for a single record");
  }
  if (options.max_files == 0) {
    return Err<std::unique_ptr<Journal>>(StatusCode::InvalidArgument,
                                         "journal must retain at least one part");
  }
  if (options.max_record_bytes > options.max_bytes) {
    return Err<std::unique_ptr<Journal>>(
        StatusCode::InvalidArgument, "max_record_bytes must not exceed max_bytes");
  }

  std::unique_ptr<Journal> journal(new Journal());
  journal->path_ = std::move(path);
  journal->options_ = options;

  Result<void> recovered = journal->recover();
  if (!recovered) {
    return Err<std::unique_ptr<Journal>>(recovered.status().code(), recovered.status().message());
  }

  bool exists = false;
  if (std::FILE* probe = open_stream(journal->path_.c_str(), "rb"); probe != nullptr) {
    exists = true;
    std::fclose(probe);
  }

  Result<void> opened = journal->open_file(!exists);
  if (!opened) {
    return Err<std::unique_ptr<Journal>>(opened.status().code(), opened.status().message());
  }
  journal->recovery_.opened = true;
  journal->recovery_.created = !exists;
  return Ok(std::move(journal));
}

Result<void> Journal::open_file(bool create) {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
  if (create) {
    // The chain seed is deterministic in the fabric identity; the runtime does
    // not use randomness anywhere in the persistence format.
    chain_ = chain_seed(options_.fabric);
    chain_initialised_ = true;
    file_ = open_stream(path_.c_str(), "wb+");
    if (file_ == nullptr) {
      return Err(StatusCode::IoError, "cannot create the journal file");
    }
    Result<void> header = write_header();
    if (!header) {
      return header;
    }
    current_bytes_ = kJournalHeaderBytes;
    file_sequence_ = 0;
    return VoidResult{};
  }
  file_ = open_stream(path_.c_str(), "ab+");
  if (file_ == nullptr) {
    return Err(StatusCode::IoError, "cannot open the journal file for append");
  }
  current_bytes_ = file_size(file_);
  if (current_bytes_ < kJournalHeaderBytes) {
    return Err(StatusCode::Corrupt, "journal file is shorter than its header");
  }
  return VoidResult{};
}

Result<void> Journal::write_header() {
  std::array<std::uint8_t, kJournalHeaderBytes> header{};
  std::memcpy(header.data(), kJournalMagic.data(), kJournalMagic.size());
  put_u32(header.data() + 8, kJournalFormatVersion);
  put_u32(header.data() + 12, kJournalHeaderBytes);
  put_u64(header.data() + 16, options_.fabric.value().hi);
  put_u64(header.data() + 24, options_.fabric.value().lo);
  put_u64(header.data() + 32, static_cast<std::uint64_t>(0));
  std::memcpy(header.data() + 40, chain_.data(), chain_.size());
  const std::uint32_t crc =
      crc32c(ByteSpan(reinterpret_cast<const std::byte*>(header.data()), kJournalHeaderBytes - 4));
  put_u32(header.data() + kJournalHeaderBytes - 4, crc);
  if (!write_all(file_, header.data(), header.size())) {
    return Err(StatusCode::IoError, "cannot write the journal header");
  }
  return VoidResult{};
}

Result<void> Journal::write_record(JournalRecordKind kind, ByteSpan payload) {
  if (file_ == nullptr) {
    return Err(StatusCode::NotOpen, "journal is not open");
  }
  if (payload.size() > options_.max_record_bytes) {
    return Err(StatusCode::TooLarge, "record exceeds the configured maximum record size");
  }
  const std::optional<std::uint64_t> total =
      checked_add(static_cast<std::uint64_t>(kJournalRecordHeaderBytes),
                  static_cast<std::uint64_t>(payload.size()));
  if (!total.has_value()) {
    return Err(StatusCode::TooLarge, "record size overflows");
  }
  if (current_bytes_ > kJournalHeaderBytes &&
      *total > options_.max_bytes - current_bytes_) {
    Result<void> rotated = rotate();
    if (!rotated) {
      return rotated;
    }
  }

  const std::array<std::uint8_t, 32> content = sha256(payload).bytes;
  const std::array<std::uint8_t, 32> previous = chain_;
  const std::uint64_t sequence = file_sequence_ + 1;
  const std::array<std::uint8_t, 32> chain = compute_chain(previous, content, kind, sequence);

  std::array<std::uint8_t, kJournalRecordHeaderBytes> header{};
  put_u32(header.data(), static_cast<std::uint32_t>(payload.size()));
  put_u32(header.data() + 4, crc32c(payload));
  header[8] = static_cast<std::uint8_t>(kind);
  put_u64(header.data() + 12, sequence);
  std::memcpy(header.data() + 20, previous.data(), previous.size());
  std::memcpy(header.data() + 52, content.data(), content.size());
  std::memcpy(header.data() + 84, chain.data(), chain.size());

  if (!write_all(file_, header.data(), header.size()) ||
      !write_all(file_, payload.data(), payload.size())) {
    return Err(StatusCode::IoError, "cannot write the journal record");
  }
  chain_ = chain;
  chain_initialised_ = true;
  file_sequence_ = sequence;
  ++records_written_;
  current_bytes_ += *total;
  if (options_.fsync_on_append) {
    sync_file(file_);
  }
  return VoidResult{};
}

Result<void> Journal::rotate() {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
  ++rotations_;
  if (options_.max_files > 1) {
    // Drop the oldest part, then shift every remaining part up by one and move
    // the active file into slot one. Rotation is the only place the runtime
    // deletes persisted bytes, and the bound it enforces is max_files parts.
    std::remove(part_path(options_.max_files - 1).c_str());
    for (std::int64_t index = static_cast<std::int64_t>(options_.max_files) - 2; index >= 1;
         --index) {
      const std::string from = part_path(static_cast<std::uint32_t>(index));
      const std::string to = part_path(static_cast<std::uint32_t>(index + 1));
      std::remove(to.c_str());
      std::rename(from.c_str(), to.c_str());
    }
    std::remove(part_path(1).c_str());
    std::rename(path_.c_str(), part_path(1).c_str());
  }
  file_ = open_stream(path_.c_str(), "wb+");
  if (file_ == nullptr) {
    return Err(StatusCode::IoError, "cannot create the rotated journal part");
  }
  Result<void> header = write_header();
  if (!header) {
    return header;
  }
  current_bytes_ = kJournalHeaderBytes;
  file_sequence_ = 0;
  return VoidResult{};
}

Result<void> Journal::append(const ObservationRecord& record) {
  CanonicalEncoder encoder(256 + record.observation.claims.size() * 96);
  encode_observation_record(encoder, record);
  return write_record(JournalRecordKind::Observation, ByteSpan(encoder.buffer().data(),
                                                                encoder.buffer().size()));
}

Result<void> Journal::append_restart_marker(RestartEpoch epoch, std::uint32_t restart_count,
                                            TimePoint wall_clock) {
  CanonicalEncoder encoder(64);
  encoder.tag("restart-marker");
  encoder.u8(1);
  encoder.u64(epoch.value());
  encoder.u32(restart_count);
  encoder.i64(wall_clock.nanos);
  return write_record(JournalRecordKind::RestartMarker,
                      ByteSpan(encoder.buffer().data(), encoder.buffer().size()));
}

Result<void> Journal::append_snapshot_marker(const SnapshotMarker& marker) {
  CanonicalEncoder encoder(96);
  encoder.tag("snapshot-marker");
  encoder.u8(1);
  encoder.bytes(as_bytes(marker.id.digest()));
  encoder.u64(marker.generation.value());
  encoder.u64(marker.epoch.value());
  encoder.i64(marker.published_at.nanos);
  return write_record(JournalRecordKind::SnapshotMarker,
                      ByteSpan(encoder.buffer().data(), encoder.buffer().size()));
}

Result<void> Journal::flush() {
  if (file_ == nullptr) {
    return Err(StatusCode::NotOpen, "journal is not open");
  }
  sync_file(file_);
  return VoidResult{};
}

Result<void> Journal::close() {
  if (file_ == nullptr) {
    return VoidResult{};
  }
  sync_file(file_);
  std::fclose(file_);
  file_ = nullptr;
  return VoidResult{};
}

Result<void> Journal::recover() {
  recovery_ = RecoveryReport{};
  recovery_.format_version = 0;
  recovery_.format_supported = false;

  std::vector<std::string> parts;
  for (std::uint32_t index = options_.max_files; index >= 1; --index) {
    const std::string candidate = part_path(index);
    if (std::FILE* probe = open_stream(candidate.c_str(), "rb"); probe != nullptr) {
      std::fclose(probe);
      parts.push_back(candidate);
    }
  }
  if (std::FILE* probe = open_stream(path_.c_str(), "rb"); probe != nullptr) {
    std::fclose(probe);
    parts.push_back(path_);
  }
  if (parts.empty()) {
    // Nothing on disk yet: the caller creates a fresh journal.
    chain_initialised_ = false;
    return VoidResult{};
  }

  std::array<std::uint8_t, 32> running_chain{};
  bool have_chain = false;
  bool stop = false;

  for (std::size_t part_index = 0; part_index < parts.size() && !stop; ++part_index) {
    const bool newest = part_index + 1 == parts.size();
    std::FILE* file = open_stream(parts[part_index].c_str(), "rb");
    if (file == nullptr) {
      recovery_.diagnostics.push_back(RecoveryDiagnostic{
          0, "part-unreadable", "journal part could not be opened for reading"});
      recovery_.corrupt = true;
      break;
    }
    ++recovery_.parts_recovered;

    std::array<std::uint8_t, kJournalHeaderBytes> header{};
    if (!read_exact(file, header.data(), header.size())) {
      recovery_.diagnostics.push_back(
          RecoveryDiagnostic{0, "header-truncated", "journal part is shorter than its header"});
      recovery_.corrupt = true;
      std::fclose(file);
      break;
    }
    if (std::memcmp(header.data(), kJournalMagic.data(), kJournalMagic.size()) != 0) {
      recovery_.diagnostics.push_back(
          RecoveryDiagnostic{0, "bad-magic", "journal header magic does not match"});
      recovery_.corrupt = true;
      std::fclose(file);
      break;
    }
    const std::uint32_t version = get_u32(header.data() + 8);
    const std::uint32_t declared_header = get_u32(header.data() + 12);
    recovery_.format_version = version;
    if (version != kJournalFormatVersion) {
      // A newer or older format is refused rather than guessed at.
      recovery_.format_supported = false;
      recovery_.diagnostics.push_back(RecoveryDiagnostic{
          0, "unsupported-format-version", "journal format version is not supported by this build"});
      std::fclose(file);
      return Err(StatusCode::VersionMismatch,
                 "journal format version is not supported by this build");
    }
    recovery_.format_supported = true;
    if (declared_header != kJournalHeaderBytes) {
      recovery_.diagnostics.push_back(
          RecoveryDiagnostic{0, "bad-header-size", "journal header size is not the expected size"});
      recovery_.corrupt = true;
      std::fclose(file);
      break;
    }
    const std::uint32_t stored_crc = get_u32(header.data() + kJournalHeaderBytes - 4);
    const std::uint32_t computed_crc = crc32c(ByteSpan(
        reinterpret_cast<const std::byte*>(header.data()), kJournalHeaderBytes - 4));
    if (stored_crc != computed_crc) {
      recovery_.diagnostics.push_back(
          RecoveryDiagnostic{0, "header-crc", "journal header checksum does not match"});
      recovery_.corrupt = true;
      std::fclose(file);
      break;
    }
    last_fabric_ =
        FabricId::from_u128(Uint128{get_u64(header.data() + 16), get_u64(header.data() + 24)});
    if (!options_.fabric.is_nil() && !(last_fabric_ == options_.fabric)) {
      recovery_.diagnostics.push_back(RecoveryDiagnostic{
          0, "fabric-mismatch", "journal was written for a different fabric identity"});
      std::fclose(file);
      return Err(StatusCode::FabricMismatch,
                 "journal was written for a different fabric identity");
    }

    std::array<std::uint8_t, 32> chain{};
    std::memcpy(chain.data(), header.data() + 40, chain.size());
    if (!have_chain) {
      running_chain = chain;
      have_chain = true;
    } else if (running_chain != chain) {
      recovery_.diagnostics.push_back(RecoveryDiagnostic{
          0, "part-chain-mismatch", "journal part chain seed does not continue the previous part"});
      recovery_.corrupt = true;
      std::fclose(file);
      break;
    }

    std::uint64_t offset = kJournalHeaderBytes;
    while (!stop) {
      std::array<std::uint8_t, kJournalRecordHeaderBytes> record_header{};
      const std::size_t read = std::fread(record_header.data(), 1, record_header.size(), file);
      if (read == 0) {
        break;  // clean end of part
      }
      if (read != record_header.size()) {
        // The unusable tail is everything from the start of the incomplete
        // record to the end of the part, not just the bytes that were read.
        const std::uint64_t part_size = file_size(file);
        recovery_.truncated_tail_bytes +=
            part_size > offset ? part_size - offset : static_cast<std::uint64_t>(read);
        recovery_.diagnostics.push_back(RecoveryDiagnostic{
            offset, "record-header-truncated", "journal ends inside a record header"});
        recovery_.truncated = true;
        recovery_.corrupt = recovery_.corrupt || !newest;
        stop = true;
        break;
      }
      const std::uint32_t payload_len = get_u32(record_header.data());
      const std::uint32_t payload_crc = get_u32(record_header.data() + 4);
      const auto kind = static_cast<JournalRecordKind>(record_header[8]);
      const std::uint64_t sequence = get_u64(record_header.data() + 12);
      std::array<std::uint8_t, 32> previous{};
      std::memcpy(previous.data(), record_header.data() + 20, previous.size());
      std::array<std::uint8_t, 32> content{};
      std::memcpy(content.data(), record_header.data() + 52, content.size());
      std::array<std::uint8_t, 32> stored_chain{};
      std::memcpy(stored_chain.data(), record_header.data() + 84, stored_chain.size());

      if (payload_len == 0 || payload_len > options_.max_record_bytes) {
        recovery_.diagnostics.push_back(RecoveryDiagnostic{
            offset, "record-length", "record payload length is outside the configured bounds"});
        recovery_.corrupt = true;
        stop = true;
        break;
      }
      if (previous != running_chain) {
        recovery_.diagnostics.push_back(RecoveryDiagnostic{
            offset, "chain-link", "record does not continue the journal hash chain"});
        recovery_.corrupt = true;
        stop = true;
        break;
      }

      std::vector<std::byte> payload(payload_len);
      const std::size_t payload_read = std::fread(payload.data(), 1, payload.size(), file);
      if (payload_read != payload.size()) {
        const std::uint64_t part_size = file_size(file);
        recovery_.truncated_tail_bytes +=
            part_size > offset ? part_size - offset : static_cast<std::uint64_t>(payload_read);
        recovery_.diagnostics.push_back(RecoveryDiagnostic{
            offset, "record-payload-truncated", "journal ends inside a record payload"});
        recovery_.truncated = true;
        recovery_.corrupt = recovery_.corrupt || !newest;
        stop = true;
        break;
      }

      ++recovery_.records_read;
      const ByteSpan payload_span(payload.data(), payload.size());
      if (crc32c(payload_span) != payload_crc) {
        recovery_.diagnostics.push_back(
            RecoveryDiagnostic{offset, "record-crc", "record payload checksum does not match"});
        recovery_.corrupt = true;
        stop = true;
        break;
      }
      const std::array<std::uint8_t, 32> computed_content = sha256(payload_span).bytes;
      if (computed_content != content) {
        recovery_.diagnostics.push_back(RecoveryDiagnostic{
            offset, "record-content-digest", "record payload digest does not match"});
        recovery_.corrupt = true;
        stop = true;
        break;
      }
      const std::array<std::uint8_t, 32> computed_chain =
          compute_chain(previous, content, kind, sequence);
      if (computed_chain != stored_chain) {
        recovery_.diagnostics.push_back(
            RecoveryDiagnostic{offset, "record-chain", "record chain value is not self consistent"});
        recovery_.corrupt = true;
        stop = true;
        break;
      }
      running_chain = computed_chain;
      chain_ = running_chain;
      chain_initialised_ = true;

      switch (kind) {
        case JournalRecordKind::Observation: {
          Result<ObservationRecord> record =
              decode_observation_record(payload_span, options_.limits);
          if (!record) {
            ++recovery_.records_rejected;
            recovery_.diagnostics.push_back(RecoveryDiagnostic{
                offset, "record-invalid", record.status().message()});
            break;
          }
          if (recovered_records_.size() >= options_.max_recovered_observations) {
            recovery_.diagnostics.push_back(RecoveryDiagnostic{
                offset, "recovery-bound", "recovered observation bound reached"});
            recovery_.truncated = true;
            stop = true;
            break;
          }
          recovered_records_.push_back(std::move(*record));
          ++recovery_.records_accepted;
          break;
        }
        case JournalRecordKind::RestartMarker: {
          CanonicalDecoder decoder(payload_span);
          Result<void> tag = decoder.tag("restart-marker");
          if (!tag) {
            ++recovery_.records_rejected;
            recovery_.diagnostics.push_back(
                RecoveryDiagnostic{offset, "restart-marker-invalid", tag.status().message()});
            break;
          }
          Result<void> revision = decoder.expect_u8(1);
          Result<std::uint64_t> epoch = decoder.u64();
          Result<std::uint32_t> count = decoder.u32();
          if (!revision || !epoch || !count) {
            ++recovery_.records_rejected;
            recovery_.diagnostics.push_back(RecoveryDiagnostic{
                offset, "restart-marker-invalid", "restart marker payload is malformed"});
            break;
          }
          ++recovery_.restart_markers;
          recovered_restart_count_ = *count;
          recovered_restart_epoch_ = RestartEpoch(*epoch);
          break;
        }
        case JournalRecordKind::SnapshotMarker: {
          CanonicalDecoder decoder(payload_span);
          Result<void> tag = decoder.tag("snapshot-marker");
          if (!tag) {
            ++recovery_.records_rejected;
            recovery_.diagnostics.push_back(
                RecoveryDiagnostic{offset, "snapshot-marker-invalid", tag.status().message()});
            break;
          }
          Result<void> revision = decoder.expect_u8(1);
          Result<ByteSpan> digest = decoder.bytes();
          Result<std::uint64_t> generation = decoder.u64();
          Result<std::uint64_t> epoch = decoder.u64();
          Result<std::int64_t> published_at = decoder.i64();
          if (!revision || !digest || !generation || !epoch || !published_at) {
            ++recovery_.records_rejected;
            recovery_.diagnostics.push_back(RecoveryDiagnostic{
                offset, "snapshot-marker-invalid", "snapshot marker payload is malformed"});
            break;
          }
          SnapshotMarker marker;
          if (digest->size() != marker.id.digest().bytes.size()) {
            ++recovery_.records_rejected;
            recovery_.diagnostics.push_back(RecoveryDiagnostic{
                offset, "snapshot-marker-invalid", "snapshot marker digest is not 32 bytes"});
            break;
          }
          Digest256 raw{};
          std::memcpy(raw.bytes.data(), digest->data(), 32);
          marker.id = SnapshotId(raw);
          marker.generation = GenerationId(*generation);
          marker.epoch = EpochId(*epoch);
          marker.published_at = TimePoint{*published_at};
          recovered_snapshots_.push_back(marker);
          ++recovery_.snapshot_markers;
          break;
        }
        default:
          ++recovery_.records_rejected;
          recovery_.diagnostics.push_back(RecoveryDiagnostic{
              offset, "unknown-record-kind", "record kind is not known to this build"});
          break;
      }
      recovery_.records_accepted = static_cast<std::uint64_t>(recovered_records_.size());
      offset += kJournalRecordHeaderBytes + payload_len;
    }
    recovery_.file_bytes += file_size(file);
    std::fclose(file);
  }

  recovery_.chain_verified = recovery_.corrupt == false;
  return VoidResult{};
}

Result<std::uint64_t> Journal::compact(const std::string& target_path) const {
  if (target_path.empty()) {
    return Err<std::uint64_t>(StatusCode::InvalidArgument, "compaction target must not be empty");
  }
  std::FILE* target = open_stream(target_path.c_str(), "wb+");
  if (target == nullptr) {
    return Err<std::uint64_t>(StatusCode::IoError, "cannot create the compaction target");
  }

  // The target is a complete journal part: header first, then records whose
  // hash chain starts from that header's seed.
  std::array<std::uint8_t, kJournalHeaderBytes> file_header{};
  std::memcpy(file_header.data(), kJournalMagic.data(), kJournalMagic.size());
  put_u32(file_header.data() + 8, kJournalFormatVersion);
  put_u32(file_header.data() + 12, kJournalHeaderBytes);
  put_u64(file_header.data() + 16, options_.fabric.value().hi);
  put_u64(file_header.data() + 24, options_.fabric.value().lo);
  std::array<std::uint8_t, 32> chain = chain_seed(options_.fabric);
  std::memcpy(file_header.data() + 40, chain.data(), chain.size());
  const std::uint32_t header_crc = crc32c(ByteSpan(
      reinterpret_cast<const std::byte*>(file_header.data()), kJournalHeaderBytes - 4));
  put_u32(file_header.data() + kJournalHeaderBytes - 4, header_crc);
  if (!write_all(target, file_header.data(), file_header.size())) {
    std::fclose(target);
    return Err<std::uint64_t>(StatusCode::IoError, "cannot write the compaction target header");
  }

  std::uint64_t written = kJournalHeaderBytes;
  std::uint64_t sequence = 0;
  for (const ObservationRecord& record : recovered_records_) {
    CanonicalEncoder encoder(256 + record.observation.claims.size() * 96);
    encode_observation_record(encoder, record);
    const ByteSpan payload(encoder.buffer().data(), encoder.buffer().size());
    if (payload.size() > options_.max_record_bytes) {
      std::fclose(target);
      return Err<std::uint64_t>(StatusCode::TooLarge,
                                "a recovered record exceeds the configured maximum record size");
    }
    const std::array<std::uint8_t, 32> content = sha256(payload).bytes;
    const std::array<std::uint8_t, 32> previous = chain;
    ++sequence;
    const std::array<std::uint8_t, 32> next =
        compute_chain(previous, content, JournalRecordKind::Observation, sequence);
    std::array<std::uint8_t, kJournalRecordHeaderBytes> header{};
    put_u32(header.data(), static_cast<std::uint32_t>(payload.size()));
    put_u32(header.data() + 4, crc32c(payload));
    header[8] = static_cast<std::uint8_t>(JournalRecordKind::Observation);
    put_u64(header.data() + 12, sequence);
    std::memcpy(header.data() + 20, previous.data(), previous.size());
    std::memcpy(header.data() + 52, content.data(), content.size());
    std::memcpy(header.data() + 84, next.data(), next.size());
    if (!write_all(target, header.data(), header.size()) ||
        !write_all(target, payload.data(), payload.size())) {
      std::fclose(target);
      return Err<std::uint64_t>(StatusCode::IoError, "cannot write the compaction target");
    }
    chain = next;
    written += kJournalRecordHeaderBytes + payload.size();
  }
  sync_file(target);
  std::fclose(target);
  return Ok(written);
}

}  // namespace fabric_observatory
