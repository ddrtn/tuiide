#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

inline constexpr std::string_view version = "0.1.0";

/** Разобранные аргументы командной строки, не зависящие от терминального UI. */
struct CliOptions {
  std::filesystem::path project;
  std::filesystem::path log_file;
  bool diagnostic{};
  bool show_help{};
  bool show_version{};
};

struct CliParseResult {
  CliOptions options;
  std::string error;
};

/** Разбирает CLI без исключений; help/version можно обработать до создания FApplication. */
[[nodiscard]] auto parseCommandLine(const std::vector<std::string_view>& arguments)
  -> CliParseResult;
[[nodiscard]] auto commandLineHelp(std::string_view program) -> std::string;

}  // namespace tuiide
