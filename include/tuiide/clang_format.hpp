#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tuiide {

struct FormatLineRange {
  std::size_t first{};  // zero-based, inclusive
  std::size_t last{};   // zero-based, inclusive
};

struct FormatResult {
  bool success{};
  std::string text;
  std::string error;
};

[[nodiscard]] auto clangFormat(std::string_view source, const std::filesystem::path& filename,
  std::optional<FormatLineRange> lines = {}, std::string executable = "clang-format") -> FormatResult;

}  // namespace tuiide
