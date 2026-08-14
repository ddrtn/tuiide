#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

[[nodiscard]] auto distinguishDocumentLabels(
  const std::vector<std::filesystem::path>& paths) -> std::vector<std::string>;

}  // namespace tuiide
