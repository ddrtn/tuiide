#include "tuiide/document_labels.hpp"

#include <map>
#include <set>

namespace tuiide {
namespace {
auto suffix(const std::filesystem::path& path, std::size_t parent_count) -> std::string {
  auto current = path.lexically_normal();
  std::filesystem::path result = current.filename();
  current = current.parent_path();
  for (std::size_t count = 0; count < parent_count && !current.empty()
       && current != current.root_path(); ++count) {
    result = current.filename() / result;
    current = current.parent_path();
  }
  return result.generic_string();
}
}  // namespace

auto distinguishDocumentLabels(const std::vector<std::filesystem::path>& paths)
    -> std::vector<std::string> {
  std::vector<std::string> labels;
  labels.reserve(paths.size());
  std::map<std::string, std::vector<std::size_t>> groups;
  for (std::size_t index = 0; index < paths.size(); ++index) {
    labels.push_back(paths[index].empty() ? "Untitled" : paths[index].filename().string());
    groups[labels.back()].push_back(index);
  }
  for (const auto& [base, indices] : groups) {
    if (indices.size() < 2) continue;
    if (base == "Untitled") {
      for (std::size_t item = 0; item < indices.size(); ++item)
        labels[indices[item]] += " " + std::to_string(item + 1);
      continue;
    }
    bool unique{};
    for (std::size_t depth = 1; depth < 128 && !unique; ++depth) {
      std::set<std::string> candidates;
      for (const auto index : indices) candidates.insert(suffix(paths[index], depth));
      unique = candidates.size() == indices.size();
      if (unique)
        for (const auto index : indices) labels[index] = suffix(paths[index], depth);
    }
    if (!unique)
      for (std::size_t item = 0; item < indices.size(); ++item)
        labels[indices[item]] = suffix(paths[indices[item]], 127) + " #" + std::to_string(item + 1);
  }
  return labels;
}

}  // namespace tuiide
