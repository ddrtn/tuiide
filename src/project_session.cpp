#include "tuiide/project_session.hpp"

#include "tuiide/document.hpp"
#include "tuiide/project_history.hpp"

namespace tuiide {

auto ProjectSession::open(std::filesystem::path project_directory,
    std::filesystem::path build_directory, ProjectOpenResult& result,
    std::string& error) -> bool {
  result = {};
  if (!validateProjectDirectory(project_directory, error)) return false;

  const auto normalized_root = normalizePath(project_directory);
  ProjectSettings loaded_settings;
  std::string settings_error;
  if (!loadProjectSettings(normalized_root, loaded_settings, settings_error)) {
    loaded_settings = defaultProjectSettings(normalized_root);
    result.used_default_settings = true;
    result.warning = settings_error;
  }
  if (!build_directory.empty()) loaded_settings.build_directory = normalizePath(build_directory);

  root_ = normalized_root;
  settings_ = std::move(loaded_settings);
  refreshDerivedPaths();
  error.clear();
  return true;
}

void ProjectSession::close() {
  root_.clear();
  build_directory_.clear();
  session_file_.clear();
  recovery_file_.clear();
  settings_ = {};
}

void ProjectSession::applySettings(ProjectSettings settings) {
  settings_ = std::move(settings);
  refreshDerivedPaths();
}

void ProjectSession::refreshDerivedPaths() {
  build_directory_ = settings_.build_directory;
  session_file_ = build_directory_ / ".tuiide-session.json";
  recovery_file_ = build_directory_ / ".tuiide-recovery.json";
}

}  // namespace tuiide
