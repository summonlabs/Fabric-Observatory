// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#ifndef FABRIC_OBSERVATORY_UTIL_HPP
#define FABRIC_OBSERVATORY_UTIL_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fabric_observatory/digest.hpp"
#include "fabric_observatory/status.hpp"

namespace fabric_observatory::util {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept;
[[nodiscard]] bool starts_with(std::string_view text, std::string_view prefix) noexcept;
[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept;
[[nodiscard]] std::string to_lower_ascii(std::string_view text);
[[nodiscard]] std::string hex_encode(ByteSpan data);
[[nodiscard]] Result<std::vector<std::byte>> hex_decode(std::string_view text);
[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);

// Identifiers accepted on the wire: lower-case ASCII letters, digits and the
// separators '.', '_' and '-'. Bounded in length by the caller.
[[nodiscard]] bool is_valid_identifier(std::string_view text, std::size_t max_length) noexcept;
[[nodiscard]] bool is_valid_authority_name(std::string_view text) noexcept;

}  // namespace fabric_observatory::util

#endif  // FABRIC_OBSERVATORY_UTIL_HPP
