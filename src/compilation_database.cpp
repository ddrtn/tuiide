#include "tuiide/compilation_database.hpp"

#include "tuiide/document.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace tuiide {

auto CompilationDatabase::load(const std::filesystem::path& build_directory,
    std::string& error) -> bool {
  clear(); error.clear();
  path_ = normalizePath(build_directory / "compile_commands.json");
  std::ifstream input(path_, std::ios::binary);
  if (!input) return true;
  try {
    const auto commands = nlohmann::json::parse(input);
    if (!commands.is_array()) { error = "compile_commands.json must contain an array."; return false; }
    for (const auto& command : commands) {
      if (!command.is_object()) continue;
      const auto file = command.value("file", std::string{});
      if (file.empty()) continue;
      auto directory = std::filesystem::path(command.value("directory", build_directory.string()));
      if (directory.is_relative()) directory = build_directory / directory;
      auto source = std::filesystem::path(file);
      if (source.is_relative()) source = directory / source;
      sources_.insert(normalizePath(source));
    }
    available_ = true;
    return true;
  } catch (const nlohmann::json::exception& exception) {
    error = "Cannot parse " + path_.string() + ": " + exception.what();
    return false;
  }
}

void CompilationDatabase::clear() { path_.clear(); sources_.clear(); available_ = false; }
auto CompilationDatabase::available() const -> bool { return available_; }
auto CompilationDatabase::contains(const std::filesystem::path& source) const -> bool {
  return available_ && sources_.contains(normalizePath(source));
}
auto CompilationDatabase::size() const -> std::size_t { return sources_.size(); }
auto CompilationDatabase::path() const -> const std::filesystem::path& { return path_; }

}  // namespace tuiide
