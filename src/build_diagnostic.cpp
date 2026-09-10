#include "tuiide/build_diagnostic.hpp"

#include <charconv>
#include <regex>

namespace tuiide {
namespace {
auto parseNumber(std::string_view value, std::size_t& number) -> bool {
  const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
  return result.ec == std::errc{} && result.ptr == value.data() + value.size() && number > 0;
}
}

auto parseCompilerDiagnostic(std::string_view text, const std::filesystem::path& project_root)
  -> std::optional<BuildDiagnostic> {
  std::string cleaned;
  cleaned.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
      i += 2;
      while (i < text.size() && !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))) ++i;
      continue;
    }
    cleaned.push_back(text[i]);
  }
  text = cleaned;
  if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
  const auto severity_end = text.find(": ");
  if (severity_end == std::string_view::npos) return std::nullopt;

  DiagnosticSeverity severity{};
  std::size_t location_end{};
  const auto find_severity = [&](std::string_view marker, DiagnosticSeverity value) -> bool {
    const auto position = text.find(marker);
    if (position == std::string_view::npos) return false;
    location_end = position;
    severity = value;
    return true;
  };
  if (!find_severity(": fatal error: ", DiagnosticSeverity::Error)
      && !find_severity(": error: ", DiagnosticSeverity::Error)
      && !find_severity(": runtime error: ", DiagnosticSeverity::Error)
      && !find_severity(": warning: ", DiagnosticSeverity::Warning)
      && !find_severity(": note: ", DiagnosticSeverity::Note)) return std::nullopt;

  const auto last_separator = text.rfind(':', location_end - 1);
  if (last_separator == std::string_view::npos || last_separator == 0) return std::nullopt;
  const auto previous_separator = text.rfind(':', last_separator - 1);
  std::size_t line_number{};
  std::size_t column_number{1};
  std::size_t path_end = last_separator;
  std::size_t trailing_number{};
  if (!parseNumber(text.substr(last_separator + 1, location_end - last_separator - 1), trailing_number)) return std::nullopt;
  std::size_t preceding_number{};
  if (previous_separator != std::string_view::npos && previous_separator > 0
      && parseNumber(text.substr(previous_separator + 1, last_separator - previous_separator - 1), preceding_number)) {
    line_number = preceding_number;
    column_number = trailing_number;
    path_end = previous_separator;
  } else line_number = trailing_number;

  auto path = std::filesystem::path(text.substr(0, path_end));
  if (path.is_relative()) path = project_root / path;
  const auto message_at = text.find(": ", location_end + 1);
  const auto message = message_at == std::string_view::npos ? std::string{} : std::string(text.substr(message_at + 2));
  return BuildDiagnostic{std::filesystem::absolute(path).lexically_normal(), line_number - 1, column_number - 1, severity, message};
}

auto parseSanitizerDiagnostic(std::string_view text,
    const std::filesystem::path& project_root) -> std::optional<BuildDiagnostic> {
  static const std::regex pattern(
    R"(^SUMMARY: (?:AddressSanitizer|UndefinedBehaviorSanitizer): (.*?) ((?:/|\.\.?/).+\.(?:c|cc|cpp|cxx|h|hh|hpp|hxx)):(\d+)(?::(\d+))?.*$)");
  const std::string line{text};
  std::smatch match;
  if (!std::regex_match(line, match, pattern)) return std::nullopt;
  std::size_t line_number{};
  std::size_t column_number{1};
  if (!parseNumber(match[3].str(), line_number)) return std::nullopt;
  if (match[4].matched && !parseNumber(match[4].str(), column_number)) return std::nullopt;
  auto path = std::filesystem::path(match[2].str());
  if (path.is_relative()) path = project_root / path;
  return BuildDiagnostic{std::filesystem::absolute(path).lexically_normal(),
    line_number - 1, column_number - 1, DiagnosticSeverity::Error,
    match[1].str()};
}

}  // namespace tuiide
