#pragma once

#include "tuiide/project_creation.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

struct ProjectImportOptions {
  std::filesystem::path project_directory;
  std::filesystem::path build_directory;
  std::string target_name;
  ProjectLanguage language{ProjectLanguage::Cpp};
  ProjectTargetType target_type{ProjectTargetType::Executable};
  std::string language_standard{"20"};
  bool warnings{true};
};

/** Preview импорта: найденные файлы и будущий CMakeLists до подтверждения пользователя. */
struct ProjectImportPlan {
  std::vector<std::filesystem::path> files;
  std::string cmake_text;
  std::size_t c_sources{};
  std::size_t cpp_sources{};
  std::size_t headers{};
};

[[nodiscard]] auto planProjectImport(const ProjectImportOptions& options,
  ProjectImportPlan& plan, std::string& error) -> bool;
/** Применяет только подтверждённый preview и не перезаписывает CMake-проект. */
auto createImportedProject(const ProjectImportOptions& options,
  const ProjectImportPlan& plan, std::string& error) -> bool;

}  // namespace tuiide
