#include "tuiide/git_session.hpp"

namespace tuiide {

auto GitFileStatus::staged() const -> bool { return !code.empty() && code[0] != ' ' && code != "??"; }
auto GitFileStatus::untracked() const -> bool { return code == "??"; }

auto parseGitStatus(std::string_view data, const std::filesystem::path& project_root)
    -> std::vector<GitFileStatus> {
  std::vector<GitFileStatus> files;
  while (!data.empty()) {
    const auto end = data.find('\0');
    if (end == std::string_view::npos) break;
    const auto record = data.substr(0, end);
    data.remove_prefix(end + 1);
    if (record.size() < 4 || record[2] != ' ') continue;
    const auto code = std::string(record.substr(0, 2));
    const auto path = std::filesystem::path(record.substr(3));
    // У rename/copy формат -z содержит ещё одно имя даже для отброшенной записи.
    const bool renamed = code.find('R') != std::string::npos || code.find('C') != std::string::npos;
    if (renamed) {
      const auto old_end = data.find('\0');
      if (old_end == std::string_view::npos) break;
      data.remove_prefix(old_end + 1);
    }
    if (path.empty() || path.is_absolute()) continue;
    const auto resolved = (project_root / path).lexically_normal();
    const auto relative = resolved.lexically_relative(project_root);
    if (relative.empty() || relative == ".." || relative.native().starts_with("../")) continue;
    files.push_back({resolved, code});
  }
  return files;
}

void GitSession::setRoot(std::filesystem::path root) {
  stop(); root_ = std::move(root).lexically_normal();
}
auto GitSession::root() const -> const std::filesystem::path& { return root_; }
auto GitSession::running() const -> bool { return process_.running(); }

auto GitSession::startStatus() -> bool {
  return start(GitOperation::Status,
    {"status", "--porcelain=v1", "-z", "--untracked-files=all", "--", "."});
}
auto GitSession::startDiff(const std::filesystem::path& path, bool staged) -> bool {
  const auto relative = relativePath(path);
  if (!relative) return false;
  std::vector<std::string> arguments{"diff", "--no-ext-diff", "--no-color"};
  if (staged) arguments.push_back("--cached");
  arguments.insert(arguments.end(), {"--", relative->string()});
  return start(GitOperation::Diff, std::move(arguments));
}
auto GitSession::startStage(const std::filesystem::path& path) -> bool {
  const auto relative = relativePath(path);
  return relative && start(GitOperation::Stage, {"add", "--", relative->string()});
}
auto GitSession::startUnstage(const std::filesystem::path& path, bool newly_added) -> bool {
  const auto relative = relativePath(path);
  if (!relative) return false;
  return newly_added
    ? start(GitOperation::Unstage, {"rm", "-q", "--cached", "--", relative->string()})
    : start(GitOperation::Unstage, {"reset", "-q", "--", relative->string()});
}
auto GitSession::startHistory(const std::filesystem::path& path) -> bool {
  const auto relative = relativePath(path);
  return relative && start(GitOperation::History,
    {"log", "--date=short", "--pretty=format:%h %ad %s", "-n", "30", "--", relative->string()});
}

auto GitSession::poll() -> std::optional<GitUpdate> {
  for (auto& chunk : process_.drain()) {
    if (output_.size() < 1024 * 1024) output_ += chunk.substr(0, 1024 * 1024 - output_.size());
  }
  const auto code = process_.exitCode();
  if (!code || operation_ == GitOperation::None) return std::nullopt;
  process_.stop();
  for (auto& chunk : process_.drain()) {
    if (output_.size() < 1024 * 1024) output_ += chunk.substr(0, 1024 * 1024 - output_.size());
  }
  GitUpdate update{operation_, *code, std::move(output_), {}};
  operation_ = GitOperation::None;
  if (update.operation == GitOperation::Status && update.exit_code == 0)
    update.files = parseGitStatus(update.output, root_);
  return update;
}

void GitSession::stop() {
  process_.stop(); (void)process_.drain(); operation_ = GitOperation::None; output_.clear();
}

auto GitSession::relativePath(const std::filesystem::path& path) const
    -> std::optional<std::filesystem::path> {
  if (root_.empty() || path.empty()) return std::nullopt;
  const auto absolute = (path.is_absolute() ? path : root_ / path).lexically_normal();
  const auto relative = absolute.lexically_relative(root_);
  if (relative.empty() || relative == "." || relative == ".."
      || relative.native().starts_with("../")) return std::nullopt;
  return relative;
}

auto GitSession::start(GitOperation operation, std::vector<std::string> arguments) -> bool {
  if (root_.empty() || operation_ != GitOperation::None || process_.running()) return false;
  std::vector<std::string> command{"git", "-C", root_.string(), "--no-pager", "--literal-pathspecs"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  if (!process_.start(command, true, root_)) return false;
  operation_ = operation; output_.clear(); return true;
}

}  // namespace tuiide
