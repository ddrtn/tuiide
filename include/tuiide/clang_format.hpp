#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tuiide {

/** Диапазон строк форматирования clang-format: границы включительны и нулевые. */
struct FormatLineRange {
  std::size_t first{};  // zero-based, inclusive
  std::size_t last{};   // zero-based, inclusive
};

/** Итог запуска форматтера: текст меняется только при `success == true`. */
struct FormatResult {
  bool success{};
  std::string text;
  std::string error;
};

[[nodiscard]] auto clangFormat(std::string_view source, const std::filesystem::path& filename,
  std::optional<FormatLineRange> lines = {}, std::string executable = "clang-format") -> FormatResult;

}  // namespace tuiide
