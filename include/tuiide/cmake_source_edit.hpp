#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

struct CMakeSourceRemoval {
  std::size_t references_removed{};
  std::vector<std::filesystem::path> changed_files;
};

struct CMakeSourceAddition {
  std::filesystem::path changed_file;
  std::size_t references_added{};
};

struct CMakePathRename {
  std::filesystem::path old_path;
  std::filesystem::path new_path;
};

struct CMakeSourceRename {
  std::size_t references_changed{};
  std::vector<std::filesystem::path> changed_files;
};

auto removeCMakeSourceReferences(const std::filesystem::path& project_root,
                                 const std::filesystem::path& source_path,
                                 CMakeSourceRemoval& result,
                                 std::string& error) -> bool;

auto addCMakeSourceReferences(const std::filesystem::path& project_root,
                              const std::vector<std::filesystem::path>& source_paths,
                              std::string_view preferred_target,
                              CMakeSourceAddition& result,
                              std::string& error) -> bool;

auto renameCMakeSourceReferences(const std::filesystem::path& project_root,
                                 const std::vector<CMakePathRename>& renames,
                                 CMakeSourceRename& result,
                                 std::string& error) -> bool;

}  // namespace tuiide
