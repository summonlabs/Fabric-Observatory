// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_TRANSPORT_HPP
#define FABRIC_OBSERVATORY_TRANSPORT_HPP

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

#include "fabric_observatory/json.hpp"
#include "fabric_observatory/observatory.hpp"

// Newline delimited JSON request/response protocol over any byte stream. The
// runtime uses it for the command line tool's stdio mode and for the
// independent-process transport test: one complete JSON request per line, one
// complete JSON response per line, no framing beyond the newline, no
// interleaving.
//
// Requests:
//   {"op":"ping"}
//   {"op":"ingest","observation":{...}}
//   {"op":"ingest_batch","observations":[...]}
//   {"op":"snapshot"}
//   {"op":"refresh","at":"<nanoseconds since the epoch>"}
//   {"op":"query","filter":{...}}
//   {"op":"hierarchy","query":{...}}
//   {"op":"diff","before":"<snapshot id>","after":"<snapshot id>"}
//   {"op":"diff","lookback":"<n>"}
//   {"op":"history","max":"<n>"}
//   {"op":"explain","subject":"<typed identity>","aspect":"<aspect>"}
//   {"op":"sources"}
//   {"op":"stats"}
//   {"op":"recovery"}
//   {"op":"summary"}
//   {"op":"policy"}
//   {"op":"shutdown"}
//
// Responses:
//   {"ok":true,"op":"<op>","result":{...}}
//   {"ok":false,"op":"<op>","code":"<StatusCode>","message":"..."}

namespace fabric_observatory {

struct TransportOptions {
  std::size_t max_line_bytes{1u << 21};
  bool pretty{false};
  bool include_claims{true};
  JsonLimits json{};
};

// Handles exactly one request line and produces exactly one response line. The
// returned string never contains a newline character.
std::string handle_request_line(Observatory& observatory, std::string_view line,
                                const TransportOptions& options);

struct TransportStats {
  std::uint64_t requests{0};
  std::uint64_t failures{0};
  std::uint64_t oversized{0};
  bool shutdown_requested{false};

  friend bool operator==(const TransportStats&, const TransportStats&) = default;
};

// Serves the protocol until end of input or a shutdown request. Every response
// is flushed before the next request is read, so the client always sees the
// effects of its previous request.
int run_stdio_server(Observatory& observatory, std::istream& in, std::ostream& out,
                     const TransportOptions& options, TransportStats* stats);

}  // namespace fabric_observatory

#endif  // FABRIC_OBSERVATORY_TRANSPORT_HPP
