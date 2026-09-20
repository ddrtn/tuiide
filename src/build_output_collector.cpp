#include "tuiide/build_output_collector.hpp"

#include "tuiide/build_progress.hpp"

#include <utility>

namespace tuiide {

auto BuildOutputCollector::append(std::string_view chunk,
    const std::filesystem::path& project_root) -> BuildOutputUpdate {
  BuildOutputUpdate update;
  partial_.append(chunk);
  std::size_t newline{};
  while ((newline = partial_.find('\n')) != std::string::npos) {
    const auto line = partial_.substr(0, newline);
    partial_.erase(0, newline + 1);
    consumeLine(line, project_root, update);
  }
  return update;
}

auto BuildOutputCollector::finish(const std::filesystem::path& project_root)
    -> BuildOutputUpdate {
  BuildOutputUpdate update;
  if (!partial_.empty()) consumeLine(partial_, project_root, update);
  partial_.clear();
  return update;
}

void BuildOutputCollector::reset() {
  partial_.clear();
  diagnostics_.clear();
  diagnostic_keys_.clear();
}

void BuildOutputCollector::addDiagnostics(std::vector<BuildDiagnostic> diagnostics) {
  for (auto& diagnostic : diagnostics) {
    const auto key = diagnostic.path.string() + ':' + std::to_string(diagnostic.line) + ':'
      + std::to_string(diagnostic.column) + ':'
      + std::to_string(static_cast<int>(diagnostic.severity)) + ':' + diagnostic.message;
    if (diagnostic_keys_.insert(key).second) diagnostics_.push_back(std::move(diagnostic));
  }
}

void BuildOutputCollector::clearDiagnostics() {
  diagnostics_.clear();
  diagnostic_keys_.clear();
}

void BuildOutputCollector::consumeLine(std::string_view line,
    const std::filesystem::path& project_root, BuildOutputUpdate& update) {
  if (const auto progress = parseBuildProgress(line)) update.progress = progress;
  auto diagnostic = parseCompilerDiagnostic(line, project_root);
  if (!diagnostic) diagnostic = parseSanitizerDiagnostic(line, project_root);
  if (!diagnostic) diagnostic = parseValgrindDiagnostic(line, project_root);
  if (!diagnostic) diagnostic = parsePerfDiagnostic(line, project_root);
  if (diagnostic) {
    const auto count = diagnostics_.size();
    std::vector<BuildDiagnostic> parsed;
    parsed.push_back(std::move(*diagnostic));
    addDiagnostics(std::move(parsed));
    if (diagnostics_.size() != count) ++update.diagnostics_added;
  }
}

}  // namespace tuiide
