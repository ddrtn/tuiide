#pragma once

#include "tuiide/document.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

struct SearchOptions {
  bool case_sensitive{};
  bool whole_word{};
  bool regular_expression{};
};

struct SearchMatch {
  Position start;
  Position end;
  std::size_t offset{};
  std::size_t length{};
  std::string replacement;
};

[[nodiscard]] auto searchText(std::string_view text, std::string_view query,
  std::string_view replacement, const SearchOptions& options, std::string& error)
  -> std::vector<SearchMatch>;

}  // namespace tuiide
