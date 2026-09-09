#pragma once

#include "tuiide/process.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

enum class CTestStatus { NotRun, Running, Passed, Failed, Disabled };
enum class CTestOperation { Idle, Discover, RunAll, RunSelected, RerunFailed, RunPreset };

struct CTestLocation {
  std::filesystem::path path;
  std::size_t line{};  // zero-based
};

struct CTestCase {
  std::string name;
  std::vector<std::string> command;
  std::vector<std::string> labels;
  CTestStatus status{CTestStatus::NotRun};
  std::vector<CTestLocation> failures;
};

struct CTestPreset {
  std::string name;
  std::string display_name;
};

struct CTestPollResult {
  std::vector<std::string> output;
  bool tests_changed{};
  std::optional<int> completion;
  std::string error;
};

/**
 * Асинхронная CTest-сессия: discovery JSON, запуск и разбор результатов.
 * UI только выбирает режим и отображает неизменяемый список testCases().
 */
class CTestSession {
 public:
  ~CTestSession();

  [[nodiscard]] auto discover(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory, std::string preset = {}) -> bool;
  [[nodiscard]] auto runAll(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory) -> bool;
  [[nodiscard]] auto runSelected(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory, std::string_view name) -> bool;
  [[nodiscard]] auto rerunFailed(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory) -> bool;
  [[nodiscard]] auto runPreset(const std::filesystem::path& project_root,
    std::string_view preset) -> bool;
  [[nodiscard]] auto poll() -> CTestPollResult;
  void stop();
  void clear();

  [[nodiscard]] auto running() const noexcept -> bool { return process_.running(); }
  [[nodiscard]] auto operation() const noexcept -> CTestOperation { return operation_; }
  [[nodiscard]] auto tests() const noexcept -> const std::vector<CTestCase>& { return tests_; }
  [[nodiscard]] auto lastError() const noexcept -> const std::string& { return last_error_; }

 private:
  [[nodiscard]] auto start(std::vector<std::string> arguments,
    const std::filesystem::path& working_directory, CTestOperation operation) -> bool;
  void ingestRunOutput(std::string_view output);
  void finishRunOutput();

  AsyncProcess process_;
  CTestOperation operation_{CTestOperation::Idle};
  std::vector<CTestCase> tests_;
  std::string captured_;
  std::string line_buffer_;
  std::string current_test_;
  std::vector<CTestLocation> pending_locations_;
  std::filesystem::path project_root_;
  std::string last_error_;
  bool tests_changed_{};
};

[[nodiscard]] auto parseCTestDiscovery(std::string_view json,
  std::vector<CTestCase>& tests, std::string& error) -> bool;
[[nodiscard]] auto loadCTestPresets(const std::filesystem::path& project_root,
  std::string& error) -> std::vector<CTestPreset>;
[[nodiscard]] auto parseCTestLocations(std::string_view text,
  const std::filesystem::path& project_root) -> std::vector<CTestLocation>;
[[nodiscard]] auto ctestStatusLabel(CTestStatus status) noexcept -> std::string_view;

}  // namespace tuiide
