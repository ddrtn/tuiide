#include "tuiide/recovery.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace tuiide {

auto saveRecovery(const std::filesystem::path& file,
    const std::vector<RecoveryDocument>& documents, std::string& error) -> bool {
  if (documents.empty()) { clearRecovery(file); error.clear(); return true; }
  std::error_code filesystem_error;
  std::filesystem::create_directories(file.parent_path(), filesystem_error);
  if (filesystem_error) { error = "Cannot create recovery directory: " + filesystem_error.message(); return false; }
  auto entries = nlohmann::json::array();
  for (const auto& document : documents) entries.push_back({
    {"path", document.path.string()}, {"text", document.text},
    {"cursor", {{"line", document.cursor.line}, {"column", document.cursor.column}}}
  });
  const auto temporary = file.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    const auto contents = nlohmann::json{{"version", 1}, {"documents", std::move(entries)}}.dump(2) + "\n";
    if (!output.write(contents.data(), static_cast<std::streamsize>(contents.size()))) {
      error = "Cannot write recovery file"; return false;
    }
  }
  std::filesystem::rename(temporary, file, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    error = "Cannot install recovery file: " + filesystem_error.message(); return false;
  }
  error.clear(); return true;
}

auto loadRecovery(const std::filesystem::path& file, std::vector<RecoveryDocument>& documents,
    std::string& error) -> bool {
  documents.clear(); error.clear();
  std::ifstream input(file, std::ios::binary);
  if (!input) return true;
  try {
    const auto json = nlohmann::json::parse(input);
    if (json.value("version", 0) != 1 || !json.contains("documents") || !json["documents"].is_array()) {
      error = "Unsupported recovery file"; return false;
    }
    for (const auto& entry : json["documents"]) {
      RecoveryDocument document;
      document.path = entry.value("path", std::string{});
      document.text = entry.at("text").get<std::string>();
      document.cursor.line = entry.at("cursor").value("line", std::size_t{});
      document.cursor.column = entry.at("cursor").value("column", std::size_t{});
      documents.push_back(std::move(document));
    }
  } catch (const nlohmann::json::exception& exception) {
    error = "Cannot parse recovery file: " + std::string(exception.what()); return false;
  }
  return true;
}

void clearRecovery(const std::filesystem::path& file) {
  if (file.empty()) return;
  std::error_code error;
  std::filesystem::remove(file, error);
  std::filesystem::remove(file.string() + ".tmp", error);
}

}  // namespace tuiide
