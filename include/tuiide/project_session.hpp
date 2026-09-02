#pragma once

#include "tuiide/project_settings.hpp"

#include <filesystem>
#include <string>

namespace tuiide {

/** Дополнительное сообщение об открытии, не являющееся фатальной ошибкой. */
struct ProjectOpenResult {
  bool used_default_settings{};
  std::string warning;
};

/**
 * Владеет корнем проекта, настройками и производными путями сессии/recovery.
 * Закрытие очищает состояние целиком, чтобы следующий проект не наследовал его.
 */
class ProjectSession {
 public:
  [[nodiscard]] auto open(std::filesystem::path project_directory,
    std::filesystem::path build_directory, ProjectOpenResult& result,
    std::string& error) -> bool;
  void close();
  /** Принимает уже проверенные настройки и пересчитывает производные пути. */
  void applySettings(ProjectSettings settings);

  [[nodiscard]] auto open() const noexcept -> bool { return !root_.empty(); }
  [[nodiscard]] auto root() noexcept -> std::filesystem::path& { return root_; }
  [[nodiscard]] auto root() const noexcept -> const std::filesystem::path& { return root_; }
  [[nodiscard]] auto buildDirectory() noexcept -> std::filesystem::path& { return build_directory_; }
  [[nodiscard]] auto buildDirectory() const noexcept -> const std::filesystem::path& { return build_directory_; }
  [[nodiscard]] auto sessionFile() noexcept -> std::filesystem::path& { return session_file_; }
  [[nodiscard]] auto sessionFile() const noexcept -> const std::filesystem::path& { return session_file_; }
  [[nodiscard]] auto recoveryFile() noexcept -> std::filesystem::path& { return recovery_file_; }
  [[nodiscard]] auto recoveryFile() const noexcept -> const std::filesystem::path& { return recovery_file_; }
  [[nodiscard]] auto settings() noexcept -> ProjectSettings& { return settings_; }
  [[nodiscard]] auto settings() const noexcept -> const ProjectSettings& { return settings_; }

 private:
  void refreshDerivedPaths();

  std::filesystem::path root_;
  std::filesystem::path build_directory_;
  std::filesystem::path session_file_;
  std::filesystem::path recovery_file_;
  ProjectSettings settings_;
};

}  // namespace tuiide
