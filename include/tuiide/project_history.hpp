#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

[[nodiscard]] auto validateProjectDirectory(const std::filesystem::path& path,
  std::string& error) -> bool;
[[nodiscard]] auto defaultProjectHistoryPath() -> std::filesystem::path;
[[nodiscard]] auto loadRecentProjects(const std::filesystem::path& storage,
  std::string& error) -> std::vector<std::filesystem::path>;
auto rememberRecentProject(const std::filesystem::path& storage,
  const std::filesystem::path& project, std::string& error, std::size_t limit = 10) -> bool;

}  // namespace tuiide
