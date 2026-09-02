#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace tuiide {

struct UserSettings {
  int version{1};
  std::map<std::string, std::string> shortcuts;
  std::string theme{"Dark"};
  std::map<std::string, std::string> custom_themes;
  std::map<std::string, std::string> colors;
};

[[nodiscard]] auto defaultUserSettingsPath() -> std::filesystem::path;
[[nodiscard]] auto validateUserSettings(const UserSettings& settings, std::string& error) -> bool;
[[nodiscard]] auto loadUserSettings(const std::filesystem::path& path, UserSettings& settings,
  std::string& error) -> bool;
auto saveUserSettings(const std::filesystem::path& path, const UserSettings& settings,
  std::string& error) -> bool;
[[nodiscard]] auto effectiveEditorTheme(const UserSettings& settings) -> std::string;

}  // namespace tuiide
