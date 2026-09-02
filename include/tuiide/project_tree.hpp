#pragma once

#include "tuiide/cmake_source_edit.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

/** Одна строка развёрнутого дерева проекта, готовая для передачи list-виджету. */
struct ProjectTreeEntry {
  std::filesystem::path path;
  bool directory{};
};

/**
 * Снимок дерева без UI-ссылок. Сканер исключает VCS и build-каталог, но сохраняет
 * пустые каталоги и список файлов, которые можно менять CMake-aware операциями.
 */
struct ProjectTreeSnapshot {
  std::vector<ProjectTreeEntry> entries;
  std::vector<std::filesystem::path> editable_files;
  std::size_t scanned_files{};
  std::size_t skipped_errors{};
};

[[nodiscard]] auto scanProjectTree(const std::filesystem::path& root,
  const std::filesystem::path& build_directory, std::string_view filter,
  ProjectTreeSnapshot& snapshot, std::string& error) -> bool;
[[nodiscard]] auto createProjectDirectory(const std::filesystem::path& root,
  const std::filesystem::path& path, std::string& error) -> bool;
[[nodiscard]] auto deleteEmptyProjectDirectory(const std::filesystem::path& root,
  const std::filesystem::path& path, std::string& error) -> bool;
[[nodiscard]] auto renameProjectEntry(const std::filesystem::path& root,
  const std::filesystem::path& source, const std::filesystem::path& destination,
  std::string& error) -> bool;
/** Перемещает путь и транзакционно обновляет его аргументы в CMakeLists.txt. */
[[nodiscard]] auto moveProjectEntryWithCMake(const std::filesystem::path& root,
  const std::filesystem::path& source, const std::filesystem::path& destination,
  CMakeSourceRename& result, std::string& error) -> bool;

}  // namespace tuiide
