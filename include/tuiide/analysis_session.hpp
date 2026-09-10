#pragma once

#include "tuiide/build_output_collector.hpp"
#include "tuiide/process.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

enum class AnalysisTool { ClangTidy, Cppcheck, IncludeWhatYouUse, Sanitizers };
enum class AnalysisScope { File, Target, Project };
enum class AnalysisStage { Idle, Check, SanitizerConfigure, SanitizerBuild, SanitizerRun };

struct AnalysisCommand {
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
  std::string display;
};

struct AnalysisPollResult {
  std::vector<std::string> output;
  std::size_t diagnostics_added{};
  std::optional<int> completion;
};

/** Выполняет один внешний анализатор и собирает его диагностики построчно. */
class AnalysisSession {
 public:
  ~AnalysisSession();
  void prepare();
  [[nodiscard]] auto start(AnalysisCommand command, AnalysisTool tool,
    AnalysisScope scope, AnalysisStage stage,
    const std::map<std::string, std::string>& environment = {}) -> bool;
  [[nodiscard]] auto poll(const std::filesystem::path& project_root) -> AnalysisPollResult;
  void stop();
  void clearDiagnostics();

  [[nodiscard]] auto running() const noexcept -> bool { return process_.running(); }
  [[nodiscard]] auto stage() const noexcept -> AnalysisStage { return stage_; }
  [[nodiscard]] auto tool() const noexcept -> AnalysisTool { return tool_; }
  [[nodiscard]] auto scope() const noexcept -> AnalysisScope { return scope_; }
  [[nodiscard]] auto diagnostics() const noexcept -> const std::vector<BuildDiagnostic>& {
    return output_.diagnostics();
  }

 private:
  AsyncProcess process_;
  BuildOutputCollector output_;
  AnalysisTool tool_{AnalysisTool::ClangTidy};
  AnalysisScope scope_{AnalysisScope::File};
  AnalysisStage stage_{AnalysisStage::Idle};
};

[[nodiscard]] auto makeAnalysisCommand(AnalysisTool tool, AnalysisScope scope,
  const std::filesystem::path& executable, const std::filesystem::path& project_root,
  const std::filesystem::path& build_directory,
  const std::vector<std::filesystem::path>& sources) -> AnalysisCommand;
[[nodiscard]] auto makeSanitizerConfigureCommand(const std::filesystem::path& cmake,
  const std::filesystem::path& project_root,
  const std::filesystem::path& sanitizer_build_directory,
  std::string_view generator = {}, const std::filesystem::path& toolchain = {},
  const std::filesystem::path& make_program = {},
  const std::filesystem::path& sysroot = {},
  const std::filesystem::path& c_compiler = {},
  const std::filesystem::path& cpp_compiler = {}) -> AnalysisCommand;
[[nodiscard]] auto makeSanitizerBuildCommand(const std::filesystem::path& cmake,
  const std::filesystem::path& sanitizer_build_directory, unsigned jobs,
  std::string_view target) -> AnalysisCommand;
[[nodiscard]] auto makeSanitizerRunCommand(const std::filesystem::path& executable,
  const std::vector<std::string>& arguments,
  const std::filesystem::path& working_directory) -> AnalysisCommand;
[[nodiscard]] auto analysisToolName(AnalysisTool tool) noexcept -> std::string_view;
[[nodiscard]] auto analysisScopeName(AnalysisScope scope) noexcept -> std::string_view;

}  // namespace tuiide
