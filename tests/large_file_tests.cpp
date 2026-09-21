#include "tuiide/document.hpp"
#include "tuiide/syntax.hpp"
#include "tuiide/text_search.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct TemporaryDirectory {
  std::filesystem::path path;
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

}  // namespace

int main(int argc, char** argv) {
  const bool moderate = argc == 2 && std::string_view(argv[1]) == "--moderate";
  expect(argc == 1 || moderate, "large-file test accepts only --moderate");
  // Быстрый профиль проверяет те же операции; полный объём остаётся opt-in.
  const std::size_t line_count = moderate ? 12000 : 80000;
  const std::size_t syntax_line_count = moderate ? 9000 : 60000;
  const std::size_t changed_line = syntax_line_count * 3 / 4;
  TemporaryDirectory temporary{std::filesystem::temp_directory_path()
    / ("tuiide-large-file-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()))};
  std::filesystem::create_directories(temporary.path);
  const auto source = temporary.path / "large UTF-8 source.cpp";
  {
    std::ofstream output(source, std::ios::binary);
    for (std::size_t line{}; line < line_count; ++line) {
      output << "int value_" << line << " = " << line << "; // строка\n";
    }
  }
  expect(std::filesystem::file_size(source) > 256 * 1024,
    "large-file fixture exceeds a quarter mebibyte");

  tuiide::Document document;
  std::string error;
  expect(document.load(source, error) && document.lines().size() == line_count + 1
      && document.hasFinalNewline() && !document.modified(),
    "large UTF-8 document loads with all lines and its clean state");
  document.setCursor({line_count - 1, document.line(line_count - 1).size()});
  document.insert(" // LARGE_EDIT_MARKER");
  expect(document.modified() && document.undo() && !document.modified()
      && document.redo() && document.modified(),
    "tail editing of a large document preserves undo, redo, and dirty state");
  expect(document.save(error) && !document.modified(),
    "large document saves atomically and marks its current history state clean");

  tuiide::Document reloaded;
  expect(reloaded.load(source, error)
      && reloaded.text().find("LARGE_EDIT_MARKER") != std::string::npos,
    "large atomic save can be loaded without truncation");
  const auto matches = tuiide::searchText(reloaded.text(),
    "value_" + std::to_string(line_count - 1), "",
    tuiide::SearchOptions{}, error);
  expect(matches.size() == 1 && matches.front().start.line == line_count - 1,
    "large-file search locates a unique match near the end of the document");

  std::vector<std::string> syntax_lines;
  syntax_lines.reserve(syntax_line_count);
  for (std::size_t line{}; line < syntax_line_count; ++line)
    syntax_lines.push_back("constexpr int item_" + std::to_string(line) + " = "
      + std::to_string(line) + ";");
  tuiide::CppSyntaxCache cache;
  expect(cache.update(syntax_lines) == syntax_lines.size(),
    "large syntax cache performs a complete initial scan");
  syntax_lines[changed_line] = "constexpr auto changed = \"UTF-8: данные\";";
  cache.invalidateFrom(changed_line);
  expect(cache.update(syntax_lines) == 1,
    "large syntax cache stabilizes after one changed line");

  std::cout << (moderate ? "Large-file regression passed\n" : "Large-file stress test passed\n");
}
