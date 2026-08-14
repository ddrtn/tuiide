#include "tuiide/tab_bar_layout.hpp"

#include <algorithm>

namespace tuiide {

auto layoutTabBar(const std::vector<std::string>& titles, const std::vector<bool>& visible,
    std::size_t current, std::size_t requested_first, int width) -> TabBarLayout {
  TabBarLayout result;
  std::vector<std::size_t> indices;
  for (std::size_t index{}; index < titles.size(); ++index)
    if (index >= visible.size() || visible[index]) indices.push_back(index);
  if (indices.empty() || width <= 0) return result;

  const auto total = [&] {
    int value{};
    for (const auto index : indices) value += static_cast<int>(titles[index].size() + 2);
    return value;
  }();
  const bool scrolling = total > width;
  const int content_width = std::max(1, width - (scrolling ? 2 : 0));
  auto first = std::find(indices.begin(), indices.end(), requested_first);
  std::size_t first_position = first == indices.end() ? 0
    : static_cast<std::size_t>(std::distance(indices.begin(), first));
  const auto selected = std::find(indices.begin(), indices.end(), current);
  if (selected != indices.end()) {
    const auto selected_position = static_cast<std::size_t>(std::distance(indices.begin(), selected));
    if (selected_position < first_position) first_position = selected_position;
    while (first_position < selected_position) {
      int required{};
      for (auto position = first_position; position <= selected_position; ++position)
        required += static_cast<int>(titles[indices[position]].size() + 2);
      if (required <= content_width) break;
      ++first_position;
    }
  }

  result.first_index = indices[first_position];
  result.left_overflow = first_position > 0;
  int x = scrolling ? 2 : 1;
  int remaining = content_width;
  std::size_t position = first_position;
  bool truncated_item{};
  for (; position < indices.size() && remaining > 0; ++position) {
    auto label = " " + titles[indices[position]] + " ";
    const bool truncated = static_cast<int>(label.size()) > remaining;
    if (truncated) label.resize(static_cast<std::size_t>(remaining));
    if (label.empty()) break;
    result.items.push_back({indices[position], x, static_cast<int>(label.size()), std::move(label)});
    x += result.items.back().width; remaining -= result.items.back().width;
    if (truncated) { truncated_item = true; ++position; break; }
  }
  result.right_overflow = truncated_item || position < indices.size();
  return result;
}

}  // namespace tuiide
