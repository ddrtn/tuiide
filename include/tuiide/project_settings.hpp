#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "tuiide/launch_configuration.hpp"

namespace tuiide {

struct ProjectSettings {
  int version{1};
  std::filesystem::path build_directory;
  std::string generator;
  std::filesystem::path toolchain;
  std::filesystem::path c_compiler;
  std::filesystem::path cpp_compiler;
  std::string c_standard;
  std::string cpp_standard;
  std::string cpp_header_extension{"hpp"};
  std::string build_type;
  unsigned build_jobs{1};
  unsigned tab_width{2};
  bool use_spaces{true};
  std::map<std::string, std::string> environment;
  std::vector<std::string> clangd_arguments;
  std::map<std::string, std::string> shortcuts;
  std::string theme{"Dark"};
  std::map<std::string, std::string> custom_themes;
  std::map<std::string, std::string> colors;
  LaunchConfiguration launch;
};

[[nodiscard]] auto defaultProjectSettings(const std::filesystem::path& project_directory)
  -> ProjectSettings;
[[nodiscard]] auto validateProjectSettings(const std::filesystem::path& project_directory,
  const ProjectSettings& settings, std::string& error) -> bool;
[[nodiscard]] auto loadProjectSettings(const std::filesystem::path& project_directory,
  ProjectSettings& settings, std::string& error) -> bool;
auto saveProjectSettings(const std::filesystem::path& project_directory,
  const ProjectSettings& settings, std::string& error) -> bool;
auto updateProjectGitignore(const std::filesystem::path& project_directory,
  const ProjectSettings& settings, std::string& error) -> bool;
[[nodiscard]] auto effectiveEditorTheme(const ProjectSettings& settings) -> std::string;
[[nodiscard]] auto parseEnvironmentSettings(std::string_view text,
  std::map<std::string, std::string>& environment, std::string& error) -> bool;
[[nodiscard]] auto formatEnvironmentSettings(const std::map<std::string, std::string>& environment)
  -> std::string;
[[nodiscard]] auto parseArgumentList(std::string_view text, std::vector<std::string>& arguments,
  std::string& error) -> bool;
[[nodiscard]] auto formatArgumentList(const std::vector<std::string>& arguments) -> std::string;

}  // namespace tuiide
