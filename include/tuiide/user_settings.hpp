#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace tuiide {

/**
 * Независимые от проекта пользовательские предпочтения IDE.
 * Сохраняются атомарно в XDG-конфигурации, поэтому доступны до открытия проекта.
 */
struct UserSettings {
  int version{1};
  std::map<std::string, std::string> shortcuts;
  std::string theme{"Dark"};
  std::map<std::string, std::string> custom_themes;
  std::map<std::string, std::string> colors;
};

/** Возвращает XDG-путь `$XDG_CONFIG_HOME/tuiide/settings.json`. */
[[nodiscard]] auto defaultUserSettingsPath() -> std::filesystem::path;
[[nodiscard]] auto validateUserSettings(const UserSettings& settings, std::string& error) -> bool;
[[nodiscard]] auto loadUserSettings(const std::filesystem::path& path, UserSettings& settings,
  std::string& error) -> bool;
/** Проверяет и атомарно сохраняет настройки, не повреждая прежний файл при ошибке. */
auto saveUserSettings(const std::filesystem::path& path, const UserSettings& settings,
  std::string& error) -> bool;
[[nodiscard]] auto effectiveEditorTheme(const UserSettings& settings) -> std::string;

}  // namespace tuiide
