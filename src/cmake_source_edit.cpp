#include "tuiide/cmake_source_edit.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace tuiide {
namespace {

struct TextEdit {
  std::size_t begin{};
  std::size_t end{};
};

auto bracketEnd(std::string_view text, std::size_t opening) -> std::optional<std::size_t> {
  if (opening >= text.size() || text[opening] != '[') return std::nullopt;
  auto marker_end = opening + 1;
  while (marker_end < text.size() && text[marker_end] == '=') ++marker_end;
  if (marker_end >= text.size() || text[marker_end] != '[') return std::nullopt;
  const auto equals = marker_end - opening - 1;
  const auto closing = "]" + std::string(equals, '=') + "]";
  const auto end = text.find(closing, marker_end + 1);
  return end == std::string_view::npos ? text.size() : end + closing.size();
}

auto normalized(const std::filesystem::path& path) -> std::filesystem::path {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : result;
}

auto isInside(const std::filesystem::path& root, const std::filesystem::path& path) -> bool {
  std::error_code error;
  const auto relative = std::filesystem::relative(path, root, error);
  if (error || relative.empty()) return false;
  return *relative.begin() != "..";
}

auto candidatesFor(const std::filesystem::path& cmake_file,
                   const std::filesystem::path& source) -> std::set<std::string> {
  std::set<std::string> candidates{source.generic_string()};
  std::error_code error;
  const auto relative = std::filesystem::relative(source, cmake_file.parent_path(), error);
  if (!error && !relative.empty()) {
    const auto value = relative.generic_string();
    candidates.insert(value);
    candidates.insert("./" + value);
    candidates.insert("${CMAKE_CURRENT_SOURCE_DIR}/" + value);
  }
  return candidates;
}

auto removeArguments(std::string_view text, const std::set<std::string>& candidates,
                     std::size_t& count) -> std::string {
  std::vector<TextEdit> edits;
  std::size_t position{};
  while (position < text.size()) {
    if (text[position] == '#') {
      if (const auto bracket = bracketEnd(text, position + 1)) {
        position = *bracket;
        continue;
      }
      const auto newline = text.find('\n', position);
      position = newline == std::string_view::npos ? text.size() : newline + 1;
      continue;
    }
    if (const auto bracket = bracketEnd(text, position)) {
      position = *bracket;
      continue;
    }
    if (text[position] == '"') {
      const auto begin = position++;
      std::string value;
      while (position < text.size() && text[position] != '"') {
        if (text[position] == '\\' && position + 1 < text.size()) ++position;
        value.push_back(text[position++]);
      }
      if (position < text.size()) ++position;
      if (candidates.contains(value)) edits.push_back({begin, position});
      continue;
    }
    const auto ch = text[position];
    if (ch == '(' || ch == ')' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      ++position;
      continue;
    }
    const auto begin = position;
    while (position < text.size()) {
      const auto current = text[position];
      if (current == '#' || current == '"' || current == '(' || current == ')'
          || current == ' ' || current == '\t' || current == '\r' || current == '\n') break;
      ++position;
    }
    if (candidates.contains(std::string(text.substr(begin, position - begin))))
      edits.push_back({begin, position});
  }

  count = edits.size();
  std::string output(text);
  for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit) {
    auto begin = edit->begin;
    auto end = edit->end;
    if (begin > 0 && (output[begin - 1] == ' ' || output[begin - 1] == '\t')) --begin;
    else if (end < output.size() && (output[end] == ' ' || output[end] == '\t')) ++end;
    output.erase(begin, end - begin);
  }
  return output;
}

auto replaceArguments(std::string_view text, const std::map<std::string, std::string>& replacements,
                      std::size_t& count) -> std::string {
  struct Replacement { std::size_t begin{}; std::size_t end{}; std::string value; };
  std::vector<Replacement> edits;
  std::size_t position{};
  while (position < text.size()) {
    if (text[position] == '#') {
      if (const auto bracket = bracketEnd(text, position + 1)) { position = *bracket; continue; }
      const auto newline = text.find('\n', position);
      position = newline == std::string_view::npos ? text.size() : newline + 1;
      continue;
    }
    if (const auto bracket = bracketEnd(text, position)) { position = *bracket; continue; }
    if (text[position] == '"') {
      ++position;
      const auto begin = position;
      std::string value;
      while (position < text.size() && text[position] != '"') {
        if (text[position] == '\\' && position + 1 < text.size()) ++position;
        value.push_back(text[position++]);
      }
      const auto found = replacements.find(value);
      if (found != replacements.end() && found->second != value)
        edits.push_back({begin, position, found->second});
      if (position < text.size()) ++position;
      continue;
    }
    const auto character = text[position];
    if (character == '(' || character == ')' || std::isspace(static_cast<unsigned char>(character))) {
      ++position; continue;
    }
    const auto begin = position;
    while (position < text.size()) {
      const auto current = text[position];
      if (current == '#' || current == '"' || current == '(' || current == ')'
          || std::isspace(static_cast<unsigned char>(current))) break;
      ++position;
    }
    const auto value = std::string(text.substr(begin, position - begin));
    const auto found = replacements.find(value);
    if (found != replacements.end() && found->second != value)
      edits.push_back({begin, position, found->second});
  }
  count = edits.size();
  std::string output(text);
  for (auto edit = edits.rbegin(); edit != edits.rend(); ++edit)
    output.replace(edit->begin, edit->end - edit->begin, edit->value);
  return output;
}

auto writeFile(const std::filesystem::path& path, const std::string& content,
               std::string& error) -> bool {
  auto temporary = path;
  temporary += ".tuiide.tmp";
  std::error_code filesystem_error;
  if (std::filesystem::exists(temporary, filesystem_error)) {
    error = "Temporary file already exists: " + temporary.string();
    return false;
  }
  {
    std::ofstream output(temporary, std::ios::binary);
    if (!output || !output.write(content.data(), static_cast<std::streamsize>(content.size()))) {
      error = "Cannot write temporary file: " + temporary.string();
      std::filesystem::remove(temporary, filesystem_error);
      return false;
    }
  }
  const auto permissions = std::filesystem::status(path, filesystem_error).permissions();
  if (!filesystem_error) std::filesystem::permissions(temporary, permissions, filesystem_error);
  filesystem_error.clear();
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    error = "Cannot replace " + path.string() + ": " + filesystem_error.message();
    std::filesystem::remove(temporary, filesystem_error);
    return false;
  }
  return true;
}

auto skipQuotedOrBracket(std::string_view text, std::size_t position) -> std::size_t {
  if (const auto bracket = bracketEnd(text, position)) return *bracket;
  if (position >= text.size() || text[position] != '"') return position;
  ++position;
  while (position < text.size()) {
    if (text[position] == '\\' && position + 1 < text.size()) position += 2;
    else if (text[position++] == '"') break;
  }
  return position;
}

struct CMakeCall {
  std::string command;
  std::string target;
  std::size_t close{};
};

auto findCalls(std::string_view text) -> std::vector<CMakeCall> {
  std::vector<CMakeCall> calls;
  std::size_t position{};
  while (position < text.size()) {
    if (text[position] == '#') {
      if (const auto bracket = bracketEnd(text, position + 1)) position = *bracket;
      else {
        const auto newline = text.find('\n', position);
        position = newline == std::string_view::npos ? text.size() : newline + 1;
      }
      continue;
    }
    const auto skipped = skipQuotedOrBracket(text, position);
    if (skipped != position) { position = skipped; continue; }
    if (!std::isalpha(static_cast<unsigned char>(text[position])) && text[position] != '_') {
      ++position;
      continue;
    }
    const auto command_begin = position++;
    while (position < text.size()
           && (std::isalnum(static_cast<unsigned char>(text[position])) || text[position] == '_')) ++position;
    auto command = std::string(text.substr(command_begin, position - command_begin));
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size() || text[position] != '(') continue;
    const auto opening = position++;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    const auto target_begin = position;
    while (position < text.size() && !std::isspace(static_cast<unsigned char>(text[position]))
           && text[position] != ')' && text[position] != '(') ++position;
    const auto target = std::string(text.substr(target_begin, position - target_begin));
    std::size_t depth{1};
    position = opening + 1;
    while (position < text.size() && depth > 0) {
      if (text[position] == '#') {
        if (const auto bracket = bracketEnd(text, position + 1)) position = *bracket;
        else {
          const auto newline = text.find('\n', position);
          position = newline == std::string_view::npos ? text.size() : newline + 1;
        }
        continue;
      }
      const auto argument_end = skipQuotedOrBracket(text, position);
      if (argument_end != position) { position = argument_end; continue; }
      if (text[position] == '(') ++depth;
      else if (text[position] == ')') --depth;
      ++position;
    }
    if (depth == 0 && (command == "target_sources" || command == "add_executable" || command == "add_library"))
      calls.push_back({std::move(command), target, position - 1});
  }
  return calls;
}

auto containsArgument(std::string_view text, std::string_view argument) -> bool {
  std::size_t position{};
  while (position < text.size()) {
    if (text[position] == '#') {
      const auto newline = text.find('\n', position);
      position = newline == std::string_view::npos ? text.size() : newline + 1;
      continue;
    }
    if (text[position] == '"') {
      const auto begin = ++position;
      while (position < text.size() && text[position] != '"') {
        if (text[position] == '\\' && position + 1 < text.size()) ++position;
        ++position;
      }
      if (text.substr(begin, position - begin) == argument) return true;
      if (position < text.size()) ++position;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(text[position])) || text[position] == '(' || text[position] == ')') {
      ++position;
      continue;
    }
    const auto begin = position++;
    while (position < text.size() && !std::isspace(static_cast<unsigned char>(text[position]))
           && text[position] != '(' && text[position] != ')' && text[position] != '#') ++position;
    if (text.substr(begin, position - begin) == argument) return true;
  }
  return false;
}

}  // namespace

auto removeCMakeSourceReferences(const std::filesystem::path& project_root,
                                 const std::filesystem::path& source_path,
                                 CMakeSourceRemoval& result,
                                 std::string& error) -> bool {
  result = {};
  error.clear();
  const auto root = normalized(project_root);
  const auto source = normalized(source_path);
  if (!isInside(root, source)) {
    error = "The selected file is outside the project root.";
    return false;
  }

  std::vector<std::pair<std::filesystem::path, std::string>> changes;
  std::error_code iterator_error;
  for (std::filesystem::recursive_directory_iterator iterator(
           root, std::filesystem::directory_options::skip_permission_denied, iterator_error), end;
       iterator != end; iterator.increment(iterator_error)) {
    if (iterator_error) {
      iterator_error.clear();
      continue;
    }
    if (iterator->is_directory() && iterator->path().filename() == ".git") {
      iterator.disable_recursion_pending();
      continue;
    }
    if (!iterator->is_regular_file() || iterator->path().filename() != "CMakeLists.txt") continue;
    std::ifstream input(iterator->path(), std::ios::binary);
    if (!input) {
      error = "Cannot read " + iterator->path().string();
      return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::size_t removed{};
    auto updated = removeArguments(text, candidatesFor(iterator->path(), source), removed);
    if (removed == 0) continue;
    result.references_removed += removed;
    changes.emplace_back(iterator->path(), std::move(updated));
  }
  if (iterator_error) {
    error = "Cannot scan project: " + iterator_error.message();
    return false;
  }
  for (const auto& [path, content] : changes) {
    if (!writeFile(path, content, error)) return false;
    result.changed_files.push_back(path);
  }
  return true;
}

auto addCMakeSourceReferences(const std::filesystem::path& project_root,
                              const std::vector<std::filesystem::path>& source_paths,
                              std::string_view preferred_target,
                              CMakeSourceAddition& result,
                              std::string& error) -> bool {
  result = {};
  error.clear();
  if (source_paths.empty()) { error = "No source files were provided."; return false; }
  const auto root = normalized(project_root);
  std::vector<std::filesystem::path> sources;
  sources.reserve(source_paths.size());
  for (const auto& path : source_paths) {
    auto source = normalized(path);
    if (!isInside(root, source)) { error = "A source file is outside the project root."; return false; }
    sources.push_back(std::move(source));
  }

  const int passes = preferred_target.empty() ? 1 : 2;
  for (int pass = 0; pass < passes; ++pass) {
    auto directory = sources.front().parent_path();
    while (isInside(root, directory) || directory == root) {
      const auto cmake_file = directory / "CMakeLists.txt";
      std::ifstream input(cmake_file, std::ios::binary);
      if (input) {
        const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto calls = findCalls(text);
        const CMakeCall* selected{};
        for (const auto& call : calls) {
          const bool target_matches = call.target == preferred_target;
          if ((pass == 0 && (preferred_target.empty() || target_matches)) || pass == 1) {
            if (!selected) selected = &call;
            if (target_matches && call.command == "target_sources") break;
          }
        }
        if (selected) {
          std::string insertion;
          for (const auto& source : sources) {
            std::error_code relative_error;
            const auto relative = std::filesystem::relative(source, directory, relative_error).generic_string();
            if (relative_error) { error = "Cannot make a relative CMake source path."; return false; }
            if (!containsArgument(text, relative)) {
              insertion += "\n  " + relative;
              ++result.references_added;
            }
          }
          if (insertion.empty()) { result.changed_file = cmake_file; return true; }
          auto updated = text;
          updated.insert(selected->close, insertion + "\n");
          if (!writeFile(cmake_file, updated, error)) return false;
          result.changed_file = cmake_file;
          return true;
        }
      }
      if (directory == root) break;
      directory = directory.parent_path();
    }
  }
  error = "No add_executable(), add_library(), or target_sources() call was found in an enclosing CMakeLists.txt.";
  return false;
}

auto renameCMakeSourceReferences(const std::filesystem::path& project_root,
    const std::vector<CMakePathRename>& renames, CMakeSourceRename& result,
    std::string& error) -> bool {
  result = {}; error.clear();
  const auto root = normalized(project_root);
  if (renames.empty()) return true;
  std::vector<CMakePathRename> paths;
  paths.reserve(renames.size());
  for (const auto& rename : renames) {
    const auto old_path = normalized(rename.old_path);
    const auto new_path = normalized(rename.new_path);
    if (!isInside(root, old_path) || !isInside(root, new_path)) {
      error = "CMake rename paths must stay inside the project root."; return false;
    }
    paths.push_back({old_path, new_path});
  }
  struct Change { std::filesystem::path path; std::string original; std::string updated; };
  std::vector<Change> changes;
  std::error_code iterator_error;
  for (std::filesystem::recursive_directory_iterator iterator(root,
         std::filesystem::directory_options::skip_permission_denied, iterator_error), end;
       iterator != end; iterator.increment(iterator_error)) {
    if (iterator_error) { iterator_error.clear(); continue; }
    if (iterator->is_directory() && iterator->path().filename() == ".git") {
      iterator.disable_recursion_pending(); continue;
    }
    if (!iterator->is_regular_file() || iterator->path().filename() != "CMakeLists.txt") continue;
    const auto cmake_path = normalized(iterator->path());
    auto original_cmake_path = cmake_path;
    for (const auto& rename : paths) {
      if (cmake_path == rename.new_path) { original_cmake_path = rename.old_path; break; }
    }
    std::ifstream input(cmake_path, std::ios::binary);
    if (!input) { error = "Cannot read " + cmake_path.string(); return false; }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::map<std::string, std::string> replacements;
    for (const auto& rename : paths) {
      replacements[rename.old_path.generic_string()] = rename.new_path.generic_string();
      std::error_code old_error, new_error;
      const auto old_relative = std::filesystem::relative(rename.old_path, original_cmake_path.parent_path(), old_error).generic_string();
      const auto new_relative = std::filesystem::relative(rename.new_path, cmake_path.parent_path(), new_error).generic_string();
      if (!old_error && !new_error) {
        replacements[old_relative] = new_relative;
        replacements["./" + old_relative] = "./" + new_relative;
        replacements["${CMAKE_CURRENT_SOURCE_DIR}/" + old_relative]
          = "${CMAKE_CURRENT_SOURCE_DIR}/" + new_relative;
      }
    }
    std::size_t changed{};
    auto updated = replaceArguments(text, replacements, changed);
    if (changed != 0) {
      result.references_changed += changed;
      changes.push_back({cmake_path, text, std::move(updated)});
    }
  }
  std::vector<const Change*> saved;
  for (const auto& change : changes) {
    if (!writeFile(change.path, change.updated, error)) {
      bool rollback_failed{};
      for (auto saved_change = saved.rbegin(); saved_change != saved.rend(); ++saved_change) {
        std::string rollback_error;
        if (!writeFile((*saved_change)->path, (*saved_change)->original, rollback_error)) rollback_failed = true;
      }
      error += rollback_failed ? "; rollback of CMake files failed" : "; CMake files were restored";
      result = {};
      return false;
    }
    saved.push_back(&change);
    result.changed_files.push_back(change.path);
  }
  return true;
}

}  // namespace tuiide
