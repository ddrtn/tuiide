#include "tuiide/workspace_file_transaction.hpp"

#include "tuiide/document.hpp"

#include <fstream>

namespace tuiide {
namespace {
auto insideProject(const std::filesystem::path& root, const std::filesystem::path& path) -> bool {
  const auto relative = normalizePath(path).lexically_relative(normalizePath(root));
  return !relative.empty() && relative != "." && *relative.begin() != "..";
}

auto pathExists(const std::filesystem::path& path, std::error_code& error) -> bool {
  error.clear();
  const bool result = std::filesystem::exists(path, error);
  return result && !error;
}
}  // namespace

auto WorkspaceFileTransaction::prepare(const std::filesystem::path& project_root,
    std::vector<WorkspaceFileOperation> operations, std::string& error) -> bool {
  root_ = normalizePath(project_root); operations_ = std::move(operations);
  applied_.clear(); applied_indices_.clear(); error.clear();
  if (root_.empty()) { error = "No project is open."; return false; }
  for (auto& operation : operations_) {
    operation.path = normalizePath(operation.path);
    if (operation.path.empty() || !insideProject(root_, operation.path)) {
      error = "Language server file operation is outside the project: " + operation.path.string();
      return false;
    }
    if (operation.kind == WorkspaceFileOperationKind::Rename) {
      operation.new_path = normalizePath(operation.new_path);
      if (operation.new_path.empty() || !insideProject(root_, operation.new_path)) {
        error = "Language server rename destination is outside the project: " + operation.new_path.string();
        return false;
      }
      if (operation.path == operation.new_path) {
        error = "Language server requested a rename to the same path."; return false;
      }
    }
  }
  return true;
}

auto WorkspaceFileTransaction::backupPath(const std::filesystem::path& path) -> std::filesystem::path {
  for (std::size_t sequence = 0; ; ++sequence) {
    const auto candidate = path.parent_path() / ("." + path.filename().string()
      + ".tuiide-lsp-backup-" + std::to_string(sequence));
    std::error_code error;
    if (!std::filesystem::exists(candidate, error) && !error) return candidate;
  }
}

auto WorkspaceFileTransaction::apply(std::string& error) -> bool {
  error.clear();
  // Никакая операция не уничтожает прежний файл сразу: overwrite/delete сначала
  // переименовывают его в backup. При ошибке порядок applied_ разворачивается.
  for (std::size_t index = 0; index < operations_.size(); ++index) {
    const auto& operation = operations_[index];
    std::error_code filesystem_error;
    const bool source_exists = pathExists(operation.path, filesystem_error);
    if (filesystem_error) { error = filesystem_error.message(); break; }
    AppliedOperation applied{index, {}};
    if (operation.kind == WorkspaceFileOperationKind::Create) {
      if (source_exists && operation.ignore_if_exists) continue;
      if (source_exists && !operation.overwrite) { error = "File already exists: " + operation.path.string(); break; }
      if (!std::filesystem::is_directory(operation.path.parent_path(), filesystem_error) || filesystem_error) {
        error = "Parent directory does not exist: " + operation.path.parent_path().string(); break;
      }
      if (source_exists) {
        applied.backup = backupPath(operation.path);
        std::filesystem::rename(operation.path, applied.backup, filesystem_error);
        if (filesystem_error) { error = "Cannot preserve existing file: " + filesystem_error.message(); break; }
      }
      std::ofstream output(operation.path, std::ios::binary | std::ios::trunc);
      if (!output) { error = "Cannot create file: " + operation.path.string(); }
    } else if (operation.kind == WorkspaceFileOperationKind::Rename) {
      if (!source_exists) { error = "Rename source does not exist: " + operation.path.string(); break; }
      const bool destination_exists = pathExists(operation.new_path, filesystem_error);
      if (filesystem_error) { error = filesystem_error.message(); break; }
      if (destination_exists && operation.ignore_if_exists) continue;
      if (destination_exists && !operation.overwrite) {
        error = "Rename destination already exists: " + operation.new_path.string(); break;
      }
      if (!std::filesystem::is_directory(operation.new_path.parent_path(), filesystem_error) || filesystem_error) {
        error = "Rename parent directory does not exist: " + operation.new_path.parent_path().string(); break;
      }
      if (destination_exists) {
        applied.backup = backupPath(operation.new_path);
        std::filesystem::rename(operation.new_path, applied.backup, filesystem_error);
        if (filesystem_error) { error = "Cannot preserve rename destination: " + filesystem_error.message(); break; }
      }
      std::filesystem::rename(operation.path, operation.new_path, filesystem_error);
      if (filesystem_error) { error = "Cannot rename file: " + filesystem_error.message(); }
    } else {
      if (!source_exists && operation.ignore_if_not_exists) continue;
      if (!source_exists) { error = "Delete target does not exist: " + operation.path.string(); break; }
      if (std::filesystem::is_directory(operation.path, filesystem_error)
          && !operation.recursive && !std::filesystem::is_empty(operation.path, filesystem_error)) {
        error = "Recursive option is required to delete a non-empty directory: " + operation.path.string(); break;
      }
      applied.backup = backupPath(operation.path);
      std::filesystem::rename(operation.path, applied.backup, filesystem_error);
      if (filesystem_error) error = "Cannot stage deletion: " + filesystem_error.message();
    }
    if (!error.empty()) {
      if (operation.kind == WorkspaceFileOperationKind::Create && applied.backup.empty()) {
        filesystem_error.clear(); std::filesystem::remove_all(operation.path, filesystem_error);
      } else if (operation.kind == WorkspaceFileOperationKind::Create && !applied.backup.empty()) {
        filesystem_error.clear(); std::filesystem::remove_all(operation.path, filesystem_error);
        if (!filesystem_error) std::filesystem::rename(applied.backup, operation.path, filesystem_error);
      } else if (!applied.backup.empty() && std::filesystem::exists(applied.backup, filesystem_error))
        std::filesystem::rename(applied.backup, operation.kind == WorkspaceFileOperationKind::Rename
          ? operation.new_path : operation.path, filesystem_error);
      break;
    }
    applied_.push_back(std::move(applied)); applied_indices_.push_back(index);
  }
  if (error.empty()) return true;
  std::string rollback_error;
  if (!rollback(rollback_error) && !rollback_error.empty()) error += "; rollback failed: " + rollback_error;
  return false;
}

auto WorkspaceFileTransaction::rollback(std::string& error) -> bool {
  error.clear();
  // Откат обратен применению: rename возвращается в исходный путь до восстановления
  // прежнего destination, иначе можно потерять файл при overwrite.
  for (auto iterator = applied_.rbegin(); iterator != applied_.rend(); ++iterator) {
    const auto& operation = operations_[iterator->index];
    std::error_code filesystem_error;
    if (operation.kind == WorkspaceFileOperationKind::Create) {
      std::filesystem::remove_all(operation.path, filesystem_error);
      if (!filesystem_error && !iterator->backup.empty())
        std::filesystem::rename(iterator->backup, operation.path, filesystem_error);
    } else if (operation.kind == WorkspaceFileOperationKind::Rename) {
      std::filesystem::rename(operation.new_path, operation.path, filesystem_error);
      if (!filesystem_error && !iterator->backup.empty())
        std::filesystem::rename(iterator->backup, operation.new_path, filesystem_error);
    } else {
      std::filesystem::rename(iterator->backup, operation.path, filesystem_error);
    }
    if (filesystem_error && error.empty()) error = filesystem_error.message();
  }
  applied_.clear(); applied_indices_.clear();
  return error.empty();
}

auto WorkspaceFileTransaction::commit(std::string& error) -> bool {
  error.clear();
  for (const auto& applied : applied_) {
    if (applied.backup.empty()) continue;
    std::error_code filesystem_error;
    std::filesystem::remove_all(applied.backup, filesystem_error);
    if (filesystem_error && error.empty()) error = filesystem_error.message();
  }
  applied_.clear(); applied_indices_.clear();
  return error.empty();
}

auto WorkspaceFileTransaction::operations() const -> const std::vector<WorkspaceFileOperation>& {
  return operations_;
}
auto WorkspaceFileTransaction::appliedOperationIndices() const -> const std::vector<std::size_t>& {
  return applied_indices_;
}

}  // namespace tuiide
