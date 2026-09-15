#include "tuiide/user_settings.hpp"

#include "tuiide/document.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tuiide {
namespace {
const std::vector<std::string> themes{"Dark", "Light", "High contrast"};
const std::vector<std::string> color_roles{"foreground", "background", "gutter", "breakpoint",
  "diagnosticError", "diagnosticWarning", "diagnosticNote", "selectionForeground",
  "selectionBackground", "executionLineBackground", "plain", "keyword", "type", "string", "number", "comment",
  "preprocessor", "namespace", "function", "variable", "parameter", "property", "macro",
  "enumMember"};
const std::vector<std::string> color_names{"Black", "Blue", "Green", "Cyan", "Red", "Magenta",
  "Brown", "LightGray", "DarkGray", "LightBlue", "LightGreen", "LightCyan", "LightRed",
  "LightMagenta", "Yellow", "White"};
}  // namespace

auto reservedShortcutReason(std::string shortcut) -> std::string {
  shortcut.erase(std::remove_if(shortcut.begin(), shortcut.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }), shortcut.end());
  std::transform(shortcut.begin(), shortcut.end(), shortcut.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  if (shortcut == "CTRL+L") return "Ctrl+L is reserved by Final Cut for screen redraw.";
  if (shortcut == "F10") return "F10 is reserved for the menu bar.";
  if (shortcut == "ALT+K") return "Alt+K is reserved for the command palette.";
  static const std::vector<std::string> menus{
    "ALT+F", "ALT+E", "ALT+S", "ALT+R", "ALT+P", "ALT+D", "ALT+T", "ALT+W", "ALT+H"};
  if (std::find(menus.begin(), menus.end(), shortcut) != menus.end())
    return shortcut + " is reserved for a top-level menu.";
  return {};
}

auto normalizeRecentFiles(const std::vector<std::filesystem::path>& files, std::size_t limit)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> result;
  if (limit == 0) return result;
  std::unordered_set<std::filesystem::path> seen;
  for (const auto& file : files) {
    const auto normalized = normalizePath(file);
    std::error_code error;
    if (normalized.empty() || !std::filesystem::is_regular_file(normalized, error)
        || !seen.insert(normalized).second)
      continue;
    result.push_back(normalized);
    if (result.size() == limit) break;
  }
  return result;
}

void rememberRecentFile(std::vector<std::filesystem::path>& files,
    const std::filesystem::path& file, std::size_t limit) {
  const auto normalized = normalizePath(file);
  std::error_code error;
  if (normalized.empty() || !std::filesystem::is_regular_file(normalized, error)) return;
  files.erase(std::remove(files.begin(), files.end(), normalized), files.end());
  files.insert(files.begin(), normalized);
  files = normalizeRecentFiles(files, limit);
}

auto defaultUserSettingsPath() -> std::filesystem::path {
  if (const auto* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
    return std::filesystem::path(xdg) / "tuiide" / "settings.json";
  if (const auto* home = std::getenv("HOME"); home && *home)
    return std::filesystem::path(home) / ".config" / "tuiide" / "settings.json";
  return {};
}

auto validateUserSettings(const UserSettings& settings, std::string& error) -> bool {
  error.clear();
  if (settings.version != 1) {
    error = "Unsupported user settings version: " + std::to_string(settings.version);
    return false;
  }
  for (const auto& [command, shortcut] : settings.shortcuts) {
    if (const auto reason = reservedShortcutReason(shortcut); !reason.empty()) {
      error = command + ": " + reason;
      return false;
    }
  }
  for (const auto& [name, base] : settings.custom_themes) {
    if (name.empty() || std::find(themes.begin(), themes.end(), name) != themes.end()
        || std::find(themes.begin(), themes.end(), base) == themes.end()) {
      error = "Invalid custom editor theme: " + name;
      return false;
    }
  }
  if (std::find(themes.begin(), themes.end(), settings.theme) == themes.end()
      && !settings.custom_themes.contains(settings.theme)) {
    error = "Unsupported editor theme.";
    return false;
  }
  for (const auto& [role, color] : settings.colors) {
    if (std::find(color_roles.begin(), color_roles.end(), role) == color_roles.end()) {
      error = "Unsupported editor color role: " + role;
      return false;
    }
    if (std::find(color_names.begin(), color_names.end(), color) == color_names.end()) {
      error = "Unsupported editor color: " + color;
      return false;
    }
  }
  return true;
}

auto loadUserSettings(const std::filesystem::path& path, UserSettings& settings,
    std::string& error, std::vector<std::string>* warnings) -> bool {
  settings = {};
  error.clear();
  if (warnings) warnings->clear();
  if (path.empty() || !std::filesystem::exists(path)) return true;
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    error = "Cannot read user settings: " + path.string();
    return false;
  }
  try {
    const auto json = nlohmann::json::parse(input);
    settings.version = json.value("version", 0);
    settings.shortcuts = json.value("shortcuts", std::map<std::string, std::string>{});
    settings.theme = json.value("theme", std::string("Dark"));
    settings.custom_themes = json.value("customThemes", std::map<std::string, std::string>{});
    settings.colors = json.value("colors", std::map<std::string, std::string>{});
    std::vector<std::filesystem::path> recent_files;
    if (const auto iterator = json.find("recentFiles"); iterator != json.end() && iterator->is_array()) {
      for (const auto& value : *iterator)
        if (value.is_string()) recent_files.emplace_back(value.get<std::string>());
    }
    settings.recent_files = normalizeRecentFiles(recent_files);
  } catch (const nlohmann::json::exception& exception) {
    error = "Cannot parse user settings: " + std::string(exception.what());
    return false;
  }
  // Старые ошибочные назначения не должны лишать пользователя темы и истории.
  std::vector<std::string> ignored;
  for (auto iterator = settings.shortcuts.begin(); iterator != settings.shortcuts.end();) {
    const auto reason = reservedShortcutReason(iterator->second);
    if (reason.empty()) { ++iterator; continue; }
    ignored.push_back("Ignored shortcut " + iterator->first + ": " + reason);
    iterator = settings.shortcuts.erase(iterator);
  }
  if (!validateUserSettings(settings, error)) return false;
  if (warnings) *warnings = std::move(ignored);
  return true;
}

auto saveUserSettings(const std::filesystem::path& path, const UserSettings& settings,
    std::string& error) -> bool {
  if (path.empty()) {
    error = "User settings path is unavailable.";
    return false;
  }
  if (!validateUserSettings(settings, error)) return false;
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = "Cannot create user settings directory: " + filesystem_error.message();
    return false;
  }
  auto recent_files = nlohmann::json::array();
  for (const auto& file : normalizeRecentFiles(settings.recent_files))
    recent_files.push_back(file.string());
  const nlohmann::json json{{"version", settings.version}, {"shortcuts", settings.shortcuts},
    {"theme", settings.theme}, {"customThemes", settings.custom_themes},
    {"colors", settings.colors}, {"recentFiles", std::move(recent_files)}};
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
    if (!output) {
      error = "Cannot write temporary user settings: " + temporary;
      return false;
    }
  }
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::error_code remove_error;
    std::filesystem::remove(temporary, remove_error);
    error = "Cannot atomically install user settings: " + filesystem_error.message();
    return false;
  }
  error.clear();
  return true;
}

auto effectiveEditorTheme(const UserSettings& settings) -> std::string {
  const auto custom = settings.custom_themes.find(settings.theme);
  return custom == settings.custom_themes.end() ? settings.theme : custom->second;
}

}  // namespace tuiide
