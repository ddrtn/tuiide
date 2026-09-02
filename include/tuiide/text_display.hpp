#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace tuiide {

inline constexpr std::size_t defaultTabWidth = 2;

/** Один UTF-8 code point и его ширина в терминальных колонках. */
struct DisplayUnit {
  std::size_t byte_begin{};
  std::size_t byte_end{};
  std::size_t column_begin{};
  std::size_t column_end{};
  bool tab{};
};

[[nodiscard]] auto displayUnits(std::string_view text, std::size_t tab_width = defaultTabWidth)
  -> std::vector<DisplayUnit>;
[[nodiscard]] auto displayColumn(std::string_view text, std::size_t byte_column,
  std::size_t tab_width = defaultTabWidth) -> std::size_t;
[[nodiscard]] auto byteColumnAtDisplay(std::string_view text, std::size_t display_column,
  std::size_t tab_width = defaultTabWidth) -> std::size_t;
/** Вычисляет terminal columns, не разрезая wide glyph или combining sequence. */
[[nodiscard]] auto displayWidth(std::string_view text, std::size_t tab_width = defaultTabWidth)
  -> std::size_t;

}  // namespace tuiide
