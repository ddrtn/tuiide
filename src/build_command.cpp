#include "tuiide/build_command.hpp"

#include <algorithm>

namespace tuiide {
namespace {
void appendProjectSettings(std::vector<std::string>& arguments, const ProjectSettings& settings) {
  if (!settings.generator.empty()) arguments.insert(arguments.end(), {"-G", settings.generator});
  const auto definition = [&arguments](std::string name, const auto& value) {
    if (!value.empty()) arguments.push_back("-D" + std::move(name) + "=" + value.string());
  };
  definition("CMAKE_TOOLCHAIN_FILE", settings.toolchain);
  definition("CMAKE_MAKE_PROGRAM", settings.make_program);
  definition("CMAKE_SYSROOT", settings.sysroot);
  definition("CMAKE_C_COMPILER", settings.c_compiler);
  definition("CMAKE_CXX_COMPILER", settings.cpp_compiler);
  if (!settings.c_standard.empty()) arguments.push_back("-DCMAKE_C_STANDARD=" + settings.c_standard);
  if (!settings.cpp_standard.empty()) arguments.push_back("-DCMAKE_CXX_STANDARD=" + settings.cpp_standard);
  if (!settings.build_type.empty()) arguments.push_back("-DCMAKE_BUILD_TYPE=" + settings.build_type);
}
}  // namespace

auto BuildCommandService::configure(const std::filesystem::path& project_directory,
    const std::filesystem::path& build_directory, const ProjectSettings& settings,
    const std::string& preset_name, const std::vector<CMakeConfigurePreset>& presets,
    std::string& error) -> std::optional<BuildCommand> {
  error.clear();
  BuildCommand command;
  if (preset_name.empty()) {
    command.arguments = {"cmake", "-S", project_directory.string(), "-B", build_directory.string()};
    command.display = "$ cmake -S " + project_directory.string() + " -B " + build_directory.string()
      + " [project settings]";
  } else {
    const auto preset = std::find_if(presets.begin(), presets.end(), [&](const auto& item) {
      return item.name == preset_name;
    });
    if (preset == presets.end()) {
      error = "CMake configure preset no longer exists: " + preset_name;
      return std::nullopt;
    }
    command.arguments = {"cmake", "--preset", preset_name, "-S", project_directory.string()};
    command.display = "$ cmake --preset " + preset_name + " -S " + project_directory.string();
    if (preset->binary_directory.empty()) {
      command.arguments.insert(command.arguments.end(), {"-B", build_directory.string()});
      command.display += " -B " + build_directory.string();
    }
  }
  appendProjectSettings(command.arguments, settings);
  command.arguments.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
  if (preset_name.empty() && settings.build_type.empty())
    command.arguments.push_back("-DCMAKE_BUILD_TYPE=Debug");
  return command;
}

auto BuildCommandService::build(const std::filesystem::path& project_directory,
    const std::filesystem::path& build_directory, unsigned parallel_jobs,
    const std::string& preset_name, const CMakeTarget* target, bool clean) -> BuildCommand {
  BuildCommand command;
  const auto jobs = std::to_string(std::max(1U, parallel_jobs));
  if (preset_name.empty()) {
    command.arguments = {"cmake", "--build", build_directory.string(), "--parallel", jobs};
    command.display = "$ cmake --build " + build_directory.string() + " --parallel " + jobs;
  } else {
    command.arguments = {"cmake", "--build", "--preset", preset_name, "--parallel", jobs};
    command.working_directory = project_directory;
    command.display = "$ cmake --build --preset " + preset_name + " --parallel " + jobs;
  }
  if (clean) {
    command.arguments.insert(command.arguments.end(), {"--target", "clean"});
    command.display += " --target clean";
  } else if (target) {
    command.arguments.insert(command.arguments.end(), {"--target", target->name});
    command.display += " --target " + target->name;
    if (!target->configuration.empty()) {
      command.arguments.insert(command.arguments.end(), {"--config", target->configuration});
      command.display += " --config " + target->configuration;
    }
  }
  return command;
}

}  // namespace tuiide
