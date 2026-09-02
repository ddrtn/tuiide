#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace tuiide {

struct TabBarItemLayout {
  std::size_t index{};
  int x{};
  int width{};
  std::string label;
};

/** Результат размещения вкладок с учётом прокрутки и кнопок `<`/`>`. */
struct TabBarLayout {
  std::vector<TabBarItemLayout> items;
  std::size_t first_index{};
  bool left_overflow{};
  bool right_overflow{};
};

[[nodiscard]] auto layoutTabBar(const std::vector<std::string>& titles,
  const std::vector<bool>& visible, std::size_t current, std::size_t requested_first,
  int width) -> TabBarLayout;

}  // namespace tuiide
