#include "tuiide/analysis_session.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace tuiide {
namespace {
auto quoted(const std::string& argument) -> std::string {
  if (argument.find_first_of(" \t\"'") == std::string::npos) return argument;
  std::string result{"\""};
  for (const char character : argument) result += character == '\"' ? "\\\"" : std::string(1, character);
  return result + '"';
}

auto displayCommand(const std::vector<std::string>& arguments) -> std::string {
  std::ostringstream output;
  for (std::size_t index{}; index < arguments.size(); ++index) {
    if (index != 0) output << ' ';
    output << quoted(arguments[index]);
  }
  return output.str();
}
}  // namespace

AnalysisSession::~AnalysisSession() { stop(); }

void AnalysisSession::prepare() {
  process_.stop(); (void)process_.drain(); output_.reset(); stage_ = AnalysisStage::Idle;
}

auto AnalysisSession::start(AnalysisCommand command, AnalysisTool tool,
    AnalysisScope scope, AnalysisStage stage,
    const std::map<std::string, std::string>& environment) -> bool {
  process_.stop(); (void)process_.drain();
  tool_ = tool; scope_ = scope; stage_ = stage;
  if (process_.start(command.arguments, true, command.working_directory, environment)) return true;
  stage_ = AnalysisStage::Idle;
  return false;
}

auto AnalysisSession::poll(const std::filesystem::path& project_root) -> AnalysisPollResult {
  AnalysisPollResult result;
  result.output = process_.drain();
  for (const auto& chunk : result.output)
    result.diagnostics_added += output_.append(chunk, project_root).diagnostics_added;
  const auto exit_code = process_.exitCode();
  if (process_.running() || !exit_code || stage_ == AnalysisStage::Idle) return result;
  process_.stop();
  for (auto& chunk : process_.drain()) {
    result.diagnostics_added += output_.append(chunk, project_root).diagnostics_added;
    result.output.push_back(std::move(chunk));
  }
  result.diagnostics_added += output_.finish(project_root).diagnostics_added;
  result.completion = *exit_code;
  stage_ = AnalysisStage::Idle;
  return result;
}

void AnalysisSession::stop() {
  process_.stop(); (void)process_.drain(); stage_ = AnalysisStage::Idle;
}

void AnalysisSession::clearDiagnostics() { output_.reset(); }

auto makeAnalysisCommand(AnalysisTool tool, AnalysisScope scope,
    const std::filesystem::path& executable,
    const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory,
    const std::vector<std::filesystem::path>& sources) -> AnalysisCommand {
  AnalysisCommand command;
  command.working_directory = project_root;
  command.arguments.push_back(executable.string());
  switch (tool) {
    case AnalysisTool::ClangTidy:
      for (const auto& source : sources) command.arguments.push_back(source.string());
      command.arguments.push_back("-p=" + build_directory.string());
      command.arguments.push_back("--quiet");
      break;
    case AnalysisTool::Cppcheck:
      command.arguments.insert(command.arguments.end(), {"--enable=warning,performance,portability",
        "--inline-suppr", "--suppress=missingIncludeSystem",
        "--template={file}:{line}:{column}: {severity}: {message}"});
      if (scope == AnalysisScope::Project)
        command.arguments.push_back("--project=" + (build_directory / "compile_commands.json").string());
      else for (const auto& source : sources) command.arguments.push_back(source.string());
      break;
    case AnalysisTool::IncludeWhatYouUse:
      command.arguments.insert(command.arguments.end(), {"-p", build_directory.string()});
      for (const auto& source : sources) command.arguments.push_back(source.string());
      break;
    case AnalysisTool::Sanitizers: break;
  }
  command.display = displayCommand(command.arguments);
  return command;
}

auto makeSanitizerConfigureCommand(const std::filesystem::path& cmake,
    const std::filesystem::path& project_root,
    const std::filesystem::path& sanitizer_build_directory,
    std::string_view generator, const std::filesystem::path& toolchain,
    const std::filesystem::path& c_compiler,
    const std::filesystem::path& cpp_compiler) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {cmake.string(), "-S", project_root.string(), "-B",
    sanitizer_build_directory.string(), "-DCMAKE_BUILD_TYPE=Debug",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
    "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer",
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined",
    "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined"};
  if (!generator.empty()) command.arguments.insert(command.arguments.end(), {"-G", std::string(generator)});
  if (!toolchain.empty()) command.arguments.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain.string());
  if (!c_compiler.empty()) command.arguments.push_back("-DCMAKE_C_COMPILER=" + c_compiler.string());
  if (!cpp_compiler.empty()) command.arguments.push_back("-DCMAKE_CXX_COMPILER=" + cpp_compiler.string());
  command.working_directory = project_root;
  command.display = displayCommand(command.arguments);
  return command;
}

auto makeSanitizerBuildCommand(const std::filesystem::path& cmake,
    const std::filesystem::path& sanitizer_build_directory, unsigned jobs,
    std::string_view target) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {cmake.string(), "--build", sanitizer_build_directory.string(),
    "--parallel", std::to_string(std::max(1U, jobs))};
  if (!target.empty()) command.arguments.insert(command.arguments.end(), {"--target", std::string(target)});
  command.display = displayCommand(command.arguments);
  return command;
}

auto makeSanitizerRunCommand(const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments.push_back(executable.string());
  command.arguments.insert(command.arguments.end(), arguments.begin(), arguments.end());
  command.working_directory = working_directory;
  command.display = displayCommand(command.arguments);
  return command;
}

auto analysisToolName(AnalysisTool tool) noexcept -> std::string_view {
  switch (tool) {
    case AnalysisTool::ClangTidy: return "clang-tidy";
    case AnalysisTool::Cppcheck: return "cppcheck";
    case AnalysisTool::IncludeWhatYouUse: return "include-what-you-use";
    case AnalysisTool::Sanitizers: return "ASan + UBSan";
  }
  return "analysis";
}

auto analysisScopeName(AnalysisScope scope) noexcept -> std::string_view {
  switch (scope) {
    case AnalysisScope::File: return "file";
    case AnalysisScope::Target: return "target";
    case AnalysisScope::Project: return "project";
  }
  return "scope";
}

}  // namespace tuiide
