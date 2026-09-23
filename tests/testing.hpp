// Fabric Observatory test support.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small test harness. It has no dependencies, it runs test cases
// in a deterministic order, and it uses no timeouts anywhere: a test either
// finishes or the process does not return, and that is treated as a defect
// rather than hidden behind a timer.

#ifndef FABRIC_OBSERVATORY_TESTING_HPP
#define FABRIC_OBSERVATORY_TESTING_HPP

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace fabobs_test {

struct TestCase {
  const char* suite;
  const char* name;
  bool slow;
  void (*function)();
};

struct Failure {
  std::string suite;
  std::string test;
  std::string file;
  int line;
  std::string message;
};

std::vector<TestCase>& registry();
std::vector<Failure>& failures();
std::uint64_t& check_count();

struct Registrar {
  Registrar(const char* suite, const char* name, bool slow, void (*function)());
};

// Indirection that keeps the assertion macros from tripping the "conditional
// expression is constant" diagnostic when both operands happen to be constant.
[[nodiscard]] inline bool assertion_failed(bool condition) noexcept { return !condition; }

int run_all(int argc, char** argv);

template <class T>
concept Streamable = requires(std::ostream& stream, const T& value) { stream << value; };

// Rendering used by the equality assertion. Library types that are not
// streamable expose their canonical text, so a failing assertion is readable
// rather than a compile error.
template <class T>
std::string to_display(const T& value) {
  if constexpr (Streamable<T>) {
    std::ostringstream out;
    out << value;
    return out.str();
  } else if constexpr (requires { to_string(value); }) {
    return std::string(to_string(value));
  } else if constexpr (std::is_enum_v<T>) {
    return "enum(" + std::to_string(static_cast<long long>(value)) + ")";
  } else if constexpr (requires { value.to_text(); }) {
    return value.to_text();
  } else if constexpr (requires { value.to_hex(); }) {
    return value.to_hex();
  } else {
    return "<value>";
  }
}

// Set by the harness before each test body runs; the assertion macros use them
// to attribute failures to the right test.
extern const char* fabobs_test_current_suite;
extern const char* fabobs_test_current_name;

}  // namespace fabobs_test

#define FABOBS_TEST_IMPL(suite, name, slow)                                            \
  static void fabobs_test_body_##suite##_##name();                                     \
  static const ::fabobs_test::Registrar fabobs_test_registrar_##suite##_##name(        \
      #suite, #name, slow, &fabobs_test_body_##suite##_##name);                        \
  static void fabobs_test_body_##suite##_##name()

#define FABOBS_TEST(suite, name) FABOBS_TEST_IMPL(suite, name, false)
#define FABOBS_SLOW_TEST(suite, name) FABOBS_TEST_IMPL(suite, name, true)

#define FABOBS_FAIL(message)                                                          \
  ::fabobs_test::failures().push_back(::fabobs_test::Failure{                         \
      ::fabobs_test::fabobs_test_current_suite, ::fabobs_test::fabobs_test_current_name, \
      __FILE__, __LINE__, (message)})

#define FABOBS_CHECK(expression)                                                      \
  do {                                                                                \
    ++::fabobs_test::check_count();                                                   \
    if (::fabobs_test::assertion_failed(static_cast<bool>(expression))) {              \
      FABOBS_FAIL(std::string("CHECK failed: ") + #expression);                       \
    }                                                                                 \
  } while (false)

#define FABOBS_REQUIRE(expression)                                                    \
  do {                                                                                \
    ++::fabobs_test::check_count();                                                   \
    if (::fabobs_test::assertion_failed(static_cast<bool>(expression))) {              \
      FABOBS_FAIL(std::string("REQUIRE failed: ") + #expression);                     \
      return;                                                                         \
    }                                                                                 \
  } while (false)

// The operands are copied rather than bound by reference: an expression such as
// build()->id() returns a reference into a temporary that dies at the end of the
// initialising full-expression, and binding it would make a failing assertion
// read freed memory instead of reporting the difference.
#define FABOBS_CHECK_EQ(actual, expected)                                             \
  do {                                                                                \
    ++::fabobs_test::check_count();                                                   \
    const auto fabobs_check_actual = (actual);                                        \
    const auto fabobs_check_expected = (expected);                                   \
    if (::fabobs_test::assertion_failed(fabobs_check_actual == fabobs_check_expected)) { \
      FABOBS_FAIL(std::string("CHECK_EQ failed: ") + #actual + " == " + #expected +   \
                  " (actual: " + ::fabobs_test::to_display(fabobs_check_actual) +     \
                  ", expected: " + ::fabobs_test::to_display(fabobs_check_expected) + \
                  ")");                                                               \
    }                                                                                 \
  } while (false)

// Like CHECK_EQ, but stops the test when it fails. Use it for the assertion
// that guards an index, so a wrong count cannot turn into out-of-range access.
#define FABOBS_REQUIRE_EQ(actual, expected)                                             do {                                                                                    ++::fabobs_test::check_count();                                                       const auto fabobs_require_actual = (actual);                                          const auto fabobs_require_expected = (expected);                                      if (::fabobs_test::assertion_failed(fabobs_require_actual == fabobs_require_expected)) { \
      FABOBS_FAIL(std::string("REQUIRE_EQ failed: ") + #actual + " == " + #expected +                    " (actual: " + ::fabobs_test::to_display(fabobs_require_actual) +                     ", expected: " + ::fabobs_test::to_display(fabobs_require_expected) + \
                  ")");                                                                     return;                                                                             }                                                                                   } while (false)

#define FABOBS_CHECK_MSG(expression, message)                                         \
  do {                                                                                \
    ++::fabobs_test::check_count();                                                   \
    if (::fabobs_test::assertion_failed(static_cast<bool>(expression))) {              \
      FABOBS_FAIL(std::string("CHECK failed: ") + #expression + ": " + (message));    \
    }                                                                                 \
  } while (false)

#endif  // FABRIC_OBSERVATORY_TESTING_HPP
