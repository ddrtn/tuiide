#include "tuiide/project_creation.hpp"
#include "tuiide/project_settings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace tuiide {
namespace {
auto validName(std::string_view value) -> bool {
  if (value.empty() || (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')) return false;
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  });
}

auto write(const std::filesystem::path& path, std::string_view text, std::string& error) -> bool {
  std::ofstream output(path, std::ios::binary);
  if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size()))) {
    error = "Cannot write " + path.string();
    return false;
  }
  return true;
}
}  // namespace

auto createNewProject(const NewProjectOptions& options, std::string& error) -> bool {
  error.clear();
  if (!validName(options.name)) { error = "Project name must be a valid CMake target name."; return false; }
  if (options.project_directory.empty() || options.build_directory.empty()) {
    error = "Project and build directories are required."; return false;
  }
  if (options.language == ProjectLanguage::Cpp
      && options.cpp_header_extension != "h" && options.cpp_header_extension != "hpp") {
    error = "C++ header extension must be h or hpp."; return false;
  }
  const std::vector<std::string> standards = options.language == ProjectLanguage::Cpp
    ? std::vector<std::string>{"98", "11", "14", "17", "20", "23", "26"}
    : std::vector<std::string>{"90", "99", "11", "17", "23"};
  if (std::find(standards.begin(), standards.end(), options.language_standard) == standards.end()) {
    error = "Unsupported language standard for the selected language."; return false;
  }
  static const std::vector<std::string> generators{"", "Ninja", "Unix Makefiles"};
  if (std::find(generators.begin(), generators.end(), options.generator) == generators.end()) {
    error = "Unsupported CMake generator."; return false;
  }
  static const std::vector<std::string> build_types{"Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
  if (std::find(build_types.begin(), build_types.end(), options.build_type) == build_types.end()) {
    error = "Unsupported CMake build type."; return false;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(options.project_directory, filesystem_error);
  if (filesystem_error) { error = "Cannot create project directory: " + filesystem_error.message(); return false; }
  if (!std::filesystem::is_empty(options.project_directory, filesystem_error) || filesystem_error) {
    error = "Project directory must be empty."; return false;
  }
  std::filesystem::create_directories(options.build_directory, filesystem_error);
  if (filesystem_error) { error = "Cannot create build directory: " + filesystem_error.message(); return false; }
  std::filesystem::create_directories(options.project_directory / "src", filesystem_error);
  std::filesystem::create_directories(options.project_directory / "include", filesystem_error);
  if (filesystem_error) { error = "Cannot create source directories: " + filesystem_error.message(); return false; }

  const bool cpp = options.language == ProjectLanguage::Cpp;
  const auto source_extension = cpp ? "cpp" : "c";
  const auto header_extension = cpp ? options.cpp_header_extension : "h";
  std::string source;
  std::string header;
  std::string source_name;
  if (options.target_type == ProjectTargetType::Executable) {
    source_name = "main." + std::string(source_extension);
    source = cpp ? "#include <iostream>\n\nint main() {\n  std::cout << \"Hello from " + options.name + "!\\n\";\n  return 0;\n}\n"
                 : "#include <stdio.h>\n\nint main(void) {\n  puts(\"Hello from " + options.name + "!\");\n  return 0;\n}\n";
  } else {
    source_name = options.name + "." + source_extension;
    const auto header_name = options.name + "." + header_extension;
    if (cpp) {
      header = "#pragma once\n\nnamespace " + options.name + " {\nauto version() -> const char*;\n}\n";
      source = "#include \"" + header_name + "\"\n\nnamespace " + options.name + " {\nauto version() -> const char* { return \"0.1.0\"; }\n}\n";
    } else {
      header = "#pragma once\n\nconst char* " + options.name + "_version(void);\n";
      source = "#include \"" + header_name + "\"\n\nconst char* " + options.name + "_version(void) { return \"0.1.0\"; }\n";
    }
    if (!write(options.project_directory / "include" / header_name, header, error)) return false;
  }
  if (!write(options.project_directory / "src" / source_name, source, error)) return false;

  const std::string language = cpp ? "CXX" : "C";
  const std::string command = options.target_type == ProjectTargetType::Executable ? "add_executable"
    : "add_library";
  const std::string kind = options.target_type == ProjectTargetType::StaticLibrary ? " STATIC"
    : (options.target_type == ProjectTargetType::SharedLibrary ? " SHARED" : "");
  std::string cmake = "cmake_minimum_required(VERSION 3.20)\nproject(" + options.name + " VERSION 0.1.0 LANGUAGES " + language + ")\n\n";
  cmake += "set(CMAKE_" + language + "_STANDARD " + options.language_standard + ")\n";
  cmake += "set(CMAKE_" + language + "_STANDARD_REQUIRED ON)\nset(CMAKE_" + language + "_EXTENSIONS OFF)\n\n";
  cmake += command + "(" + options.name + kind + "\n  src/" + source_name + "\n";
  if (options.target_type != ProjectTargetType::Executable)
    cmake += "  include/" + options.name + "." + header_extension + "\n";
  cmake += ")\n";
  if (options.target_type != ProjectTargetType::Executable) {
    if (options.install_layout == ProjectInstallLayout::Gnu) {
      cmake += "include(GNUInstallDirs)\n";
      cmake += "target_include_directories(" + options.name + " PUBLIC\n"
        "  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>\n"
        "  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>\n"
        ")\n";
    } else {
      cmake += "target_include_directories(" + options.name + " PUBLIC include)\n";
    }
  } else if (options.install_layout == ProjectInstallLayout::Gnu) {
    cmake += "include(GNUInstallDirs)\n";
  }
  if (options.warnings)
    cmake += "target_compile_options(" + options.name + " PRIVATE -Wall -Wextra -Wpedantic)\n";
  if (options.install_layout == ProjectInstallLayout::Gnu) {
    cmake += "install(TARGETS " + options.name + "\n"
      "  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}\n"
      "  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
      "  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}\n"
      ")\n";
    if (options.target_type != ProjectTargetType::Executable)
      cmake += "install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})\n";
  }
  if (options.enable_testing) cmake += "\ninclude(CTest)\n";
  if (!write(options.project_directory / "CMakeLists.txt", cmake, error)) return false;
  if (options.create_readme && !write(options.project_directory / "README.md", "# " + options.name + "\n", error)) return false;
  auto project_settings = defaultProjectSettings(options.project_directory);
  project_settings.build_directory = std::filesystem::absolute(options.build_directory);
  project_settings.generator = options.generator;
  project_settings.build_type = options.build_type;
  if (cpp) {
    project_settings.cpp_standard = options.language_standard;
    project_settings.cpp_header_extension = options.cpp_header_extension;
  }
  else project_settings.c_standard = options.language_standard;
  if (!saveProjectSettings(options.project_directory, project_settings, error)) return false;
  if (options.create_gitignore) {
    if (!updateProjectGitignore(options.project_directory, project_settings, error)) return false;
  }
  return true;
}

auto loadProjectBuildDirectory(const std::filesystem::path& project_directory) -> std::filesystem::path {
  ProjectSettings settings;
  std::string error;
  return loadProjectSettings(project_directory, settings, error) ? settings.build_directory : std::filesystem::path{};
}
}  // namespace tuiide
