#include "tuiide/project_settings.hpp"

#include "tuiide/document.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_set>

namespace tuiide {
namespace {
auto validEnvironmentName(std::string_view name) -> bool {
  if (name.empty() || (std::isalpha(static_cast<unsigned char>(name.front())) == 0 && name.front() != '_'))
    return false;
  return std::all_of(name.begin() + 1, name.end(), [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '_';
  });
}

auto portablePath(const std::filesystem::path& root, const std::filesystem::path& path) -> std::string {
  if (path.empty()) return {};
  const auto normalized = normalizePath(path);
  std::error_code error;
  const auto relative = std::filesystem::relative(normalized, root, error);
  if (!error && !relative.empty() && !relative.generic_string().starts_with("../") && relative != "..")
    return relative.generic_string();
  return normalized.string();
}

auto resolvePath(const std::filesystem::path& root, const std::string& value) -> std::filesystem::path {
  if (value.empty()) return {};
  const std::filesystem::path path(value);
  return normalizePath(path.is_absolute() ? path : root / path);
}

auto readLaunchConfiguration(const std::filesystem::path& root, const nlohmann::json& value)
    -> LaunchConfiguration {
  LaunchConfiguration launch;
  launch.executable = resolvePath(root, value.value("executable", std::string{}));
  launch.target = value.value("target", std::string{});
  launch.working_directory = resolvePath(root, value.value("workingDirectory", std::string{}));
  launch.arguments = value.value("arguments", std::vector<std::string>{});
  launch.environment = value.value("environment", std::map<std::string, std::string>{});
  launch.stdin_file = resolvePath(root, value.value("stdinFile", std::string{}));
  launch.pre_launch_build = value.value("preLaunchBuild", false);
  launch.external_terminal = value.value("externalTerminal", false);
  launch.terminal = value.value("terminal", std::string("x-terminal-emulator"));
  return launch;
}

auto launchConfigurationJson(const std::filesystem::path& root,
    const LaunchConfiguration& launch) -> nlohmann::json {
  return {{"executable", portablePath(root, launch.executable)}, {"target", launch.target},
    {"workingDirectory", portablePath(root, launch.working_directory)},
    {"arguments", launch.arguments}, {"environment", launch.environment},
    {"stdinFile", portablePath(root, launch.stdin_file)},
    {"preLaunchBuild", launch.pre_launch_build},
    {"externalTerminal", launch.external_terminal}, {"terminal", launch.terminal}};
}

auto gitignorePath(std::string value) -> std::string {
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto character : value) {
    if (character == '\\' || character == '!' || character == '#' || character == '['
        || character == ']' || character == '?' || character == '*') result.push_back('\\');
    result.push_back(character);
  }
  if (!result.empty() && result.front() == ' ') result.insert(result.begin(), '\\');
  if (value.size() > 1 && value.back() == ' ') result.insert(result.end() - 1, '\\');
  return result;
}

auto writeAtomically(const std::filesystem::path& destination, std::string_view text,
    std::string& error) -> bool {
  const auto temporary = destination.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size()))) {
      error = "Cannot write temporary file: " + temporary; return false;
    }
  }
  std::error_code filesystem_error;
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    error = "Cannot atomically install " + destination.string();
    return false;
  }
  return true;
}
}  // namespace

auto selectLaunchConfiguration(ProjectSettings& settings, std::string_view name) -> bool {
  const auto selected = std::find_if(settings.launch_configurations.begin(),
    settings.launch_configurations.end(), [name](const auto& item) { return item.name == name; });
  if (selected == settings.launch_configurations.end()) return false;
  settings.active_launch_configuration = selected->name;
  settings.launch = selected->configuration;
  return true;
}

void synchronizeActiveLaunchConfiguration(ProjectSettings& settings) {
  const auto active = std::find_if(settings.launch_configurations.begin(),
    settings.launch_configurations.end(), [&settings](const auto& item) {
      return item.name == settings.active_launch_configuration;
    });
  if (active != settings.launch_configurations.end()) active->configuration = settings.launch;
}

auto defaultProjectSettings(const std::filesystem::path& project_directory) -> ProjectSettings {
  const auto root = normalizePath(project_directory);
  ProjectSettings settings;
  settings.build_directory = root.parent_path() / (root.filename().string() + "-build");
  settings.build_jobs = std::clamp(std::thread::hardware_concurrency(), 1U, 1024U);
  return settings;
}

auto validateProjectSettings(const std::filesystem::path& project_directory,
    const ProjectSettings& settings, std::string& error) -> bool {
  error.clear();
  const auto root = normalizePath(project_directory);
  if (root.empty() || settings.build_directory.empty()) { error = "Build directory is required."; return false; }
  if (normalizePath(settings.build_directory) == root) {
    error = "Build directory must differ from the source directory.";
    return false;
  }
  static const std::vector<std::string> c_standards{"", "90", "99", "11", "17", "23"};
  static const std::vector<std::string> cpp_standards{"", "98", "11", "14", "17", "20", "23", "26"};
  static const std::vector<std::string> build_types{"", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
  if (std::find(c_standards.begin(), c_standards.end(), settings.c_standard) == c_standards.end()) {
    error = "Unsupported C language standard."; return false;
  }
  if (std::find(cpp_standards.begin(), cpp_standards.end(), settings.cpp_standard) == cpp_standards.end()) {
    error = "Unsupported C++ language standard."; return false;
  }
  if (settings.cpp_header_extension != "h" && settings.cpp_header_extension != "hpp") {
    error = "C++ header extension must be h or hpp."; return false;
  }
  if (std::find(build_types.begin(), build_types.end(), settings.build_type) == build_types.end()) {
    error = "Unsupported CMake build type."; return false;
  }
  if (settings.build_jobs == 0 || settings.build_jobs > 1024) {
    error = "Parallel jobs must be between 1 and 1024."; return false;
  }
  if (settings.tab_width == 0 || settings.tab_width > 16) {
    error = "Tab width must be between 1 and 16."; return false;
  }
  static const std::vector<std::string> themes{"Dark", "Light", "High contrast"};
  for (const auto& [name, base] : settings.custom_themes) {
    if (name.empty() || std::find(themes.begin(), themes.end(), name) != themes.end()
        || std::find(themes.begin(), themes.end(), base) == themes.end()) {
      error = "Invalid custom editor theme: " + name; return false;
    }
  }
  if (std::find(themes.begin(), themes.end(), settings.theme) == themes.end()
      && !settings.custom_themes.contains(settings.theme)) {
    error = "Unsupported editor theme."; return false;
  }
  static const std::vector<std::string> color_roles{"foreground", "background", "gutter", "breakpoint",
    "diagnosticError", "diagnosticWarning", "diagnosticNote", "selectionForeground", "selectionBackground",
    "executionLineBackground",
    "plain", "keyword", "type", "string", "number", "comment", "preprocessor", "namespace",
    "function", "variable", "parameter", "property", "macro", "enumMember"};
  static const std::vector<std::string> color_names{"Black", "Blue", "Green", "Cyan", "Red", "Magenta",
    "Brown", "LightGray", "DarkGray", "LightBlue", "LightGreen", "LightCyan", "LightRed",
    "LightMagenta", "Yellow", "White"};
  for (const auto& [role, color] : settings.colors) {
    if (std::find(color_roles.begin(), color_roles.end(), role) == color_roles.end()) {
      error = "Unsupported editor color role: " + role; return false;
    }
    if (std::find(color_names.begin(), color_names.end(), color) == color_names.end()) {
      error = "Unsupported editor color: " + color; return false;
    }
  }
  for (const auto& [name, value] : settings.environment) {
    (void)value;
    if (!validEnvironmentName(name)) { error = "Invalid environment variable name: " + name; return false; }
  }
  if (settings.launch_configurations.empty()) {
    error = "At least one launch configuration is required."; return false;
  }
  std::unordered_set<std::string> launch_names;
  bool active_found{};
  for (const auto& named : settings.launch_configurations) {
    if (named.name.empty() || !launch_names.insert(named.name).second) {
      error = named.name.empty() ? "Launch configuration name is required."
        : "Duplicate launch configuration name: " + named.name;
      return false;
    }
    active_found = active_found || named.name == settings.active_launch_configuration;
    const auto& launch = named.name == settings.active_launch_configuration
      ? settings.launch : named.configuration;
    for (const auto& [name, value] : launch.environment) {
      (void)value;
      if (!validEnvironmentName(name)) {
        error = "Invalid launch environment variable name in " + named.name + ": " + name;
        return false;
      }
    }
    if (launch.external_terminal && launch.terminal.empty()) {
      error = "External terminal command is required for " + named.name + "."; return false;
    }
  }
  if (!active_found) { error = "Active launch configuration was not found."; return false; }
  return true;
}

auto loadProjectSettings(const std::filesystem::path& project_directory,
    ProjectSettings& settings, std::string& error) -> bool {
  error.clear();
  const auto root = normalizePath(project_directory);
  settings = defaultProjectSettings(root);
  std::ifstream input(root / ".tuiide-project.json", std::ios::binary);
  if (!input) return true;
  try {
    const auto json = nlohmann::json::parse(input);
    const auto version = json.value("version", 1);
    if (version != 1) { error = "Unsupported project settings version: " + std::to_string(version); return false; }
    settings.version = version;
    settings.build_directory = resolvePath(root, json.value("buildDirectory", std::string{}));
    if (settings.build_directory.empty()) settings.build_directory = defaultProjectSettings(root).build_directory;
    settings.generator = json.value("generator", std::string{});
    settings.toolchain = resolvePath(root, json.value("toolchain", std::string{}));
    settings.c_compiler = resolvePath(root, json.value("cCompiler", std::string{}));
    settings.cpp_compiler = resolvePath(root, json.value("cppCompiler", std::string{}));
    settings.c_standard = json.value("cStandard", std::string{});
    settings.cpp_standard = json.value("cppStandard", std::string{});
    settings.cpp_header_extension = json.value("cppHeaderExtension", std::string("hpp"));
    settings.build_type = json.value("buildType", std::string{});
    settings.build_jobs = json.value("buildJobs", settings.build_jobs);
    settings.tab_width = json.value("tabWidth", settings.tab_width);
    settings.use_spaces = json.value("useSpaces", settings.use_spaces);
    settings.environment = json.value("environment", std::map<std::string, std::string>{});
    settings.clangd_arguments = json.value("clangdArguments", std::vector<std::string>{});
    settings.shortcuts = json.value("shortcuts", std::map<std::string, std::string>{});
    settings.theme = json.value("theme", std::string("Dark"));
    settings.custom_themes = json.value("customThemes", std::map<std::string, std::string>{});
    settings.colors = json.value("colors", std::map<std::string, std::string>{});
    if (json.contains("launchConfigurations") && json["launchConfigurations"].is_array()) {
      settings.launch_configurations.clear();
      for (const auto& value : json["launchConfigurations"]) {
        if (!value.is_object()) continue;
        const auto name = value.value("name", std::string{});
        const auto configuration = value.find("configuration");
        if (name.empty() || configuration == value.end() || !configuration->is_object()) continue;
        settings.launch_configurations.push_back({name, readLaunchConfiguration(root, *configuration)});
      }
      settings.active_launch_configuration = json.value("activeLaunchConfiguration", std::string("Default"));
      if (!selectLaunchConfiguration(settings, settings.active_launch_configuration)) {
        error = "Active launch configuration was not found."; return false;
      }
    } else if (json.contains("launch") && json["launch"].is_object()) {
      settings.launch = readLaunchConfiguration(root, json["launch"]);
      settings.launch_configurations = {{"Default", settings.launch}};
      settings.active_launch_configuration = "Default";
    }
  } catch (const nlohmann::json::exception& exception) {
    error = "Cannot parse project settings: " + std::string(exception.what());
    return false;
  }
  return validateProjectSettings(root, settings, error);
}

auto saveProjectSettings(const std::filesystem::path& project_directory,
    const ProjectSettings& settings, std::string& error) -> bool {
  const auto root = normalizePath(project_directory);
  if (!validateProjectSettings(root, settings, error)) return false;
  auto launch_configurations = settings.launch_configurations;
  const auto active = std::find_if(launch_configurations.begin(), launch_configurations.end(),
    [&settings](const auto& item) { return item.name == settings.active_launch_configuration; });
  if (active != launch_configurations.end()) active->configuration = settings.launch;
  auto launch_json = nlohmann::json::array();
  for (const auto& named : launch_configurations) {
    launch_json.push_back({{"name", named.name},
      {"configuration", launchConfigurationJson(root, named.configuration)}});
  }
  const nlohmann::json json{
    {"version", 1}, {"buildDirectory", portablePath(root, settings.build_directory)},
    {"generator", settings.generator}, {"toolchain", portablePath(root, settings.toolchain)},
    {"cCompiler", portablePath(root, settings.c_compiler)},
    {"cppCompiler", portablePath(root, settings.cpp_compiler)}, {"cStandard", settings.c_standard},
    {"cppStandard", settings.cpp_standard}, {"cppHeaderExtension", settings.cpp_header_extension},
    {"buildType", settings.build_type},
    {"buildJobs", settings.build_jobs}, {"tabWidth", settings.tab_width},
    {"useSpaces", settings.use_spaces},
    {"environment", settings.environment}, {"clangdArguments", settings.clangd_arguments},
    {"shortcuts", settings.shortcuts},
    {"theme", settings.theme}, {"customThemes", settings.custom_themes}, {"colors", settings.colors},
    {"activeLaunchConfiguration", settings.active_launch_configuration},
    {"launchConfigurations", std::move(launch_json)}
  };
  if (!writeAtomically(root / ".tuiide-project.json", json.dump(2) + "\n", error)) return false;
  error.clear();
  return true;
}

auto effectiveEditorTheme(const ProjectSettings& settings) -> std::string {
  const auto custom = settings.custom_themes.find(settings.theme);
  return custom == settings.custom_themes.end() ? settings.theme : custom->second;
}

auto updateProjectGitignore(const std::filesystem::path& project_directory,
    const ProjectSettings& settings, std::string& error) -> bool {
  error.clear();
  const auto root = normalizePath(project_directory);
  if (!validateProjectSettings(root, settings, error)) return false;
  const auto path = root / ".gitignore";
  std::string original;
  {
    std::ifstream input(path, std::ios::binary);
    if (input) original.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  constexpr std::string_view begin_marker = "# BEGIN TUI IDE";
  constexpr std::string_view end_marker = "# END TUI IDE";
  const auto begin = original.find(begin_marker);
  if (begin != std::string::npos) {
    const auto end = original.find(end_marker, begin + begin_marker.size());
    if (end == std::string::npos) { error = "Malformed TUI IDE block in .gitignore."; return false; }
    auto after = end + end_marker.size();
    if (after < original.size() && original[after] == '\r') ++after;
    if (after < original.size() && original[after] == '\n') ++after;
    original.erase(begin, after - begin);
  }
  while (!original.empty() && (original.back() == '\n' || original.back() == '\r')) original.pop_back();
  if (!original.empty()) original += "\n\n";
  original += "# BEGIN TUI IDE\n.tuiide-project.json\n";
  const auto build = normalizePath(settings.build_directory);
  std::error_code relative_error;
  const auto relative = std::filesystem::relative(build, root, relative_error);
  if (!relative_error && !relative.empty() && relative != ".."
      && !relative.generic_string().starts_with("../"))
    original += "/" + gitignorePath(relative.generic_string()) + "/\n";
  original += "# END TUI IDE\n";
  return writeAtomically(path, original, error);
}

auto parseEnvironmentSettings(std::string_view text, std::map<std::string, std::string>& environment,
    std::string& error) -> bool {
  environment.clear(); error.clear();
  std::size_t begin{};
  while (begin <= text.size()) {
    const auto end = text.find(';', begin);
    auto item = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front())) != 0) item.remove_prefix(1);
    while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back())) != 0) item.remove_suffix(1);
    if (!item.empty()) {
      const auto equal = item.find('=');
      const auto name = item.substr(0, equal);
      if (equal == std::string_view::npos || !validEnvironmentName(name)) {
        error = "Environment must use NAME=value entries separated by semicolons."; return false;
      }
      environment[std::string(name)] = std::string(item.substr(equal + 1));
    }
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return true;
}

auto formatEnvironmentSettings(const std::map<std::string, std::string>& environment) -> std::string {
  std::string result;
  for (const auto& [name, value] : environment) {
    if (!result.empty()) result += "; ";
    result += name + "=" + value;
  }
  return result;
}

auto parseArgumentList(std::string_view text, std::vector<std::string>& arguments, std::string& error) -> bool {
  arguments.clear(); error.clear();
  std::string current;
  char quote{};
  bool escaped{};
  for (const auto character : text) {
    if (escaped) { current.push_back(character); escaped = false; continue; }
    if (character == '\\' && quote != '\'') { escaped = true; continue; }
    if ((character == '\'' || character == '"')) {
      if (quote == character) quote = 0;
      else if (quote == 0) quote = character;
      else current.push_back(character);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character)) != 0 && quote == 0) {
      if (!current.empty()) { arguments.push_back(std::move(current)); current.clear(); }
    } else current.push_back(character);
  }
  if (escaped || quote != 0) { error = "Unterminated escape or quote in clangd arguments."; return false; }
  if (!current.empty()) arguments.push_back(std::move(current));
  return true;
}

auto formatArgumentList(const std::vector<std::string>& arguments) -> std::string {
  std::string result;
  for (const auto& argument : arguments) {
    if (!result.empty()) result.push_back(' ');
    const bool quote = argument.empty() || argument.find_first_of(" \t'\"") != std::string::npos;
    if (!quote) { result += argument; continue; }
    result.push_back('\'');
    for (const auto character : argument) {
      if (character == '\'') result += "'\\''";
      else result.push_back(character);
    }
    result.push_back('\'');
  }
  return result;
}

}  // namespace tuiide
