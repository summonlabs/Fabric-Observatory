// Fabric Observatory example: the newline delimited JSON protocol.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The same protocol the runtime speaks over a pipe to an independent process is
// available in process, which is what this example drives.

#include "fabric_observatory/transport.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace fabric_observatory;

int main() {
  auto clock = std::make_shared<ManualClock>(TimePoint{3000000000000});
  ObservatoryConfig config;
  config.fabric = FabricId::derive("fabric/example");
  config.clock = clock;

  Result<std::unique_ptr<Observatory>> opened = Observatory::open(config);
  if (!opened) {
    std::fprintf(stderr, "example: %s\n", opened.status().to_string().c_str());
    return 1;
  }

  const std::string observation =
      "{\"schema\":\"fabric-observatory/observation/1\","
      "\"fabric\":\"" + config.fabric.to_text() + "\","
      "\"source\":\"" + SourceId::derive("source/bridge").to_text() + "\","
      "\"incarnation\":\"" + IncarnationId::derive("bridge/boot-1").to_text() + "\","
      "\"sequence\":\"1\",\"generation\":\"1\",\"epoch\":\"1\",\"observed_at\":\"" +
      std::to_string(clock->now().nanos) + "\","
      "\"claims\":[{\"subject\":\"" +
      SubjectIdentity::of(PortId(DeviceId::derive("device/leaf-1"), 3)).typed_text() +
      "\",\"aspect\":\"port.state\",\"value\":{\"k\":\"text\",\"v\":\"up\"}}]}";

  const std::vector<std::string> requests = {
      "{\"op\":\"ping\"}",
      "{\"op\":\"ingest\",\"observation\":" + observation + "}",
      "{\"op\":\"snapshot\"}",
      "{\"op\":\"sources\"}",
      "{\"op\":\"diff\",\"lookback\":\"1\"}",
  };

  TransportOptions options;
  options.pretty = false;
  for (const std::string& request : requests) {
    const std::string response = handle_request_line(**opened, request, options);
    std::printf("%s\n", response.c_str());
  }
  return 0;
}
