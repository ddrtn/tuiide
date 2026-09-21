#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tuiide {

/**
 * Независимые от проекта пользовательские предпочтения IDE.
 * Сохраняются атомарно в XDG-конфигурации, поэтому доступны до открытия проекта.
 */
struct UserSettings {
  int version{1};
  std::string language{"en"};
  std::map<std::string, std::string> shortcuts;
  std::string theme{"Dark"};
  std::map<std::string, std::string> custom_themes;
  std::map<std::string, std::string> colors;
  std::vector<std::filesystem::path> recent_files;
};

/** Причина запрета клавиши, перехватываемой Final Cut или служебным меню IDE. */
[[nodiscard]] auto reservedShortcutReason(std::string shortcut) -> std::string;

/** Нормализует, дедуплицирует и ограничивает историю существующих файлов. */
[[nodiscard]] auto normalizeRecentFiles(const std::vector<std::filesystem::path>& files,
  std::size_t limit = 10) -> std::vector<std::filesystem::path>;
/** Перемещает существующий файл в начало ограниченной истории. */
void rememberRecentFile(std::vector<std::filesystem::path>& files,
  const std::filesystem::path& file, std::size_t limit = 10);

/** Возвращает XDG-путь `$XDG_CONFIG_HOME/tuiide/settings.json`. */
[[nodiscard]] auto defaultUserSettingsPath() -> std::filesystem::path;
[[nodiscard]] auto validateUserSettings(const UserSettings& settings, std::string& error) -> bool;
/** Загружает предпочтения; старые запрещённые shortcuts удаляет с предупреждениями. */
[[nodiscard]] auto loadUserSettings(const std::filesystem::path& path, UserSettings& settings,
  std::string& error, std::vector<std::string>* warnings = nullptr) -> bool;
/** Проверяет и атомарно сохраняет настройки, не повреждая прежний файл при ошибке. */
auto saveUserSettings(const std::filesystem::path& path, const UserSettings& settings,
  std::string& error) -> bool;
[[nodiscard]] auto effectiveEditorTheme(const UserSettings& settings) -> std::string;

}  // namespace tuiide
