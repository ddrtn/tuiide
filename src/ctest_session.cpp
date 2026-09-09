#include "tuiide/ctest_session.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <utility>

namespace tuiide {
namespace {
using Json = nlohmann::json;

auto stringValue(const Json& value, std::string_view key) -> std::string {
  const auto found = value.find(key);
  return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

auto exactRegex(std::string_view name) -> std::string {
  static constexpr std::string_view special{".^$|()[]{}*+?\\"};
  std::string result{"^"};
  for (const char character : name) {
    if (special.find(character) != std::string_view::npos) result.push_back('\\');
    result.push_back(character);
  }
  result.push_back('$');
  return result;
}

auto normalizeLocation(std::filesystem::path path, const std::filesystem::path& root)
    -> std::filesystem::path {
  if (path.is_relative()) path = root / path;
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : normalized;
}

void collectPresetFile(const std::filesystem::path& file,
    std::vector<CTestPreset>& result, std::set<std::filesystem::path>& visited,
    std::string& error) {
  std::error_code filesystem_error;
  const auto canonical = std::filesystem::weakly_canonical(file, filesystem_error);
  const auto key = filesystem_error ? file.lexically_normal() : canonical;
  if (!visited.insert(key).second || !std::filesystem::exists(file)) return;
  std::ifstream input(file);
  if (!input) { error = "cannot open " + file.string(); return; }
  Json document;
  try { input >> document; }
  catch (const std::exception& exception) {
    error = file.string() + ": " + exception.what(); return;
  }
  if (!document.is_object()) { error = file.string() + ": root must be an object"; return; }
  const auto includes = document.find("include");
  if (includes != document.end()) {
    std::vector<std::string> paths;
    if (includes->is_string()) paths.push_back(includes->get<std::string>());
    else if (includes->is_array()) {
      for (const auto& include : *includes) if (include.is_string()) paths.push_back(include.get<std::string>());
    }
    for (const auto& include : paths) {
      collectPresetFile(file.parent_path() / include, result, visited, error);
      if (!error.empty()) return;
    }
  }
  const auto presets = document.find("testPresets");
  if (presets == document.end()) return;
  if (!presets->is_array()) { error = file.string() + ": testPresets must be an array"; return; }
  for (const auto& preset : *presets) {
    if (!preset.is_object()) continue;
    const auto name = stringValue(preset, "name");
    const auto hidden_value = preset.find("hidden");
    const bool hidden = hidden_value != preset.end() && hidden_value->is_boolean()
      && hidden_value->get<bool>();
    if (name.empty() || hidden) continue;
    auto display = stringValue(preset, "displayName");
    result.push_back({name, display.empty() ? name : std::move(display)});
  }
}
}  // namespace

CTestSession::~CTestSession() { stop(); }

auto CTestSession::start(std::vector<std::string> arguments,
    const std::filesystem::path& working_directory, CTestOperation operation) -> bool {
  stop();
  captured_.clear(); line_buffer_.clear(); current_test_.clear(); pending_locations_.clear();
  last_error_.clear(); operation_ = operation;
  if (!process_.start(arguments, true, working_directory)) {
    operation_ = CTestOperation::Idle;
    last_error_ = "failed to start ctest";
    return false;
  }
  return true;
}

auto CTestSession::discover(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory, std::string preset) -> bool {
  project_root_ = project_root;
  std::vector<std::string> command{"ctest"};
  std::filesystem::path working = project_root;
  if (preset.empty()) command.insert(command.end(), {"--test-dir", build_directory.string()});
  else command.insert(command.end(), {"--preset", std::move(preset)});
  command.insert(command.end(), {"--show-only=json-v1"});
  return start(std::move(command), working, CTestOperation::Discover);
}

auto CTestSession::runAll(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory) -> bool {
  project_root_ = project_root;
  for (auto& test : tests_) { if (test.status != CTestStatus::Disabled) test.status = CTestStatus::NotRun; test.failures.clear(); }
  tests_changed_ = true;
  return start({"ctest", "--test-dir", build_directory.string(), "--output-on-failure"},
    project_root, CTestOperation::RunAll);
}

auto CTestSession::runSelected(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory, std::string_view name) -> bool {
  project_root_ = project_root;
  return start({"ctest", "--test-dir", build_directory.string(), "--output-on-failure",
      "-R", exactRegex(name)}, project_root, CTestOperation::RunSelected);
}

auto CTestSession::rerunFailed(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory) -> bool {
  project_root_ = project_root;
  return start({"ctest", "--test-dir", build_directory.string(), "--rerun-failed",
      "--output-on-failure"}, project_root, CTestOperation::RerunFailed);
}

auto CTestSession::runPreset(const std::filesystem::path& project_root,
    std::string_view preset) -> bool {
  project_root_ = project_root;
  return start({"ctest", "--preset", std::string(preset), "--output-on-failure"},
    project_root, CTestOperation::RunPreset);
}

auto CTestSession::poll() -> CTestPollResult {
  CTestPollResult result;
  result.output = process_.drain();
  for (const auto& chunk : result.output) {
    captured_ += chunk;
    if (operation_ != CTestOperation::Discover) ingestRunOutput(chunk);
  }
  result.tests_changed = std::exchange(tests_changed_, false);
  const auto exit_code = process_.exitCode();
  if (process_.running() || !exit_code || operation_ == CTestOperation::Idle) return result;
  process_.stop();
  for (auto& chunk : process_.drain()) {
    captured_ += chunk;
    if (operation_ != CTestOperation::Discover) ingestRunOutput(chunk);
    result.output.push_back(std::move(chunk));
  }
  if (operation_ == CTestOperation::Discover) {
    std::vector<CTestCase> discovered;
    if (!parseCTestDiscovery(captured_, discovered, result.error)) last_error_ = result.error;
    else { tests_ = std::move(discovered); result.tests_changed = true; }
  } else {
    finishRunOutput();
    result.tests_changed = true;
  }
  result.completion = *exit_code;
  operation_ = CTestOperation::Idle;
  return result;
}

void CTestSession::ingestRunOutput(std::string_view output) {
  line_buffer_.append(output);
  std::size_t newline{};
  while ((newline = line_buffer_.find('\n')) != std::string::npos) {
    auto line = line_buffer_.substr(0, newline);
    line_buffer_.erase(0, newline + 1);
    static const std::regex start_pattern(R"(^\s*Start\s+[0-9]+:\s+(.+?)\s*$)");
    static const std::regex result_pattern(
      R"(^\s*[0-9]+/[0-9]+\s+Test\s+#[0-9]+:\s+(.+?)\s+\.{3,}\s*(.+?)\s*$)");
    std::smatch match;
    if (std::regex_match(line, match, start_pattern)) {
      current_test_ = match[1].str(); pending_locations_.clear();
      for (auto& test : tests_) if (test.name == current_test_) {
        test.status = CTestStatus::Running; test.failures.clear(); tests_changed_ = true; break;
      }
      continue;
    }
    const auto locations = parseCTestLocations(line, project_root_);
    pending_locations_.insert(pending_locations_.end(), locations.begin(), locations.end());
    if (!std::regex_match(line, match, result_pattern)) continue;
    const auto name = match[1].str();
    const auto outcome = match[2].str();
    for (auto& test : tests_) if (test.name == name) {
      test.status = outcome.find("Passed") != std::string::npos ? CTestStatus::Passed
        : outcome.find("Disabled") != std::string::npos || outcome.find("Not Run") != std::string::npos
          ? CTestStatus::Disabled : CTestStatus::Failed;
      test.failures = pending_locations_;
      tests_changed_ = true;
      break;
    }
    current_test_.clear(); pending_locations_.clear();
  }
}

void CTestSession::finishRunOutput() {
  if (!line_buffer_.empty()) { line_buffer_.push_back('\n'); ingestRunOutput({}); }
  for (auto& test : tests_) if (test.status == CTestStatus::Running) {
    test.status = CTestStatus::Failed; test.failures = pending_locations_;
  }
}

void CTestSession::stop() {
  process_.stop();
  (void)process_.drain();
  if (operation_ != CTestOperation::Idle) {
    for (auto& test : tests_) if (test.status == CTestStatus::Running) test.status = CTestStatus::NotRun;
  }
  operation_ = CTestOperation::Idle;
  captured_.clear(); line_buffer_.clear(); current_test_.clear(); pending_locations_.clear();
}

void CTestSession::clear() { stop(); tests_.clear(); last_error_.clear(); tests_changed_ = true; }

auto parseCTestDiscovery(std::string_view text, std::vector<CTestCase>& tests,
    std::string& error) -> bool {
  tests.clear(); error.clear();
  Json document;
  try { document = Json::parse(text); }
  catch (const std::exception& exception) { error = std::string("invalid CTest JSON: ") + exception.what(); return false; }
  const auto entries = document.find("tests");
  if (entries == document.end() || !entries->is_array()) { error = "CTest JSON has no tests array"; return false; }
  for (const auto& entry : *entries) {
    if (!entry.is_object()) continue;
    CTestCase test;
    test.name = stringValue(entry, "name");
    if (test.name.empty()) continue;
    const auto command = entry.find("command");
    if (command != entry.end() && command->is_array())
      for (const auto& argument : *command) if (argument.is_string()) test.command.push_back(argument.get<std::string>());
    const auto properties = entry.find("properties");
    if (properties != entry.end() && properties->is_array()) for (const auto& property : *properties) {
      if (!property.is_object()) continue;
      const auto name = stringValue(property, "name");
      const auto value = property.find("value");
      if (name == "DISABLED" && value != property.end() && value->is_boolean() && value->get<bool>())
        test.status = CTestStatus::Disabled;
      if (name == "LABELS" && value != property.end() && value->is_array())
        for (const auto& label : *value) if (label.is_string()) test.labels.push_back(label.get<std::string>());
    }
    tests.push_back(std::move(test));
  }
  return true;
}

auto loadCTestPresets(const std::filesystem::path& project_root, std::string& error)
    -> std::vector<CTestPreset> {
  error.clear();
  std::vector<CTestPreset> result;
  std::set<std::filesystem::path> visited;
  collectPresetFile(project_root / "CMakePresets.json", result, visited, error);
  if (error.empty()) collectPresetFile(project_root / "CMakeUserPresets.json", result, visited, error);
  std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
  result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.name == right.name;
  }), result.end());
  return result;
}

auto parseCTestLocations(std::string_view text, const std::filesystem::path& project_root)
    -> std::vector<CTestLocation> {
  std::vector<CTestLocation> result;
  std::set<std::pair<std::filesystem::path, std::size_t>> seen;
  static const std::regex location_pattern(R"(((?:/|\.?\.?/)?[^\s():]+\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx))(?::|\()([0-9]+))");
  const std::string value{text};
  for (auto iterator = std::sregex_iterator(value.begin(), value.end(), location_pattern);
       iterator != std::sregex_iterator(); ++iterator) {
    const auto line = static_cast<std::size_t>(std::stoull((*iterator)[2].str()));
    auto path = normalizeLocation((*iterator)[1].str(), project_root);
    if (line != 0 && seen.emplace(path, line - 1).second) result.push_back({std::move(path), line - 1});
  }
  return result;
}

auto ctestStatusLabel(CTestStatus status) noexcept -> std::string_view {
  switch (status) {
    case CTestStatus::NotRun: return "[ ]";
    case CTestStatus::Running: return "[~]";
    case CTestStatus::Passed: return "[+]";
    case CTestStatus::Failed: return "[!]";
    case CTestStatus::Disabled: return "[-]";
  }
  return "[?]";
}

}  // namespace tuiide
