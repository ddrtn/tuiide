#include "tuiide/project_import.hpp"

#include "tuiide/document.hpp"
#include "tuiide/project_settings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>

namespace tuiide {
namespace {
auto validTarget(std::string_view value) -> bool {
  if (value.empty() || (!std::isalpha(static_cast<unsigned char>(value.front())) && value.front() != '_')) return false;
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
    return std::isalnum(character) != 0 || character == '_';
  });
}

auto cmakePath(const std::filesystem::path& path) -> std::string {
  std::string result = "\"";
  for (const auto character : path.generic_string()) {
    if (character == '\\' || character == '"') result.push_back('\\');
    result.push_back(character);
  }
  return result + '"';
}

auto writeFile(const std::filesystem::path& path, std::string_view text, std::string& error) -> bool {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || !output.write(text.data(), static_cast<std::streamsize>(text.size()))) {
    error = "Cannot write " + path.string();
    return false;
  }
  return true;
}
}  // namespace

auto planProjectImport(const ProjectImportOptions& options, ProjectImportPlan& plan, std::string& error) -> bool {
  plan = {};
  error.clear();
  if (!validTarget(options.target_name)) { error = "Target name must be a valid CMake identifier"; return false; }
  const auto standards = options.language == ProjectLanguage::Cpp
    ? std::set<std::string>{"98", "11", "14", "17", "20", "23", "26"}
    : std::set<std::string>{"90", "99", "11", "17", "23"};
  if (!standards.contains(options.language_standard)) { error = "Unsupported language standard"; return false; }
  const auto root = normalizePath(options.project_directory);
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(root, filesystem_error)) { error = "Import directory does not exist"; return false; }
  if (std::filesystem::exists(root / "CMakeLists.txt")) { error = "CMakeLists.txt already exists"; return false; }
  const auto build = normalizePath(options.build_directory);
  if (build.empty() || build == root) { error = "Build directory must differ from the project directory"; return false; }
  static const std::set<std::string> c_extensions{".c"};
  static const std::set<std::string> cpp_extensions{".cc", ".cpp", ".cxx"};
  static const std::set<std::string> header_extensions{".h", ".hh", ".hpp", ".hxx", ".ipp"};
  for (std::filesystem::recursive_directory_iterator iterator(root,
         std::filesystem::directory_options::skip_permission_denied, filesystem_error), end;
       iterator != end; iterator.increment(filesystem_error)) {
    if (filesystem_error) { filesystem_error.clear(); continue; }
    const auto path = normalizePath(iterator->path());
    if (iterator->is_directory()) {
      const auto name = path.filename().string();
      if (path == build || name == ".git" || name == "CMakeFiles") iterator.disable_recursion_pending();
      continue;
    }
    if (!iterator->is_regular_file()) continue;
    const auto extension = path.extension().string();
    if (c_extensions.contains(extension)) ++plan.c_sources;
    else if (cpp_extensions.contains(extension)) {
      if (options.language == ProjectLanguage::C) continue;
      ++plan.cpp_sources;
    } else if (header_extensions.contains(extension)) ++plan.headers;
    else continue;
    std::error_code relative_error;
    const auto relative = std::filesystem::relative(path, root, relative_error);
    if (!relative_error) plan.files.push_back(relative);
    if (plan.files.size() > 5000) { error = "Import contains more than 5000 source files"; return false; }
  }
  std::sort(plan.files.begin(), plan.files.end());
  const auto compiled_sources = options.language == ProjectLanguage::Cpp
    ? plan.c_sources + plan.cpp_sources : plan.c_sources;
  if (compiled_sources == 0) { error = "No compatible C/C++ implementation files were found"; return false; }
  const bool mixed = options.language == ProjectLanguage::Cpp && plan.c_sources != 0;
  const std::string languages = options.language == ProjectLanguage::Cpp ? (mixed ? "C CXX" : "CXX") : "C";
  const std::string variable = options.language == ProjectLanguage::Cpp ? "CXX" : "C";
  const std::string command = options.target_type == ProjectTargetType::Executable ? "add_executable"
    : "add_library";
  const std::string kind = options.target_type == ProjectTargetType::StaticLibrary ? " STATIC"
    : (options.target_type == ProjectTargetType::SharedLibrary ? " SHARED" : "");
  plan.cmake_text = "cmake_minimum_required(VERSION 3.20)\nproject(" + options.target_name
    + " VERSION 0.1.0 LANGUAGES " + languages + ")\n\nset(CMAKE_" + variable + "_STANDARD "
    + options.language_standard + ")\nset(CMAKE_" + variable
    + "_STANDARD_REQUIRED ON)\nset(CMAKE_" + variable + "_EXTENSIONS OFF)\n";
  if (mixed) plan.cmake_text += "set(CMAKE_C_STANDARD 17)\nset(CMAKE_C_STANDARD_REQUIRED ON)\n";
  plan.cmake_text += "\n" + command + "(" + options.target_name + kind + "\n";
  for (const auto& file : plan.files) plan.cmake_text += "  " + cmakePath(file) + "\n";
  plan.cmake_text += ")\n";
  if (std::filesystem::is_directory(root / "include", filesystem_error))
    plan.cmake_text += "target_include_directories(" + options.target_name + " PRIVATE include)\n";
  if (options.warnings)
    plan.cmake_text += "target_compile_options(" + options.target_name + " PRIVATE -Wall -Wextra -Wpedantic)\n";
  return true;
}

auto createImportedProject(const ProjectImportOptions& options, const ProjectImportPlan& plan,
    std::string& error) -> bool {
  error.clear();
  ProjectImportPlan verified;
  if (!planProjectImport(options, verified, error)) return false;
  if (verified.files != plan.files || verified.cmake_text != plan.cmake_text) {
    error = "Project contents changed after the import preview";
    return false;
  }
  std::error_code filesystem_error;
  const auto root = normalizePath(options.project_directory);
  const auto build = normalizePath(options.build_directory);
  std::filesystem::create_directories(build, filesystem_error);
  if (filesystem_error) { error = "Cannot create build directory: " + filesystem_error.message(); return false; }
  const auto cmake = root / "CMakeLists.txt";
  const auto settings = root / ".tuiide-project.json";
  const auto cmake_temporary = cmake.string() + ".tuiide-import.tmp";
  const auto settings_temporary = settings.string() + ".tuiide-import.tmp";
  const nlohmann::json configuration{{"version", 1}, {"buildDirectory", build.string()}};
  if (!writeFile(cmake_temporary, plan.cmake_text, error)
      || !writeFile(settings_temporary, configuration.dump(2) + "\n", error)) {
    std::filesystem::remove(cmake_temporary, filesystem_error);
    std::filesystem::remove(settings_temporary, filesystem_error);
    return false;
  }
  std::filesystem::rename(cmake_temporary, cmake, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(cmake_temporary, filesystem_error);
    std::filesystem::remove(settings_temporary, filesystem_error);
    error = "Cannot install CMakeLists.txt";
    return false;
  }
  std::filesystem::rename(settings_temporary, settings, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(cmake, filesystem_error);
    std::filesystem::remove(settings_temporary, filesystem_error);
    error = "Cannot install project settings";
    return false;
  }
  auto project_settings = defaultProjectSettings(root);
  project_settings.build_directory = build;
  if (!updateProjectGitignore(root, project_settings, error)) {
    std::filesystem::remove(cmake, filesystem_error);
    std::filesystem::remove(settings, filesystem_error);
    return false;
  }
  return true;
}

}  // namespace tuiide
