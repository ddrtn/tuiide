#include "tuiide/build_progress.hpp"

#include <charconv>

namespace tuiide {
namespace {
auto number(std::string_view value) -> std::optional<unsigned> {
  while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
  while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
  unsigned result{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() ? std::optional{result} : std::nullopt;
}
}  // namespace

auto parseBuildProgress(std::string_view line) -> std::optional<unsigned> {
  const auto bracket = line.find('[');
  const auto close = bracket == std::string_view::npos ? bracket : line.find(']', bracket + 1);
  if (bracket != std::string_view::npos && close != std::string_view::npos) {
    const auto content = line.substr(bracket + 1, close - bracket - 1);
    if (const auto slash = content.find('/'); slash != std::string_view::npos) {
      const auto current = number(content.substr(0, slash));
      const auto total = number(content.substr(slash + 1));
      if (current && total && *total != 0 && *current <= *total)
        return static_cast<unsigned>((static_cast<unsigned long long>(*current) * 100ULL) / *total);
    }
    if (const auto percent = content.find('%'); percent != std::string_view::npos) {
      const auto value = number(content.substr(0, percent));
      if (value && *value <= 100) return value;
    }
  }
  const auto percent = line.find('%');
  if (percent != std::string_view::npos) {
    auto begin = percent;
    while (begin > 0 && (line[begin - 1] == ' ' || (line[begin - 1] >= '0' && line[begin - 1] <= '9'))) --begin;
    const auto value = number(line.substr(begin, percent - begin));
    if (value && *value <= 100) return value;
  }
  return std::nullopt;
}

}  // namespace tuiide
