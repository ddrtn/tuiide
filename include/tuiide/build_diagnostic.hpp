#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tuiide {

enum class DiagnosticSeverity { Note, Warning, Error };

struct BuildDiagnostic {
  std::filesystem::path path;
  std::size_t line{};    // zero-based
  std::size_t column{};  // zero-based
  DiagnosticSeverity severity{};
  std::string message;
};

auto parseCompilerDiagnostic(std::string_view line, const std::filesystem::path& project_root)
  -> std::optional<BuildDiagnostic>;

}  // namespace tuiide
