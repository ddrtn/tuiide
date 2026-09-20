#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tuiide {

/** Уровень сообщения компилятора для Problems и EventLog. */
enum class DiagnosticSeverity { Note, Warning, Error };

/** Нормализованная диагностика CMake/компилятора с нулевыми индексами. */
struct BuildDiagnostic {
  std::filesystem::path path;
  std::size_t line{};    // zero-based
  std::size_t column{};  // zero-based
  DiagnosticSeverity severity{};
  std::string message;
};

auto parseCompilerDiagnostic(std::string_view line, const std::filesystem::path& project_root)
  -> std::optional<BuildDiagnostic>;
/** Распознаёт итоговую строку ASan/UBSan с исходным файлом и позицией. */
auto parseSanitizerDiagnostic(std::string_view line,
  const std::filesystem::path& project_root) -> std::optional<BuildDiagnostic>;
/** Распознаёт навигационный stack frame Valgrind. */
auto parseValgrindDiagnostic(std::string_view line,
  const std::filesystem::path& project_root) -> std::optional<BuildDiagnostic>;
/** Распознаёт source location в текстовом выводе perf script. */
auto parsePerfDiagnostic(std::string_view line,
  const std::filesystem::path& project_root) -> std::optional<BuildDiagnostic>;

}  // namespace tuiide
