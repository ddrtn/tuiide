#pragma once

#include "tuiide/cmake_model.hpp"
#include "tuiide/cmake_presets.hpp"
#include "tuiide/launch_configuration.hpp"
#include "tuiide/project_settings.hpp"
#include "tuiide/ui_dialogs.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tuiide {

/** Собирает и валидирует параметры Run/Debug для выбранного CMake target. */
class LaunchSettingsDialog final : public CenteredDialog {
 public:
  LaunchSettingsDialog(std::filesystem::path root,
    const LaunchConfiguration& configuration, const std::vector<CMakeTarget>& targets,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  ~LaunchSettingsDialog() override;

  [[nodiscard]] auto configuration(LaunchConfiguration& result,
    std::string& error) const -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** Управляет именованными профилями Run/Debug и выбирает активный профиль. */
class LaunchConfigurationManagerDialog final : public CenteredDialog {
 public:
  LaunchConfigurationManagerDialog(std::filesystem::path root,
    std::vector<NamedLaunchConfiguration> configurations, std::string selected,
    const std::vector<CMakeTarget>& targets, finalcut::FWidget* parent = nullptr,
    std::string language = "en");

  [[nodiscard]] auto configurations() const -> const std::vector<NamedLaunchConfiguration>& {
    return configurations_;
  }
  [[nodiscard]] auto selectedName() const -> const std::string& { return selected_; }

 private:
  void refresh(const std::string& preferred = {});
  void addConfiguration();
  void cloneConfiguration();
  void editConfiguration();
  void deleteConfiguration();
  void selectConfiguration();
  [[nodiscard]] auto currentIndex() const -> std::optional<std::size_t>;
  [[nodiscard]] auto requestUniqueName(std::string title) -> std::optional<std::string>;

  std::filesystem::path root_;
  std::string language_;
  std::vector<NamedLaunchConfiguration> configurations_;
  std::string selected_;
  std::vector<CMakeTarget> targets_;
  EnterListBox list_;
  finalcut::FButton add_;
  finalcut::FButton clone_;
  finalcut::FButton edit_;
  finalcut::FButton remove_;
  finalcut::FButton select_;
  finalcut::FButton cancel_;
};

/** Редактирует один user preset; project presets открываются только для чтения. */
class CMakePresetEditDialog final : public CenteredDialog {
 public:
  explicit CMakePresetEditDialog(CMakePresetEdit preset,
    finalcut::FWidget* parent = nullptr, std::string language = "en");
  ~CMakePresetEditDialog() override;
  [[nodiscard]] auto preset() const -> CMakePresetEdit;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/** Управляет созданием, клонированием и удалением configure/build user presets. */
class CMakePresetManagerDialog final : public CenteredDialog {
 public:
  CMakePresetManagerDialog(std::filesystem::path root, CMakePresetKind kind,
    std::string selected, finalcut::FWidget* parent = nullptr,
    std::string language = "en");
  [[nodiscard]] auto selectedPreset() const -> std::string;
  [[nodiscard]] auto changed() const noexcept -> bool { return changed_; }

 private:
  void reload(const std::string& preferred = {});
  void addPreset();
  void clonePreset();
  void editPreset();
  void deletePreset();
  void selectPreset();
  [[nodiscard]] auto currentPreset() -> CMakePresetEdit*;

  std::filesystem::path root_;
  CMakePresetKind kind_;
  std::string language_;
  std::string initial_selection_;
  std::string selected_;
  std::vector<CMakePresetEdit> presets_;
  bool changed_{};
  EnterListBox list_;
  finalcut::FButton add_;
  finalcut::FButton clone_;
  finalcut::FButton edit_;
  finalcut::FButton remove_;
  finalcut::FButton reload_;
  finalcut::FButton select_;
  finalcut::FButton cancel_;
};

}  // namespace tuiide
