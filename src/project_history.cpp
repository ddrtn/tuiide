#include "tuiide/project_history.hpp"

#include "tuiide/document.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>

namespace tuiide {

auto validateProjectDirectory(const std::filesystem::path& path, std::string& error) -> bool {
  error.clear();
  if (path.empty()) { error = "No project directory was selected"; return false; }
  const auto normalized = normalizePath(path);
  std::error_code status_error;
  if (!std::filesystem::is_directory(normalized, status_error)) {
    error = "Project directory does not exist: " + normalized.string();
    return false;
  }
  if (!std::filesystem::is_regular_file(normalized / "CMakeLists.txt", status_error)) {
    error = "CMakeLists.txt was not found in " + normalized.string();
    return false;
  }
  return true;
}

auto defaultProjectHistoryPath() -> std::filesystem::path {
  if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return std::filesystem::path(xdg) / "tuiide" / "recent-projects.json";
  if (const auto* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / ".config" / "tuiide" / "recent-projects.json";
  return {};
}

auto loadRecentProjects(const std::filesystem::path& storage, std::string& error)
    -> std::vector<std::filesystem::path> {
  error.clear();
  if (storage.empty() || !std::filesystem::exists(storage)) return {};
  std::ifstream input(storage);
  if (!input) { error = "Cannot read recent projects: " + storage.string(); return {}; }
  try {
    const auto root = nlohmann::json::parse(input);
    if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("projects")
        || !root["projects"].is_array()) {
      error = "Unsupported recent-projects file: " + storage.string();
      return {};
    }
    std::vector<std::filesystem::path> projects;
    std::unordered_set<std::filesystem::path> seen;
    for (const auto& value : root["projects"]) {
      if (!value.is_string()) continue;
      const auto path = normalizePath(value.get<std::string>());
      std::string validation_error;
      if (!seen.insert(path).second || !validateProjectDirectory(path, validation_error)) continue;
      projects.push_back(path);
    }
    return projects;
  } catch (const nlohmann::json::exception& exception) {
    error = "Cannot parse recent projects: " + std::string(exception.what());
    return {};
  }
}

auto rememberRecentProject(const std::filesystem::path& storage, const std::filesystem::path& project,
    std::string& error, std::size_t limit) -> bool {
  error.clear();
  if (storage.empty()) return true;
  std::string validation_error;
  if (!validateProjectDirectory(project, validation_error)) { error = std::move(validation_error); return false; }
  auto projects = loadRecentProjects(storage, error);
  if (!error.empty()) { projects.clear(); error.clear(); }
  const auto normalized = normalizePath(project);
  projects.erase(std::remove(projects.begin(), projects.end(), normalized), projects.end());
  projects.insert(projects.begin(), normalized);
  if (projects.size() > limit) projects.resize(limit);
  std::error_code directory_error;
  std::filesystem::create_directories(storage.parent_path(), directory_error);
  if (directory_error) { error = "Cannot create configuration directory: " + directory_error.message(); return false; }
  const auto temporary = storage.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) { error = "Cannot write recent projects: " + temporary; return false; }
    nlohmann::json root{{"version", 1}, {"projects", nlohmann::json::array()}};
    for (const auto& path : projects) root["projects"].push_back(path.string());
    output << root.dump(2) << '\n';
    if (!output) { error = "Cannot write recent projects: " + temporary; return false; }
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, storage, rename_error);
  if (rename_error) {
    std::filesystem::remove(temporary, directory_error);
    error = "Cannot replace recent projects: " + rename_error.message();
    return false;
  }
  return true;
}

}  // namespace tuiide
