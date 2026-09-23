// Test helper: run the built tool as an independent process.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The runtime claims behaviour across a process boundary - the newline
// delimited JSON protocol over stdio - so the tests exercise it with a real
// child process and real pipes rather than an in-process stand-in.

#ifndef FABRIC_OBSERVATORY_TESTS_PROCESS_RUNNER_HPP
#define FABRIC_OBSERVATORY_TESTS_PROCESS_RUNNER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fabobs_test {

#if defined(_WIN32)

// A child process with piped stdin and stdout. Reading and writing are
// line oriented because that is the protocol, and every call blocks until the
// bytes are available: the tests never sleep and never time out.
class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept { move_from(other); }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      close();
      move_from(other);
    }
    return *this;
  }

  ~ChildProcess() { close(); }

  static std::optional<ChildProcess> start(const std::string& command_line) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    if (CreatePipe(&child_stdin_read, &child_stdin_write, &attributes, 0) == 0) {
      return std::nullopt;
    }
    if (CreatePipe(&child_stdout_read, &child_stdout_write, &attributes, 0) == 0) {
      CloseHandle(child_stdin_read);
      CloseHandle(child_stdin_write);
      return std::nullopt;
    }
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stdout_write;

    PROCESS_INFORMATION process{};
    std::vector<char> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back('\0');
    const BOOL created =
        CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    if (created == 0) {
      CloseHandle(child_stdin_write);
      CloseHandle(child_stdout_read);
      return std::nullopt;
    }

    ChildProcess child;
    child.process_ = process.hProcess;
    child.thread_ = process.hThread;
    child.stdin_ = child_stdin_write;
    child.stdout_ = child_stdout_read;
    return child;
  }

  bool write_bytes(const std::string& data) {
    if (stdin_ == nullptr) {
      return false;
    }
    std::size_t written = 0;
    while (written < data.size()) {
      DWORD chunk = 0;
      const DWORD remaining = static_cast<DWORD>(data.size() - written);
      if (WriteFile(stdin_, data.data() + written, remaining, &chunk, nullptr) == 0) {
        return false;
      }
      written += chunk;
    }
    return true;
  }

  bool write_line(const std::string& line) { return write_bytes(line + "\n"); }

  void close_stdin() {
    if (stdin_ != nullptr) {
      CloseHandle(stdin_);
      stdin_ = nullptr;
    }
  }

  std::optional<std::string> read_line() {
    std::string line;
    char byte = 0;
    while (true) {
      DWORD read = 0;
      if (ReadFile(stdout_, &byte, 1, &read, nullptr) == 0 || read == 0) {
        if (line.empty()) {
          return std::nullopt;
        }
        return line;
      }
      if (byte == '\n') {
        return line;
      }
      if (byte != '\r') {
        line.push_back(byte);
      }
    }
  }

  std::string read_remaining() {
    std::string output;
    char buffer[4096];
    while (true) {
      DWORD read = 0;
      if (ReadFile(stdout_, buffer, sizeof(buffer), &read, nullptr) == 0 || read == 0) {
        break;
      }
      output.append(buffer, read);
    }
    return output;
  }

  int wait() {
    if (process_ == nullptr) {
      return -1;
    }
    const DWORD status = WaitForSingleObject(process_, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process_, &code);
    return status == WAIT_OBJECT_0 ? static_cast<int>(code) : -1;
  }

 private:
  void move_from(ChildProcess& other) {
    process_ = other.process_;
    thread_ = other.thread_;
    stdin_ = other.stdin_;
    stdout_ = other.stdout_;
    other.process_ = nullptr;
    other.thread_ = nullptr;
    other.stdin_ = nullptr;
    other.stdout_ = nullptr;
  }

  void close() {
    close_stdin();
    if (stdout_ != nullptr) {
      CloseHandle(stdout_);
      stdout_ = nullptr;
    }
    if (thread_ != nullptr) {
      CloseHandle(thread_);
      thread_ = nullptr;
    }
    if (process_ != nullptr) {
      CloseHandle(process_);
      process_ = nullptr;
    }
  }

  HANDLE process_{nullptr};
  HANDLE thread_{nullptr};
  HANDLE stdin_{nullptr};
  HANDLE stdout_{nullptr};
};

struct ProcessResult {
  bool started{false};
  int exit_code{-1};
  std::string output{};
};

inline ProcessResult run_process(const std::string& command_line, const std::string& input = {}) {
  ProcessResult result;
  std::optional<ChildProcess> child = ChildProcess::start(command_line);
  if (!child.has_value()) {
    return result;
  }
  result.started = true;
  if (!input.empty()) {
    child->write_bytes(input);
  }
  child->close_stdin();
  result.output = child->read_remaining();
  result.exit_code = child->wait();
  return result;
}

inline std::string quote(const std::string& value) { return "\"" + value + "\""; }

#endif  // _WIN32

}  // namespace fabobs_test

#endif  // FABRIC_OBSERVATORY_TESTS_PROCESS_RUNNER_HPP
