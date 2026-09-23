// Fabric Observatory - vendor-neutral Fabric OS observational runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "fabric_observatory/time.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <limits>

namespace fabric_observatory {

namespace {

constexpr std::int64_t kNsPerSecond = 1000000000;
constexpr std::int64_t kNsPerDay = 86400 * kNsPerSecond;

// Floor division and modulus for signed values, so that times before the epoch
// format correctly instead of rounding towards zero.
constexpr std::int64_t floor_div(std::int64_t numerator, std::int64_t denominator) noexcept {
  const std::int64_t quotient = numerator / denominator;
  const std::int64_t remainder = numerator % denominator;
  return (remainder != 0 && ((remainder < 0) != (denominator < 0))) ? quotient - 1 : quotient;
}

constexpr std::int64_t floor_mod(std::int64_t numerator, std::int64_t denominator) noexcept {
  const std::int64_t remainder = numerator % denominator;
  return (remainder != 0 && ((remainder < 0) != (denominator < 0))) ? remainder + denominator
                                                                   : remainder;
}

struct CivilDate {
  std::int64_t year;
  unsigned month;
  unsigned day;
};

// Howard Hinnant's civil-from-days algorithm; valid for the whole range of
// std::int64_t days that a nanosecond timestamp can address.
constexpr CivilDate civil_from_days(std::int64_t days) noexcept {
  days += 719468;
  const std::int64_t era = floor_div(days, 146097);
  const std::int64_t day_of_era = days - era * 146097;
  const std::int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const std::int64_t year = year_of_era + era * 400;
  const std::int64_t day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const std::int64_t month_prime = (5 * day_of_year + 2) / 153;
  const std::int64_t day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  const std::int64_t month = month_prime + (month_prime < 10 ? 3 : -9);
  return CivilDate{year + (month <= 2 ? 1 : 0), static_cast<unsigned>(month), static_cast<unsigned>(day)};
}

constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2 ? 1 : 0;
  const std::int64_t era = floor_div(year, 400);
  const std::int64_t year_of_era = year - era * 400;
  const std::int64_t month_prime = static_cast<std::int64_t>(month) + (month > 2 ? -3 : 9);
  const std::int64_t day_of_year = (153 * month_prime + 2) / 5 + static_cast<std::int64_t>(day) - 1;
  const std::int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

bool all_digits(std::string_view text) noexcept {
  if (text.empty()) {
    return false;
  }
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  return true;
}

Result<std::int64_t> parse_unsigned(std::string_view text, std::size_t digits) {
  if (text.size() != digits || !all_digits(text)) {
    return Err<std::int64_t>(StatusCode::MalformedInput, "expected a fixed-width decimal field");
  }
  std::int64_t value = 0;
  for (const char ch : text) {
    value = value * 10 + (ch - '0');
  }
  return Ok(value);
}

}  // namespace

TimePoint SystemClock::now() const {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  return TimePoint{static_cast<std::int64_t>(nanos)};
}

std::string format_utc(TimePoint point) {
  const std::int64_t days = floor_div(point.nanos, kNsPerDay);
  const std::int64_t within_day = floor_mod(point.nanos, kNsPerDay);
  const std::int64_t seconds_of_day = within_day / kNsPerSecond;
  const std::int64_t nanos_of_second = within_day % kNsPerSecond;
  const CivilDate date = civil_from_days(days);

  std::array<char, 64> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(),
                                    "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%09lldZ",
                                    static_cast<long long>(date.year),
                                    date.month,
                                    date.day,
                                    static_cast<long long>(seconds_of_day / 3600),
                                    static_cast<long long>((seconds_of_day / 60) % 60),
                                    static_cast<long long>(seconds_of_day % 60),
                                    static_cast<long long>(nanos_of_second));
  if (written <= 0) {
    return std::string("1970-01-01T00:00:00.000000000Z");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

Result<TimePoint> parse_utc(std::string_view text) {
  // A bare signed integer is interpreted as nanoseconds since the epoch.
  if (!text.empty() && (all_digits(text) || (text[0] == '-' && all_digits(text.substr(1))))) {
    std::int64_t value = 0;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
      negative = true;
      index = 1;
    }
    for (; index < text.size(); ++index) {
      const std::int64_t digit = text[index] - '0';
      if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
        return Err<TimePoint>(StatusCode::OutOfRange, "nanosecond timestamp does not fit in int64");
      }
      value = value * 10 + digit;
    }
    return Ok(TimePoint{negative ? -value : value});
  }

  if (text.size() < 20 || text.back() != 'Z' || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != ' ')) {
    return Err<TimePoint>(StatusCode::MalformedInput,
                          "expected UTC form YYYY-MM-DDTHH:MM:SS[.fffffffff]Z");
  }

  Result<std::int64_t> year = parse_unsigned(text.substr(0, 4), 4);
  Result<std::int64_t> month = parse_unsigned(text.substr(5, 2), 2);
  Result<std::int64_t> day = parse_unsigned(text.substr(8, 2), 2);
  Result<std::int64_t> hour = parse_unsigned(text.substr(11, 2), 2);
  Result<std::int64_t> minute = parse_unsigned(text.substr(14, 2), 2);
  Result<std::int64_t> second = parse_unsigned(text.substr(17, 2), 2);
  if (!year || !month || !day || !hour || !minute || !second) {
    return Err<TimePoint>(StatusCode::MalformedInput, "UTC timestamp contains a malformed field");
  }
  if (*month < 1 || *month > 12 || *day < 1 || *day > 31 || *hour > 23 || *minute > 59 ||
      *second > 60) {
    return Err<TimePoint>(StatusCode::OutOfRange, "UTC timestamp field is out of range");
  }

  std::int64_t fraction = 0;
  std::size_t digits = 0;
  if (text.size() > 20) {
    if (text[19] != '.') {
      return Err<TimePoint>(StatusCode::MalformedInput, "expected '.' before fractional seconds");
    }
    const std::string_view fraction_text = text.substr(20, text.size() - 21);
    if (fraction_text.empty() || fraction_text.size() > 9 || !all_digits(fraction_text)) {
      return Err<TimePoint>(StatusCode::MalformedInput,
                            "fractional seconds must be 1 to 9 digits");
    }
    for (const char ch : fraction_text) {
      fraction = fraction * 10 + (ch - '0');
    }
    digits = fraction_text.size();
  } else if (text.size() != 20) {
    return Err<TimePoint>(StatusCode::MalformedInput, "UTC timestamp has an unexpected length");
  }

  for (std::size_t pad = digits; pad < 9; ++pad) {
    fraction *= 10;
  }

  const std::int64_t days = days_from_civil(*year, static_cast<unsigned>(*month),
                                            static_cast<unsigned>(*day));
  const std::int64_t seconds = days * 86400 + *hour * 3600 + *minute * 60 + *second;
  return Ok(TimePoint{seconds * kNsPerSecond + fraction});
}

}  // namespace fabric_observatory
