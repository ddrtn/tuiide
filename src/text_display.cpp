#include "tuiide/text_display.hpp"

#include <algorithm>
#include <cwchar>
#include <limits>

namespace tuiide {
namespace {
struct Decoded {
  char32_t codepoint{};
  std::size_t length{1};
};

auto continuation(unsigned char value) -> bool { return (value & 0xc0U) == 0x80U; }

auto decode(std::string_view text, std::size_t offset) -> Decoded {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80U) return {first, 1};
  std::size_t length{};
  char32_t codepoint{};
  if ((first & 0xe0U) == 0xc0U) { length = 2; codepoint = first & 0x1fU; }
  else if ((first & 0xf0U) == 0xe0U) { length = 3; codepoint = first & 0x0fU; }
  else if ((first & 0xf8U) == 0xf0U) { length = 4; codepoint = first & 0x07U; }
  else return {0xfffd, 1};
  if (offset + length > text.size()) return {0xfffd, 1};
  for (std::size_t index = 1; index < length; ++index) {
    const auto value = static_cast<unsigned char>(text[offset + index]);
    if (!continuation(value)) return {0xfffd, 1};
    codepoint = (codepoint << 6U) | (value & 0x3fU);
  }
  const bool overlong = (length == 2 && codepoint < 0x80)
    || (length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000);
  if (overlong || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
    return {0xfffd, 1};
  return {codepoint, length};
}

auto in(char32_t value, char32_t first, char32_t last) -> bool {
  return value >= first && value <= last;
}

auto fallbackWidth(char32_t value) -> std::size_t {
  if (value == 0x200d || in(value, 0x0300, 0x036f) || in(value, 0x1ab0, 0x1aff)
      || in(value, 0x1dc0, 0x1dff) || in(value, 0x20d0, 0x20ff)
      || in(value, 0xfe00, 0xfe0f) || in(value, 0xfe20, 0xfe2f)
      || in(value, 0x1f3fb, 0x1f3ff) || in(value, 0xe0100, 0xe01ef)) return 0;
  const bool wide = in(value, 0x1100, 0x115f) || value == 0x2329 || value == 0x232a
    || in(value, 0x2e80, 0xa4cf) || in(value, 0xac00, 0xd7a3)
    || in(value, 0xf900, 0xfaff) || in(value, 0xfe10, 0xfe19)
    || in(value, 0xfe30, 0xfe6f) || in(value, 0xff00, 0xff60)
    || in(value, 0xffe0, 0xffe6) || in(value, 0x1f300, 0x1faff)
    || in(value, 0x20000, 0x3fffd);
  return wide ? 2U : 1U;
}

auto codepointWidth(char32_t value) -> std::size_t {
  if (value == 0 || value < 0x20 || in(value, 0x7f, 0x9f)) return 0;
  if (value <= static_cast<char32_t>(std::numeric_limits<wchar_t>::max())) {
    const auto width = ::wcwidth(static_cast<wchar_t>(value));
    if (width >= 0) return static_cast<std::size_t>(width);
  }
  return fallbackWidth(value);
}
}  // namespace

auto displayUnits(std::string_view text, std::size_t tab_width) -> std::vector<DisplayUnit> {
  tab_width = std::max<std::size_t>(1, tab_width);
  std::vector<DisplayUnit> result;
  result.reserve(text.size());
  std::size_t offset{};
  std::size_t column{};
  while (offset < text.size()) {
    const auto decoded = decode(text, offset);
    const bool tab = decoded.codepoint == U'\t';
    const auto width = tab ? tab_width - column % tab_width : codepointWidth(decoded.codepoint);
    result.push_back({offset, offset + decoded.length, column, column + width, tab});
    offset += decoded.length;
    column += width;
  }
  return result;
}

auto displayColumn(std::string_view text, std::size_t byte_column,
    std::size_t tab_width) -> std::size_t {
  const auto limit = std::min(byte_column, text.size());
  std::size_t result{};
  for (const auto& unit : displayUnits(text, tab_width)) {
    if (unit.byte_end > limit) break;
    result = unit.column_end;
  }
  return result;
}

auto byteColumnAtDisplay(std::string_view text, std::size_t display_column,
    std::size_t tab_width) -> std::size_t {
  for (const auto& unit : displayUnits(text, tab_width)) {
    if (display_column <= unit.column_begin) return unit.byte_begin;
    if (display_column < unit.column_end) {
      const auto distance = display_column - unit.column_begin;
      const auto width = unit.column_end - unit.column_begin;
      return distance * 2 < width ? unit.byte_begin : unit.byte_end;
    }
  }
  return text.size();
}

auto displayWidth(std::string_view text, std::size_t tab_width) -> std::size_t {
  const auto units = displayUnits(text, tab_width);
  return units.empty() ? 0 : units.back().column_end;
}

}  // namespace tuiide
