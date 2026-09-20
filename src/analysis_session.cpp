#include "tuiide/analysis_session.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
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

void AnalysisSession::addDiagnostics(std::vector<BuildDiagnostic> diagnostics) {
  output_.addDiagnostics(std::move(diagnostics));
}

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
    case AnalysisTool::Sanitizers:
    case AnalysisTool::Coverage:
    case AnalysisTool::Valgrind:
    case AnalysisTool::Perf: break;
  }
  command.display = displayCommand(command.arguments);
  return command;
}

auto makeSanitizerConfigureCommand(const std::filesystem::path& cmake,
    const std::filesystem::path& project_root,
    const std::filesystem::path& sanitizer_build_directory,
    std::string_view generator, const std::filesystem::path& toolchain,
    const std::filesystem::path& make_program,
    const std::filesystem::path& sysroot,
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
  if (!make_program.empty()) command.arguments.push_back("-DCMAKE_MAKE_PROGRAM=" + make_program.string());
  if (!sysroot.empty()) command.arguments.push_back("-DCMAKE_SYSROOT=" + sysroot.string());
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

auto makeCoverageConfigureCommand(const std::filesystem::path& cmake,
    const std::filesystem::path& project_root,
    const std::filesystem::path& coverage_build_directory,
    std::string_view generator, const std::filesystem::path& toolchain,
    const std::filesystem::path& make_program,
    const std::filesystem::path& sysroot,
    const std::filesystem::path& c_compiler,
    const std::filesystem::path& cpp_compiler) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {cmake.string(), "-S", project_root.string(), "-B",
    coverage_build_directory.string(), "-DCMAKE_BUILD_TYPE=Debug",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", "-DCMAKE_C_FLAGS=--coverage -O0 -g",
    "-DCMAKE_CXX_FLAGS=--coverage -O0 -g", "-DCMAKE_EXE_LINKER_FLAGS=--coverage",
    "-DCMAKE_SHARED_LINKER_FLAGS=--coverage"};
  if (!generator.empty()) command.arguments.insert(command.arguments.end(), {"-G", std::string(generator)});
  if (!toolchain.empty()) command.arguments.push_back("-DCMAKE_TOOLCHAIN_FILE=" + toolchain.string());
  if (!make_program.empty()) command.arguments.push_back("-DCMAKE_MAKE_PROGRAM=" + make_program.string());
  if (!sysroot.empty()) command.arguments.push_back("-DCMAKE_SYSROOT=" + sysroot.string());
  if (!c_compiler.empty()) command.arguments.push_back("-DCMAKE_C_COMPILER=" + c_compiler.string());
  if (!cpp_compiler.empty()) command.arguments.push_back("-DCMAKE_CXX_COMPILER=" + cpp_compiler.string());
  command.working_directory = project_root;
  command.display = displayCommand(command.arguments);
  return command;
}

auto makeCoverageBuildCommand(const std::filesystem::path& cmake,
    const std::filesystem::path& coverage_build_directory, unsigned jobs,
    std::string_view target) -> AnalysisCommand {
  auto command = makeSanitizerBuildCommand(cmake, coverage_build_directory, jobs, target);
  return command;
}

auto makeCoverageTestCommand(const std::filesystem::path& ctest,
    const std::filesystem::path& coverage_build_directory) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {ctest.string(), "--test-dir", coverage_build_directory.string(),
    "--output-on-failure"};
  command.working_directory = coverage_build_directory;
  command.display = displayCommand(command.arguments);
  return command;
}

auto makeCoverageReportCommand(const std::filesystem::path& gcovr,
    const std::filesystem::path& project_root,
    const std::filesystem::path& coverage_build_directory,
    const std::filesystem::path& json_report) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {gcovr.string(), "--root", project_root.string(), "--object-directory",
    coverage_build_directory.string(), "--json", json_report.string(), "--txt", "-",
    "--print-summary"};
  command.working_directory = project_root;
  command.display = displayCommand(command.arguments);
  return command;
}

auto loadCoverageDiagnostics(const std::filesystem::path& json_report,
    const std::filesystem::path& project_root, std::string& summary,
    std::string& error) -> std::vector<BuildDiagnostic> {
  summary.clear(); error.clear();
  std::ifstream input(json_report, std::ios::binary);
  if (!input) { error = "Cannot open gcovr JSON report: " + json_report.string(); return {}; }
  try {
    const auto report = nlohmann::json::parse(input);
    const auto files = report.find("files");
    if (files == report.end() || !files->is_array()) {
      error = "gcovr JSON report has no files array"; return {};
    }
    std::vector<BuildDiagnostic> diagnostics;
    std::size_t total{};
    std::size_t covered{};
    for (const auto& file : *files) {
      if (!file.is_object()) continue;
      const auto path_item = file.find("file");
      const auto lines = file.find("lines");
      if (path_item == file.end() || !path_item->is_string()
          || lines == file.end() || !lines->is_array()) continue;
      auto path = std::filesystem::path(path_item->get<std::string>());
      if (path.is_relative()) path = project_root / path;
      path = std::filesystem::absolute(path).lexically_normal();
      for (const auto& line : *lines) {
        if (!line.is_object()) continue;
        const auto number_item = line.find("line_number");
        const auto count_item = line.find("count");
        if (number_item == line.end() || !number_item->is_number_integer()
            || count_item == line.end() || !count_item->is_number_integer()) continue;
        const auto signed_line = number_item->get<long long>();
        if (signed_line <= 0) continue;
        const auto line_number = static_cast<std::size_t>(signed_line);
        ++total;
        if (count_item->get<long long>() > 0) { ++covered; continue; }
        diagnostics.push_back({path, line_number - 1, 0, DiagnosticSeverity::Note,
          "line is not covered"});
      }
    }
    const auto percent = total == 0 ? 0.0 : 100.0 * static_cast<double>(covered)
      / static_cast<double>(total);
    std::ostringstream text;
    text.setf(std::ios::fixed); text.precision(1);
    text << "Coverage: " << covered << '/' << total << " lines (" << percent
         << "%), uncovered: " << diagnostics.size();
    summary = text.str();
    return diagnostics;
  } catch (const nlohmann::json::exception& exception) {
    error = "Cannot parse gcovr JSON report: " + std::string(exception.what());
    return {};
  }
}

auto makeValgrindCommand(const std::filesystem::path& valgrind,
    const std::filesystem::path& executable, const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {valgrind.string(), "--tool=memcheck", "--leak-check=full",
    "--show-leak-kinds=all", "--track-origins=yes", "--error-exitcode=99",
    executable.string()};
  command.arguments.insert(command.arguments.end(), arguments.begin(), arguments.end());
  command.working_directory = working_directory;
  command.display = displayCommand(command.arguments);
  return command;
}

auto makePerfRecordCommand(const std::filesystem::path& perf,
    const std::filesystem::path& data_file, const std::filesystem::path& executable,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {perf.string(), "record", "--call-graph", "dwarf", "--output",
    data_file.string(), "--", executable.string()};
  command.arguments.insert(command.arguments.end(), arguments.begin(), arguments.end());
  command.working_directory = working_directory;
  command.display = displayCommand(command.arguments);
  return command;
}

auto makePerfReportCommand(const std::filesystem::path& perf,
    const std::filesystem::path& data_file,
    const std::filesystem::path& working_directory) -> AnalysisCommand {
  AnalysisCommand command;
  command.arguments = {perf.string(), "script", "--input", data_file.string(), "--fields",
    "comm,pid,event,ip,sym,dso,srcline"};
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
    case AnalysisTool::Coverage: return "Coverage";
    case AnalysisTool::Valgrind: return "Valgrind";
    case AnalysisTool::Perf: return "perf";
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
