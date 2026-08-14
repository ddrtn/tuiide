#include "tuiide/text_search.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace tuiide {
namespace {
auto escapeRegex(std::string_view value) -> std::string {
  static constexpr std::string_view special = R"(\.^$|()[]{}*+?)";
  std::string result;
  result.reserve(value.size() * 2);
  for (const auto character : value) {
    if (special.find(character) != std::string_view::npos) result.push_back('\\');
    result.push_back(character);
  }
  return result;
}

auto positionAt(std::string_view text, std::size_t offset) -> Position {
  Position result;
  const auto limit = std::min(offset, text.size());
  for (std::size_t index = 0; index < limit; ++index) {
    if (text[index] == '\n') { ++result.line; result.column = 0; }
    else ++result.column;
  }
  return result;
}

auto wordByte(char value) -> bool {
  const auto byte = static_cast<unsigned char>(value);
  return byte >= 0x80U || std::isalnum(byte) != 0 || value == '_';
}
}  // namespace

auto searchText(std::string_view text_view, std::string_view query, std::string_view replacement,
    const SearchOptions& options, std::string& error) -> std::vector<SearchMatch> {
  error.clear();
  if (query.empty()) { error = "Search text is empty"; return {}; }
  const std::string text(text_view);
  try {
    auto flags = std::regex_constants::ECMAScript;
    if (!options.case_sensitive) flags |= std::regex_constants::icase;
    const std::regex expression(options.regular_expression ? std::string(query) : escapeRegex(query), flags);
    std::vector<SearchMatch> result;
    for (std::sregex_iterator iterator(text.begin(), text.end(), expression), end; iterator != end; ++iterator) {
      const auto offset = static_cast<std::size_t>(iterator->position());
      const auto length = static_cast<std::size_t>(iterator->length());
      if (options.whole_word) {
        const bool left_word = offset > 0 && wordByte(text[offset - 1]);
        const bool right_word = offset + length < text.size() && wordByte(text[offset + length]);
        if (left_word || right_word) continue;
      }
      const auto start = positionAt(text, offset);
      const auto finish = positionAt(text, offset + length);
      result.push_back({start, finish, offset, length,
        options.regular_expression ? iterator->format(std::string(replacement)) : std::string(replacement)});
    }
    return result;
  } catch (const std::regex_error& exception) {
    error = "Invalid regular expression: " + std::string(exception.what());
    return {};
  }
}

}  // namespace tuiide
