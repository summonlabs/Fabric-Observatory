// Fabric Observatory test harness entry point.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "testing.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace fabobs_test {

const char* fabobs_test_current_suite = "";
const char* fabobs_test_current_name = "";

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

std::vector<Failure>& failures() {
  static std::vector<Failure> collected;
  return collected;
}

std::uint64_t& check_count() {
  static std::uint64_t count = 0;
  return count;
}

Registrar::Registrar(const char* suite, const char* name, bool slow, void (*function)()) {
  registry().push_back(TestCase{suite, name, slow, function});
}

int run_all(int argc, char** argv) {
  std::string filter;
  bool list_only = false;
  bool include_slow = true;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--fast") {
      include_slow = false;
    } else if (argument.rfind("--filter=", 0) == 0) {
      filter = argument.substr(9);
    } else {
      std::fprintf(stderr, "unknown test option: %s\n", argument.c_str());
      return 2;
    }
  }

  std::vector<TestCase> ordered = registry();
  std::stable_sort(ordered.begin(), ordered.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (std::strcmp(lhs.suite, rhs.suite) != 0) {
      return std::strcmp(lhs.suite, rhs.suite) < 0;
    }
    return std::strcmp(lhs.name, rhs.name) < 0;
  });

  if (list_only) {
    for (const TestCase& test : ordered) {
      std::printf("%s.%s%s\n", test.suite, test.name, test.slow ? " (slow)" : "");
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed = 0;
  std::string current_suite;
  for (const TestCase& test : ordered) {
    if (test.slow && !include_slow) {
      continue;
    }
    if (!filter.empty()) {
      const std::string full = std::string(test.suite) + "." + test.name;
      if (full.find(filter) == std::string::npos) {
        continue;
      }
    }
    if (current_suite != test.suite) {
      current_suite = test.suite;
      std::printf("[suite] %s\n", current_suite.c_str());
    }
    const std::size_t before = failures().size();
    fabobs_test_current_suite = test.suite;
    fabobs_test_current_name = test.name;
    test.function();
    ++executed;
    const std::size_t produced = failures().size() - before;
    if (produced == 0) {
      std::printf("  ok   %s\n", test.name);
    } else {
      ++failed;
      std::printf("  FAIL %s (%zu failure(s))\n", test.name, produced);
    }
    std::fflush(stdout);
  }

  if (!failures().empty()) {
    std::printf("\nfailures:\n");
    for (const Failure& failure : failures()) {
      std::printf("  %s.%s %s:%d: %s\n", failure.suite.c_str(), failure.test.c_str(),
                  failure.file.c_str(), failure.line, failure.message.c_str());
    }
  }
  std::printf("\ntests=%zu failed=%zu assertions=%llu\n", executed, failed,
              static_cast<unsigned long long>(check_count()));
  return failed == 0 ? 0 : 1;
}

}  // namespace fabobs_test

int main(int argc, char** argv) { return fabobs_test::run_all(argc, argv); }
