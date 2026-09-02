#pragma once

#include "tuiide/cmake_model.hpp"
#include "tuiide/cmake_presets.hpp"
#include "tuiide/project_settings.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tuiide {

/** Полностью подготовленная команда CMake без запуска процесса. */
struct BuildCommand {
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
  std::string display;
};

/** Формирует воспроизводимые команды configure/build из настроек, preset и target. */
class BuildCommandService {
 public:
  [[nodiscard]] static auto configure(const std::filesystem::path& project_directory,
    const std::filesystem::path& build_directory, const ProjectSettings& settings,
    const std::string& preset_name, const std::vector<CMakeConfigurePreset>& presets,
    std::string& error) -> std::optional<BuildCommand>;

  [[nodiscard]] static auto build(const std::filesystem::path& project_directory,
    const std::filesystem::path& build_directory, unsigned parallel_jobs,
    const std::string& preset_name, const CMakeTarget* target, bool clean) -> BuildCommand;
};

}  // namespace tuiide
