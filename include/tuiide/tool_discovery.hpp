#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

/** Доступность внешней утилиты и диагностическое сообщение для Output. */
struct ExternalToolStatus {
  std::string name;
  std::optional<std::filesystem::path> executable;
  std::string purpose;
};

/** Результат одной проверки CMake, clangd, GDB и clipboard helper. */
struct ExternalTools {
  std::optional<std::filesystem::path> cmake;
  std::optional<std::filesystem::path> clangd;
  std::optional<std::filesystem::path> gdb;
  std::vector<ExternalToolStatus> clipboard;
};

[[nodiscard]] auto findExecutable(std::string_view name,
  std::string_view path_environment) -> std::optional<std::filesystem::path>;
[[nodiscard]] auto discoverExternalTools() -> ExternalTools;
[[nodiscard]] auto externalToolMessages(const ExternalTools& tools,
  bool include_available) -> std::vector<std::string>;

}  // namespace tuiide
