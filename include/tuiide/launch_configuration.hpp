#pragma once

#include "tuiide/cmake_model.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tuiide {

/** Настройки запуска target: executable, аргументы, среда, stdin и pre-launch build. */
struct LaunchConfiguration {
  std::filesystem::path executable;
  std::string target;
  std::filesystem::path working_directory;
  std::vector<std::string> arguments;
  std::map<std::string, std::string> environment;
  std::filesystem::path stdin_file;
  bool pre_launch_build{};
  bool external_terminal{};
  std::string terminal{"x-terminal-emulator"};
};

/** Разрешённая конфигурация, готовая для передачи RunSession или GdbClient. */
struct LaunchCommand {
  std::filesystem::path executable;
  std::filesystem::path working_directory;
  std::vector<std::string> arguments;
  std::map<std::string, std::string> environment;
  std::filesystem::path stdin_file;
  bool pre_launch_build{};
  bool external_terminal{};
  std::string terminal;
  std::string target;
  bool explicit_executable{};
};

[[nodiscard]] auto resolveLaunchCommand(const std::filesystem::path& project_directory,
  const LaunchConfiguration& configuration, const CMakeTarget* selected_target,
  LaunchCommand& command, std::string& error) -> bool;

[[nodiscard]] auto launchProcessArguments(const LaunchCommand& command) -> std::vector<std::string>;
[[nodiscard]] auto integratedLaunchArguments(const LaunchCommand& command) -> std::vector<std::string>;

}  // namespace tuiide
