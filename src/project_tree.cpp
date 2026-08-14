#include "tuiide/project_tree.hpp"

#include "tuiide/document.hpp"

#include <algorithm>
#include <cctype>

namespace tuiide {
namespace {
auto inside(const std::filesystem::path& root, const std::filesystem::path& path,
    bool allow_root = false) -> bool {
  std::error_code error;
  const auto relative = std::filesystem::relative(normalizePath(path), normalizePath(root), error);
  if (error || relative.empty()) return false;
  if (relative == ".") return allow_root;
  return *relative.begin() != "..";
}

auto lower(std::string value) -> std::string {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

auto editable(const std::filesystem::path& path) -> bool {
  static const std::vector<std::string> extensions{
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp", ".cmake", ".txt"
  };
  return path.filename() == "CMakeLists.txt"
    || std::find(extensions.begin(), extensions.end(), path.extension().string()) != extensions.end();
}
}  // namespace

auto scanProjectTree(const std::filesystem::path& project_root,
    const std::filesystem::path& build_directory, std::string_view filter,
    ProjectTreeSnapshot& snapshot, std::string& error) -> bool {
  snapshot = {}; error.clear();
  const auto root = normalizePath(project_root);
  const auto build = build_directory.empty() ? std::filesystem::path{} : normalizePath(build_directory);
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(root, filesystem_error)) { error = "Project root does not exist."; return false; }
  const auto needle = lower(std::string(filter));
  for (std::filesystem::recursive_directory_iterator iterator(root,
         std::filesystem::directory_options::skip_permission_denied, filesystem_error), end;
       iterator != end; iterator.increment(filesystem_error)) {
    if (filesystem_error) { ++snapshot.skipped_errors; filesystem_error.clear(); continue; }
    const auto path = normalizePath(iterator->path());
    if (iterator->is_directory()) {
      if (path == build || path.filename() == ".git") { iterator.disable_recursion_pending(); continue; }
      std::error_code relative_error;
      const auto label = std::filesystem::relative(path, root, relative_error).generic_string();
      if (needle.empty() || (!relative_error && lower(label).find(needle) != std::string::npos))
        snapshot.entries.push_back({path, true});
      continue;
    }
    if (!iterator->is_regular_file()) continue;
    ++snapshot.scanned_files;
    if (editable(path)) snapshot.editable_files.push_back(path);
    std::error_code relative_error;
    const auto label = std::filesystem::relative(path, root, relative_error).generic_string();
    if (needle.empty() || (!relative_error && lower(label).find(needle) != std::string::npos))
      snapshot.entries.push_back({path, false});
  }
  std::sort(snapshot.entries.begin(), snapshot.entries.end(), [](const auto& left, const auto& right) {
    return left.path < right.path;
  });
  std::sort(snapshot.editable_files.begin(), snapshot.editable_files.end());
  return true;
}

auto createProjectDirectory(const std::filesystem::path& project_root,
    const std::filesystem::path& path, std::string& error) -> bool {
  error.clear(); const auto root = normalizePath(project_root); const auto target = normalizePath(path);
  if (!inside(root, target)) { error = "Directory path must be inside the project root."; return false; }
  std::error_code filesystem_error;
  if (std::filesystem::exists(target, filesystem_error)) { error = "Project path already exists."; return false; }
  if (!std::filesystem::create_directories(target, filesystem_error) || filesystem_error) {
    error = "Cannot create project directory: " + filesystem_error.message(); return false;
  }
  return true;
}

auto deleteEmptyProjectDirectory(const std::filesystem::path& project_root,
    const std::filesystem::path& path, std::string& error) -> bool {
  error.clear(); const auto root = normalizePath(project_root); const auto target = normalizePath(path);
  if (!inside(root, target)) { error = "Select a directory inside the project root."; return false; }
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(target, filesystem_error)) { error = "Selected path is not a directory."; return false; }
  if (!std::filesystem::is_empty(target, filesystem_error) || filesystem_error) {
    error = "Only empty directories can be deleted safely."; return false;
  }
  if (!std::filesystem::remove(target, filesystem_error) || filesystem_error) {
    error = "Cannot delete project directory: " + filesystem_error.message(); return false;
  }
  return true;
}

auto renameProjectEntry(const std::filesystem::path& project_root,
    const std::filesystem::path& source, const std::filesystem::path& destination,
    std::string& error) -> bool {
  error.clear(); const auto root = normalizePath(project_root);
  const auto from = normalizePath(source); const auto to = normalizePath(destination);
  if (!inside(root, from) || !inside(root, to)) { error = "Source and destination must be inside the project root."; return false; }
  std::error_code filesystem_error;
  if (!std::filesystem::exists(from, filesystem_error)) { error = "Selected project entry no longer exists."; return false; }
  if (std::filesystem::exists(to, filesystem_error)) { error = "Destination path already exists."; return false; }
  if (!std::filesystem::is_directory(to.parent_path(), filesystem_error)) {
    error = "Destination parent directory does not exist."; return false;
  }
  std::filesystem::rename(from, to, filesystem_error);
  if (filesystem_error) { error = "Cannot rename project entry: " + filesystem_error.message(); return false; }
  return true;
}

auto moveProjectEntryWithCMake(const std::filesystem::path& project_root,
    const std::filesystem::path& source, const std::filesystem::path& destination,
    CMakeSourceRename& result, std::string& error) -> bool {
  result = {}; error.clear();
  const auto root = normalizePath(project_root);
  const auto from = normalizePath(source);
  const auto to = normalizePath(destination);
  std::vector<CMakePathRename> renames{{from, to}};
  std::error_code filesystem_error;
  if (std::filesystem::is_directory(from, filesystem_error)) {
    for (std::filesystem::recursive_directory_iterator iterator(from,
           std::filesystem::directory_options::skip_permission_denied, filesystem_error), end;
         iterator != end; iterator.increment(filesystem_error)) {
      if (filesystem_error) { error = "Cannot scan entry before move: " + filesystem_error.message(); return false; }
      std::error_code relative_error;
      const auto relative = std::filesystem::relative(iterator->path(), from, relative_error);
      if (relative_error) { error = "Cannot resolve an entry before move."; return false; }
      renames.push_back({normalizePath(iterator->path()), normalizePath(to / relative)});
    }
    if (filesystem_error) {
      error = "Cannot scan entry before move: " + filesystem_error.message();
      return false;
    }
  }
  if (!renameProjectEntry(root, from, to, error)) return false;
  if (renameCMakeSourceReferences(root, renames, result, error)) return true;
  std::error_code rollback_error;
  std::filesystem::rename(to, from, rollback_error);
  if (rollback_error) error += "; filesystem rollback failed: " + rollback_error.message();
  else error += "; filesystem entry was restored";
  return false;
}

}  // namespace tuiide
