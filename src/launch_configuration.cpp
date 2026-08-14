#include "tuiide/launch_configuration.hpp"

#include "tuiide/document.hpp"

#include <unistd.h>

namespace tuiide {

auto resolveLaunchCommand(const std::filesystem::path& project_directory,
    const LaunchConfiguration& configuration, const CMakeTarget* selected_target,
    LaunchCommand& command, std::string& error) -> bool {
  command = {}; error.clear();
  const auto root = normalizePath(project_directory);
  command.explicit_executable = !configuration.executable.empty();
  command.executable = command.explicit_executable ? normalizePath(configuration.executable)
    : (selected_target ? normalizePath(selected_target->artifact) : std::filesystem::path{});
  if (command.executable.empty()) {
    error = "No launch executable is configured and no CMake executable target is selected.";
    return false;
  }
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(command.executable, filesystem_error)) {
    error = "Launch executable does not exist: " + command.executable.string();
    return false;
  }
  if (::access(command.executable.c_str(), X_OK) != 0) {
    error = "Launch file is not executable: " + command.executable.string();
    return false;
  }
  command.working_directory = configuration.working_directory.empty()
    ? root : normalizePath(configuration.working_directory);
  if (!std::filesystem::is_directory(command.working_directory, filesystem_error)) {
    error = "Launch working directory does not exist: " + command.working_directory.string();
    return false;
  }
  command.arguments = configuration.arguments;
  command.environment = configuration.environment;
  command.stdin_file = configuration.stdin_file.empty() ? std::filesystem::path{}
    : normalizePath(configuration.stdin_file);
  if (!command.stdin_file.empty() && !std::filesystem::is_regular_file(command.stdin_file, filesystem_error)) {
    error = "Launch stdin file does not exist: " + command.stdin_file.string(); return false;
  }
  command.pre_launch_build = configuration.pre_launch_build;
  command.external_terminal = configuration.external_terminal;
  command.terminal = configuration.terminal.empty() ? "x-terminal-emulator" : configuration.terminal;
  command.target = configuration.target;
  return true;
}

auto launchProcessArguments(const LaunchCommand& command) -> std::vector<std::string> {
  if (!command.external_terminal) {
    auto arguments = std::vector<std::string>{command.executable.string()};
    arguments.insert(arguments.end(), command.arguments.begin(), command.arguments.end());
    return arguments;
  }

  auto arguments = std::vector<std::string>{command.terminal, "-e"};
  if (command.stdin_file.empty()) {
    arguments.push_back(command.executable.string());
  } else {
    arguments.insert(arguments.end(), {"sh", "-c", "input=$1; shift; exec \"$@\" < \"$input\"",
      "tuiide-launch", command.stdin_file.string(), command.executable.string()});
  }
  arguments.insert(arguments.end(), command.arguments.begin(), command.arguments.end());
  return arguments;
}

auto integratedLaunchArguments(const LaunchCommand& command) -> std::vector<std::string> {
  if (command.stdin_file.empty()) {
    auto arguments = std::vector<std::string>{command.executable.string()};
    arguments.insert(arguments.end(), command.arguments.begin(), command.arguments.end());
    return arguments;
  }
  auto arguments = std::vector<std::string>{"sh", "-c", "input=$1; shift; exec \"$@\" < \"$input\"",
    "tuiide-launch", command.stdin_file.string(), command.executable.string()};
  arguments.insert(arguments.end(), command.arguments.begin(), command.arguments.end());
  return arguments;
}

}  // namespace tuiide
