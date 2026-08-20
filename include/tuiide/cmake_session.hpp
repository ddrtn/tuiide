#pragma once

#include "tuiide/cmake_model.hpp"
#include "tuiide/cmake_presets.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

struct CMakePresetRefreshResult {
  bool valid{true};
  std::string configure_error;
  std::string build_error;
  std::string validation_error;
};

class CMakeSession {
 public:
  void reset(std::filesystem::path default_build_directory = {});
  void restoreSelection(std::string target, std::string configuration,
    std::string configure_preset, std::string build_preset);

  [[nodiscard]] auto refreshPresets(const std::filesystem::path& project_directory,
    const std::filesystem::path& default_build_directory) -> CMakePresetRefreshResult;
  [[nodiscard]] auto selectConfigurePreset(std::string name,
    const std::filesystem::path& default_build_directory) -> bool;
  [[nodiscard]] auto selectBuildPreset(std::string name,
    const std::filesystem::path& default_build_directory) -> bool;

  void replaceTargets(std::vector<CMakeTarget> targets);
  [[nodiscard]] auto selectTarget(std::size_t index) -> bool;
  [[nodiscard]] auto selectedTarget() const noexcept -> const CMakeTarget*;
  [[nodiscard]] auto targetNamed(std::string_view name) const noexcept -> const CMakeTarget*;
  void rememberTarget(const CMakeTarget& target);

  [[nodiscard]] auto buildDirectory() noexcept -> std::filesystem::path& { return build_directory_; }
  [[nodiscard]] auto buildDirectory() const noexcept -> const std::filesystem::path& { return build_directory_; }
  [[nodiscard]] auto targets() const noexcept -> const std::vector<CMakeTarget>& { return targets_; }
  [[nodiscard]] auto configurePresets() const noexcept -> const std::vector<CMakeConfigurePreset>& {
    return configure_presets_;
  }
  [[nodiscard]] auto buildPresets() const noexcept -> const std::vector<CMakeBuildPreset>& {
    return build_presets_;
  }
  [[nodiscard]] auto configurePreset() const noexcept -> const std::string& { return configure_preset_; }
  [[nodiscard]] auto buildPreset() const noexcept -> const std::string& { return build_preset_; }
  [[nodiscard]] auto preferredTarget() const noexcept -> const std::string& { return preferred_target_; }
  [[nodiscard]] auto preferredConfiguration() const noexcept -> const std::string& {
    return preferred_configuration_;
  }

 private:
  void clearTargets();
  void updateBuildDirectory(const std::filesystem::path& default_build_directory);

  std::filesystem::path build_directory_;
  std::vector<CMakeTarget> targets_;
  std::vector<CMakeConfigurePreset> configure_presets_;
  std::vector<CMakeBuildPreset> build_presets_;
  std::optional<std::size_t> selected_target_;
  std::string configure_preset_;
  std::string build_preset_;
  std::string preferred_target_;
  std::string preferred_configuration_;
};

}  // namespace tuiide
