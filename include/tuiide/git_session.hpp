#pragma once

#include "tuiide/process.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

struct GitFileStatus {
  std::filesystem::path path;
  std::string code;
  [[nodiscard]] auto staged() const -> bool;
  [[nodiscard]] auto untracked() const -> bool;
};

enum class GitOperation { None, Status, Diff, Stage, Unstage, History };

struct GitUpdate {
  GitOperation operation{GitOperation::None};
  int exit_code{};
  std::string output;
  std::vector<GitFileStatus> files;
};

/** Асинхронные операции Git, ограниченные корнем открытого проекта. */
class GitSession {
 public:
  void setRoot(std::filesystem::path root);
  [[nodiscard]] auto root() const -> const std::filesystem::path&;
  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto startStatus() -> bool;
  [[nodiscard]] auto startDiff(const std::filesystem::path& path, bool staged) -> bool;
  [[nodiscard]] auto startStage(const std::filesystem::path& path) -> bool;
  [[nodiscard]] auto startUnstage(const std::filesystem::path& path, bool newly_added = false) -> bool;
  [[nodiscard]] auto startHistory(const std::filesystem::path& path) -> bool;
  [[nodiscard]] auto poll() -> std::optional<GitUpdate>;
  void stop();

 private:
  [[nodiscard]] auto relativePath(const std::filesystem::path& path) const
    -> std::optional<std::filesystem::path>;
  [[nodiscard]] auto start(GitOperation operation, std::vector<std::string> arguments) -> bool;

  AsyncProcess process_;
  std::filesystem::path root_;
  GitOperation operation_{GitOperation::None};
  std::string output_;
};

[[nodiscard]] auto parseGitStatus(std::string_view data,
  const std::filesystem::path& project_root) -> std::vector<GitFileStatus>;

}  // namespace tuiide
