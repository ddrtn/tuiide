#pragma once

#include "tuiide/lsp_client.hpp"

#include <string>
#include <vector>

namespace tuiide {

struct PreparedReplacement {
  TextReplacement replacement;
  std::string original;
};

[[nodiscard]] auto prepareWorkspaceReplacements(const Document& document,
  const std::vector<LspTextEdit>& edits, std::vector<PreparedReplacement>& prepared,
  std::string& error) -> bool;

}  // namespace tuiide
