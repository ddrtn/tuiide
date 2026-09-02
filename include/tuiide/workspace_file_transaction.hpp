#pragma once

#include "tuiide/lsp_client.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

/**
 * Обратимая файловая часть workspace/applyEdit от clangd.
 * До commit() сохраняет backup удаляемых и перезаписываемых файлов, поэтому UI
 * может откатить частично применённую операцию при ошибке.
 */
class WorkspaceFileTransaction {
 public:
  /** Проверяет, что все пути и операции остаются внутри project_root. */
  auto prepare(const std::filesystem::path& project_root,
    std::vector<WorkspaceFileOperation> operations, std::string& error) -> bool;
  auto apply(std::string& error) -> bool;
  auto rollback(std::string& error) -> bool;
  auto commit(std::string& error) -> bool;

  [[nodiscard]] auto operations() const -> const std::vector<WorkspaceFileOperation>&;
  [[nodiscard]] auto appliedOperationIndices() const -> const std::vector<std::size_t>&;

 private:
  struct AppliedOperation {
    std::size_t index{};
    std::filesystem::path backup;
  };

  auto backupPath(const std::filesystem::path& path) -> std::filesystem::path;

  std::filesystem::path root_;
  std::vector<WorkspaceFileOperation> operations_;
  std::vector<AppliedOperation> applied_;
  std::vector<std::size_t> applied_indices_;
};

}  // namespace tuiide
