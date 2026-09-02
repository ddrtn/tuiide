#pragma once

#include "tuiide/lsp_client.hpp"

#include <string>
#include <vector>

namespace tuiide {

/** Замена LSP, уже переведённая из UTF-16 позиций в байтовые позиции Document. */
struct PreparedReplacement {
  TextReplacement replacement;
  std::string original;
};

/** Готовит edit без записи: несовпавшая версия или диапазон отменяют всю операцию. */
[[nodiscard]] auto prepareWorkspaceReplacements(const Document& document,
  const std::vector<LspTextEdit>& edits, std::vector<PreparedReplacement>& prepared,
  std::string& error) -> bool;

}  // namespace tuiide
