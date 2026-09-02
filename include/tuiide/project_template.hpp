#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

enum class ProjectTemplate { CHeader, CppHeader, CSource, CppSource, CppClass };
enum class InheritanceAccess { Public, Protected, Private };

struct CppClassOptions {
  std::string class_name;
  std::string header_file_name;
  std::string source_file_name;
  std::string namespace_name;
  std::string base_class;
  std::string base_header;
  InheritanceAccess inheritance{InheritanceAccess::Public};
  std::filesystem::path header_path;
  std::filesystem::path source_path;
  bool final_class{};
  bool generate_constructor{true};
  bool generate_destructor{true};
  bool virtual_destructor{true};
  bool generate_copy_operations{};
  bool generate_move_operations{};
};

struct ProjectTemplateResult {
  std::vector<std::filesystem::path> created_files;
  std::filesystem::path changed_cmake_file;
  std::size_t cmake_references_added{};
};

auto createProjectTemplate(const std::filesystem::path& project_root,
                           ProjectTemplate type,
                           const std::filesystem::path& relative_path,
                           std::string_view preferred_target,
                           ProjectTemplateResult& result,
                           std::string& error) -> bool;

auto createCppClassTemplate(const std::filesystem::path& project_root,
                            const CppClassOptions& options,
                            std::string_view preferred_target,
                            ProjectTemplateResult& result,
                            std::string& error) -> bool;

auto validateCppClassSettings(const CppClassOptions& options, std::string& error) -> bool;

}  // namespace tuiide
