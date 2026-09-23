// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_EXPORT_HPP
#define FABRIC_OBSERVATORY_EXPORT_HPP

#include <cstdint>

// Fabric Observatory is delivered as a static library; the export macro exists
// so that downstream packaging can be adjusted without touching declarations.
#ifndef FABRIC_OBSERVATORY_API
#define FABRIC_OBSERVATORY_API
#endif

// First-party code never throws. Contract violations are loud, immediate and
// unconditional: a violated internal invariant is not a recoverable condition.
namespace fabric_observatory {

[[noreturn]] FABRIC_OBSERVATORY_API void fail_fast(const char* expression,
                                                   const char* file,
                                                   int line,
                                                   const char* message) noexcept;

}  // namespace fabric_observatory

#define FABRIC_OBSERVATORY_CONTRACT(expr)                                            \
  do {                                                                               \
    if (!(expr)) {                                                                   \
      ::fabric_observatory::fail_fast(#expr, __FILE__, __LINE__, "contract violated"); \
    }                                                                                \
  } while (false)

#define FABRIC_OBSERVATORY_CONTRACT_MSG(expr, message)                               \
  do {                                                                               \
    if (!(expr)) {                                                                   \
      ::fabric_observatory::fail_fast(#expr, __FILE__, __LINE__, (message));         \
    }                                                                                \
  } while (false)

#define FABRIC_OBSERVATORY_UNREACHABLE(message)                                      \
  ::fabric_observatory::fail_fast("unreachable", __FILE__, __LINE__, (message))

#endif  // FABRIC_OBSERVATORY_EXPORT_HPP
