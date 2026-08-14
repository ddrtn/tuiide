#pragma once

#include <filesystem>
#include <string>

namespace tuiide {

enum class ProjectLanguage { C, Cpp };
enum class ProjectTargetType { Executable, StaticLibrary, SharedLibrary };
enum class ProjectInstallLayout { None, Gnu };

struct NewProjectOptions {
  std::string name;
  std::filesystem::path project_directory;
  std::filesystem::path build_directory;
  ProjectLanguage language{ProjectLanguage::Cpp};
  ProjectTargetType target_type{ProjectTargetType::Executable};
  std::string language_standard{"20"};
  std::string cpp_header_extension{"hpp"};
  std::string generator;
  std::string build_type{"Debug"};
  ProjectInstallLayout install_layout{ProjectInstallLayout::None};
  bool warnings{true};
  bool create_readme{true};
  bool create_gitignore{true};
  bool enable_testing{};
};

auto createNewProject(const NewProjectOptions& options, std::string& error) -> bool;
auto loadProjectBuildDirectory(const std::filesystem::path& project_directory)
  -> std::filesystem::path;

}  // namespace tuiide
