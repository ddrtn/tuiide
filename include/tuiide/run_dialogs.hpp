#pragma once

#include "tuiide/cmake_model.hpp"
#include "tuiide/cmake_presets.hpp"
#include "tuiide/launch_configuration.hpp"
#include "tuiide/ui_dialogs.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tuiide {

class LaunchSettingsDialog final : public CenteredDialog {
 public:
  LaunchSettingsDialog(std::filesystem::path root,
    const LaunchConfiguration& configuration, const std::vector<CMakeTarget>& targets,
    finalcut::FWidget* parent = nullptr);
  ~LaunchSettingsDialog() override;

  [[nodiscard]] auto configuration(LaunchConfiguration& result,
    std::string& error) const -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class CMakePresetEditDialog final : public CenteredDialog {
 public:
  explicit CMakePresetEditDialog(CMakePresetEdit preset,
    finalcut::FWidget* parent = nullptr);
  ~CMakePresetEditDialog() override;
  [[nodiscard]] auto preset() const -> CMakePresetEdit;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class CMakePresetManagerDialog final : public CenteredDialog {
 public:
  CMakePresetManagerDialog(std::filesystem::path root, CMakePresetKind kind,
    std::string selected, finalcut::FWidget* parent = nullptr);
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
