#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

/** Результат удаления исходника из CMake: изменённые файлы и число ссылок. */
struct CMakeSourceRemoval {
  std::size_t references_removed{};
  std::vector<std::filesystem::path> changed_files;
};

/** Результат добавления исходника в выбранную CMake target. */
struct CMakeSourceAddition {
  std::filesystem::path changed_file;
  std::size_t references_added{};
};

struct CMakePathRename {
  std::filesystem::path old_path;
  std::filesystem::path new_path;
};

/** Транзакция переименования: изменения CMake нужны для rollback при ошибке. */
struct CMakeSourceRename {
  std::size_t references_changed{};
  std::vector<std::filesystem::path> changed_files;
};

/** Удаляет точные path-аргументы CMake, не трогая комментарии и bracket arguments. */
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
