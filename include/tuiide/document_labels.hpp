#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

/** Строит кратчайшие различающие suffix-пути для файлов с одинаковым basename. */
[[nodiscard]] auto distinguishDocumentLabels(
  const std::vector<std::filesystem::path>& paths) -> std::vector<std::string>;

}  // namespace tuiide
