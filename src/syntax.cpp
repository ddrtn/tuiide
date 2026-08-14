#include "tuiide/syntax.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <string>
#include <unordered_set>

namespace tuiide {
namespace {
const std::unordered_set<std::string> keywords{
  "alignas", "alignof", "asm", "auto", "break", "case", "catch", "class", "concept", "const",
  "consteval", "constexpr", "constinit", "continue", "co_await", "co_return", "co_yield", "default",
  "delete", "do", "else", "explicit", "export", "extern", "for", "friend", "goto", "if", "inline",
  "namespace", "new", "noexcept", "operator", "private", "protected", "public", "requires", "return",
  "sizeof", "static", "struct", "switch", "template", "this", "throw", "try", "typedef", "typeid",
  "typename", "union", "using", "virtual", "volatile", "while"};
const std::unordered_set<std::string> types{
  "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long", "short",
  "signed", "unsigned", "void", "wchar_t", "size_t", "nullptr", "true", "false"};
const std::unordered_set<std::string> cmake_control{
  "block", "break", "cmake_language", "continue", "else", "elseif", "endblock", "endforeach",
  "endfunction", "endif", "endmacro", "endwhile", "foreach", "function", "if", "macro", "return", "while"};
const std::unordered_set<std::string> cmake_properties{
  "ALIAS", "APPEND", "CACHE", "COMMAND", "CONFIGURATIONS", "EXCLUDE_FROM_ALL", "EXPORT", "FATAL_ERROR",
  "FILES", "IMPORTED", "INTERFACE", "LINK_PRIVATE", "LINK_PUBLIC", "MODULE", "OBJECT", "OPTIONAL", "PRIVATE",
  "PUBLIC", "REQUIRED", "SHARED", "STATIC", "STATUS", "SYSTEM", "TARGETS", "VERSION", "WARNING"};
const std::vector<std::string> cmake_words{
  "add_compile_definitions", "add_compile_options", "add_custom_command", "add_custom_target", "add_definitions",
  "add_dependencies", "add_executable", "add_library", "add_link_options", "add_subdirectory", "add_test",
  "aux_source_directory", "block", "break", "build_command", "cmake_host_system_information", "cmake_language",
  "cmake_minimum_required", "cmake_parse_arguments", "cmake_path", "cmake_policy", "configure_file", "continue",
  "create_test_sourcelist", "ctest_build", "ctest_configure", "ctest_coverage", "ctest_empty_binary_directory",
  "ctest_memcheck", "ctest_read_custom_files", "ctest_run_script", "ctest_sleep", "ctest_start", "ctest_submit",
  "ctest_test", "ctest_update", "ctest_upload", "define_property", "else", "elseif", "enable_language",
  "enable_testing", "endblock", "endforeach", "endfunction", "endif", "endmacro", "endwhile", "execute_process",
  "export", "file", "find_file", "find_library", "find_package", "find_path", "find_program", "fltk_wrap_ui",
  "foreach", "function", "get_cmake_property", "get_directory_property", "get_filename_component", "get_property",
  "get_source_file_property", "get_target_property", "get_test_property", "if", "include", "include_directories",
  "include_external_msproject", "include_guard", "install", "link_directories", "link_libraries", "list", "load_cache",
  "macro", "mark_as_advanced", "math", "message", "option", "project", "return", "separate_arguments",
  "set", "set_directory_properties", "set_property", "set_source_files_properties", "set_target_properties",
  "set_tests_properties", "site_name", "source_group", "string", "target_compile_definitions", "target_compile_features",
  "target_compile_options", "target_include_directories", "target_link_directories", "target_link_libraries",
  "target_link_options", "target_precompile_headers", "target_sources", "try_compile", "try_run", "unset",
  "variable_watch", "while", "write_file", "CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_STANDARD",
  "CMAKE_C_COMPILER", "CMAKE_C_STANDARD", "CMAKE_CURRENT_BINARY_DIR", "CMAKE_CURRENT_LIST_DIR",
  "CMAKE_CURRENT_LIST_FILE", "CMAKE_CURRENT_SOURCE_DIR", "CMAKE_INSTALL_PREFIX", "CMAKE_PROJECT_NAME",
  "CMAKE_SOURCE_DIR", "PROJECT_BINARY_DIR", "PROJECT_NAME", "PROJECT_SOURCE_DIR", "BUILD_SHARED_LIBS",
  "PRIVATE", "PUBLIC", "INTERFACE", "STATIC", "SHARED", "MODULE", "OBJECT", "IMPORTED", "ALIAS",
  "ON", "OFF", "TRUE", "FALSE", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"};
auto identifierStart(char c) -> bool { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
auto identifierPart(char c) -> bool { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

auto bracketOpening(std::string_view line, std::size_t position) -> std::optional<int> {
  if (position >= line.size() || line[position] != '[') return std::nullopt;
  std::size_t cursor = position + 1;
  while (cursor < line.size() && line[cursor] == '=') ++cursor;
  if (cursor >= line.size() || line[cursor] != '[') return std::nullopt;
  return static_cast<int>(cursor - position - 1);
}
}  // namespace

auto highlightCpp(std::string_view line, bool& in_block_comment) -> std::vector<Token> {
  std::vector<Token> result;
  std::size_t i{};
  if (!in_block_comment) {
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos && line[first] == '#') {
      result.push_back({first, line.size() - first, TokenKind::Preprocessor});
      return result;
    }
  }
  while (i < line.size()) {
    if (in_block_comment) {
      const auto end = line.find("*/", i);
      if (end == std::string_view::npos) { result.push_back({i, line.size() - i, TokenKind::Comment}); return result; }
      result.push_back({i, end + 2 - i, TokenKind::Comment}); i = end + 2; in_block_comment = false; continue;
    }
    if (line.substr(i, 2) == "//") { result.push_back({i, line.size() - i, TokenKind::Comment}); break; }
    if (line.substr(i, 2) == "/*") { in_block_comment = true; continue; }
    if (line[i] == '"' || line[i] == '\'') {
      const char quote = line[i]; const auto start = i++;
      while (i < line.size()) { if (line[i] == '\\') i += std::min<std::size_t>(2, line.size() - i); else if (line[i++] == quote) break; else {} }
      result.push_back({start, i - start, TokenKind::String}); continue;
    }
    if (std::isdigit(static_cast<unsigned char>(line[i])) != 0) {
      const auto start = i++;
      while (i < line.size() && (std::isalnum(static_cast<unsigned char>(line[i])) != 0 || line[i] == '.' || line[i] == '_')) ++i;
      result.push_back({start, i - start, TokenKind::Number}); continue;
    }
    if (identifierStart(line[i])) {
      const auto start = i++;
      while (i < line.size() && identifierPart(line[i])) ++i;
      const std::string word(line.substr(start, i - start));
      if (keywords.contains(word)) result.push_back({start, i - start, TokenKind::Keyword});
      else if (types.contains(word)) result.push_back({start, i - start, TokenKind::Type});
      continue;
    }
    ++i;
  }
  return result;
}

auto isCMakePath(const std::filesystem::path& path) -> bool {
  return path.filename() == "CMakeLists.txt" || path.extension() == ".cmake";
}

auto highlightCMake(std::string_view line, int& bracket_equals,
                    bool& bracket_comment) -> std::vector<Token> {
  std::vector<Token> result;
  std::size_t position{};
  while (position < line.size()) {
    if (bracket_equals >= 0) {
      const auto closing = "]" + std::string(static_cast<std::size_t>(bracket_equals), '=') + "]";
      const auto end = line.find(closing, position);
      const auto token_end = end == std::string_view::npos ? line.size() : end + closing.size();
      result.push_back({position, token_end - position, bracket_comment ? TokenKind::Comment : TokenKind::String});
      position = token_end;
      if (end == std::string_view::npos) return result;
      bracket_equals = -1;
      bracket_comment = false;
      continue;
    }
    if (line[position] == '#') {
      if (const auto equals = bracketOpening(line, position + 1)) {
        bracket_equals = *equals;
        bracket_comment = true;
        continue;
      }
      result.push_back({position, line.size() - position, TokenKind::Comment});
      break;
    }
    if (const auto equals = bracketOpening(line, position)) {
      bracket_equals = *equals;
      bracket_comment = false;
      continue;
    }
    if (line[position] == '"') {
      const auto begin = position++;
      while (position < line.size()) {
        if (line[position] == '\\' && position + 1 < line.size()) position += 2;
        else if (line[position++] == '"') break;
      }
      result.push_back({begin, position - begin, TokenKind::String});
      continue;
    }
    if (line[position] == '$' && position + 1 < line.size()
        && (line[position + 1] == '{' || line[position + 1] == '<'
            || line.substr(position, 5) == "$ENV{" || line.substr(position, 7) == "$CACHE{")) {
      const auto begin = position;
      const auto close = line[position + 1] == '<' ? '>' : '}';
      position = std::min(line.size(), line.find(close, position + 2) + 1);
      if (position == 0) position = line.size();
      result.push_back({begin, position - begin, close == '}' ? TokenKind::Variable : TokenKind::Macro});
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(line[position]))) {
      const auto begin = position++;
      while (position < line.size() && (std::isdigit(static_cast<unsigned char>(line[position])) || line[position] == '.')) ++position;
      result.push_back({begin, position - begin, TokenKind::Number});
      continue;
    }
    if (identifierStart(line[position])) {
      const auto begin = position++;
      while (position < line.size() && identifierPart(line[position])) ++position;
      const auto word = std::string(line.substr(begin, position - begin));
      auto lower = word;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
      auto next = position;
      while (next < line.size() && std::isspace(static_cast<unsigned char>(line[next]))) ++next;
      if (cmake_control.contains(lower)) result.push_back({begin, position - begin, TokenKind::Keyword});
      else if (next < line.size() && line[next] == '(') result.push_back({begin, position - begin, TokenKind::Function});
      else if (cmake_properties.contains(word)) result.push_back({begin, position - begin, TokenKind::Property});
      continue;
    }
    ++position;
  }
  return result;
}

auto completeCMake(const std::vector<std::string>& lines, std::size_t line,
                   std::size_t column) -> std::vector<std::string> {
  if (lines.empty()) return {};
  line = std::min(line, lines.size() - 1);
  column = std::min(column, lines[line].size());
  auto begin = column;
  while (begin > 0 && identifierPart(lines[line][begin - 1])) --begin;
  const auto prefix = lines[line].substr(begin, column - begin);
  std::unordered_set<std::string> values(cmake_words.begin(), cmake_words.end());
  for (const auto& source_line : lines) {
    for (std::size_t position = 0; position < source_line.size();) {
      if (!identifierStart(source_line[position])) { ++position; continue; }
      const auto word_begin = position++;
      while (position < source_line.size() && identifierPart(source_line[position])) ++position;
      const auto word = source_line.substr(word_begin, position - word_begin);
      if (word.size() >= 2) values.insert(word);
    }
  }
  std::vector<std::string> result;
  for (const auto& value : values)
    if (prefix.empty() || value.starts_with(prefix)) result.push_back(value);
  std::sort(result.begin(), result.end());
  if (result.size() > 200) result.resize(200);
  return result;
}

void CppSyntaxCache::clear() { entries_.clear(); dirty_line_ = 0; dirty_ = true; }

void CppSyntaxCache::invalidateFrom(std::size_t line) {
  if (!dirty_) { dirty_line_ = line; dirty_ = true; }
  else dirty_line_ = std::min(dirty_line_, line);
}

auto CppSyntaxCache::update(const std::vector<std::string>& lines) -> std::size_t {
  if (!dirty_ && entries_.size() == lines.size()) return 0;
  auto previous = std::move(entries_);
  entries_.clear();
  entries_.reserve(lines.size());
  const auto prefix = std::min({dirty_line_, previous.size(), lines.size()});
  entries_.insert(entries_.end(), std::make_move_iterator(previous.begin()),
    std::make_move_iterator(previous.begin() + static_cast<std::ptrdiff_t>(prefix)));
  bool in_comment = prefix > 0 && entries_[prefix - 1].ends_in_comment;
  const auto delta = static_cast<std::ptrdiff_t>(lines.size()) - static_cast<std::ptrdiff_t>(previous.size());
  std::size_t recomputed{};
  for (std::size_t line = prefix; line < lines.size(); ++line) {
    const auto mapped = static_cast<std::ptrdiff_t>(line) - delta;
    const auto hash = fingerprint(lines[line]);
    if (mapped >= static_cast<std::ptrdiff_t>(prefix) && mapped < static_cast<std::ptrdiff_t>(previous.size())) {
      const auto& cached = previous[static_cast<std::size_t>(mapped)];
      if (cached.length == lines[line].size() && cached.fingerprint == hash && cached.starts_in_comment == in_comment) {
        entries_.insert(entries_.end(),
          std::make_move_iterator(previous.begin() + mapped), std::make_move_iterator(previous.end()));
        break;
      }
    }
    const bool starts_in_comment = in_comment;
    auto tokens = highlightCpp(lines[line], in_comment);
    entries_.push_back({hash, lines[line].size(), starts_in_comment, in_comment, std::move(tokens)});
    ++recomputed;
  }
  dirty_ = false;
  dirty_line_ = lines.size();
  return recomputed;
}

auto CppSyntaxCache::tokens(std::size_t line) const -> const std::vector<Token>& {
  static const std::vector<Token> empty;
  return line < entries_.size() ? entries_[line].tokens : empty;
}

auto CppSyntaxCache::fingerprint(std::string_view line) -> std::size_t {
  std::size_t value = sizeof(std::size_t) == 8 ? static_cast<std::size_t>(1469598103934665603ULL)
                                               : static_cast<std::size_t>(2166136261U);
  const auto prime = sizeof(std::size_t) == 8 ? static_cast<std::size_t>(1099511628211ULL)
                                              : static_cast<std::size_t>(16777619U);
  for (const unsigned char byte : line) { value ^= byte; value *= prime; }
  return value;
}

void CMakeSyntaxCache::clear() { entries_.clear(); dirty_ = true; }
void CMakeSyntaxCache::invalidateFrom(std::size_t) { dirty_ = true; }

auto CMakeSyntaxCache::update(const std::vector<std::string>& lines) -> std::size_t {
  if (!dirty_ && entries_.size() == lines.size()) return 0;
  entries_.clear();
  entries_.reserve(lines.size());
  int bracket_equals{-1};
  bool bracket_comment{};
  for (const auto& line : lines) entries_.push_back(highlightCMake(line, bracket_equals, bracket_comment));
  dirty_ = false;
  return lines.size();
}

auto CMakeSyntaxCache::tokens(std::size_t line) const -> const std::vector<Token>& {
  static const std::vector<Token> empty;
  return line < entries_.size() ? entries_[line] : empty;
}

}  // namespace tuiide
