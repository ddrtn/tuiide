#include "tuiide/workspace_edit.hpp"

#include <algorithm>

namespace tuiide {
namespace {
auto before(const Position& left, const Position& right) -> bool {
  return left.line < right.line || (left.line == right.line && left.column < right.column);
}
}  // namespace

auto prepareWorkspaceReplacements(const Document& document, const std::vector<LspTextEdit>& edits,
    std::vector<PreparedReplacement>& prepared, std::string& error) -> bool {
  prepared.clear();
  error.clear();
  prepared.reserve(edits.size());
  for (const auto& edit : edits) {
    if (edit.start.line >= document.lines().size() || edit.end.line >= document.lines().size()) {
      error = "line is outside the document";
      prepared.clear();
      return false;
    }
    const Position start{edit.start.line, document.byteColumn(edit.start.line, edit.start.column)};
    const Position end{edit.end.line, document.byteColumn(edit.end.line, edit.end.column)};
    if (document.utf16Column(start.line, start.column) != edit.start.column
        || document.utf16Column(end.line, end.column) != edit.end.column || before(end, start)) {
      error = "invalid UTF-16 range";
      prepared.clear();
      return false;
    }
    prepared.push_back({{start, end, edit.text}, document.extractRange(start, end)});
  }
  std::vector<TextReplacement> ordered;
  ordered.reserve(prepared.size());
  for (const auto& item : prepared) ordered.push_back(item.replacement);
  std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
    return before(left.start, right.start);
  });
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    if (before(ordered[index].start, ordered[index - 1].end)) {
      error = "overlapping ranges";
      prepared.clear();
      return false;
    }
  }
  return true;
}

}  // namespace tuiide
