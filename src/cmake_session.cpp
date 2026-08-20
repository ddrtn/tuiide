#include "tuiide/cmake_session.hpp"

#include <algorithm>
#include <utility>

namespace tuiide {

void CMakeSession::reset(std::filesystem::path default_build_directory) {
  build_directory_ = std::move(default_build_directory);
  targets_.clear();
  configure_presets_.clear();
  build_presets_.clear();
  selected_target_.reset();
  configure_preset_.clear();
  build_preset_.clear();
  preferred_target_.clear();
  preferred_configuration_.clear();
}

void CMakeSession::restoreSelection(std::string target, std::string configuration,
    std::string configure_preset, std::string build_preset) {
  preferred_target_ = std::move(target);
  preferred_configuration_ = std::move(configuration);
  configure_preset_ = std::move(configure_preset);
  build_preset_ = std::move(build_preset);
  selected_target_.reset();
}

auto CMakeSession::refreshPresets(const std::filesystem::path& project_directory,
    const std::filesystem::path& default_build_directory) -> CMakePresetRefreshResult {
  CMakePresetRefreshResult result;
  configure_presets_ = loadCMakeConfigurePresets(project_directory, result.configure_error);
  build_presets_.clear();
  build_directory_ = default_build_directory;
  if (!result.configure_error.empty()) {
    result.valid = configure_preset_.empty();
    return result;
  }
  build_presets_ = loadCMakeBuildPresets(project_directory, result.build_error);
  if (!result.build_error.empty() && !build_preset_.empty()) {
    result.valid = false;
    return result;
  }
  if (configure_preset_.empty()) {
    if (!build_preset_.empty()) {
      result.valid = false;
      result.validation_error = "A CMake build preset requires a configure preset";
    }
    return result;
  }
  const auto configure = std::ranges::find(configure_presets_, configure_preset_,
    &CMakeConfigurePreset::name);
  if (configure == configure_presets_.end()) {
    result.valid = false;
    result.validation_error = "CMake preset no longer exists: " + configure_preset_;
    return result;
  }
  if (!configure->binary_directory.empty()) build_directory_ = configure->binary_directory;
  if (build_preset_.empty()) return result;
  const auto build = std::ranges::find(build_presets_, build_preset_, &CMakeBuildPreset::name);
  if (build == build_presets_.end()) {
    result.valid = false;
    result.validation_error = "CMake build preset no longer exists: " + build_preset_;
  } else if (build->configure_preset != configure_preset_) {
    result.valid = false;
    result.validation_error = "Build preset " + build->name + " requires configure preset "
      + build->configure_preset;
  }
  return result;
}

auto CMakeSession::selectConfigurePreset(std::string name,
    const std::filesystem::path& default_build_directory) -> bool {
  if (!name.empty() && std::ranges::find(configure_presets_, name,
      &CMakeConfigurePreset::name) == configure_presets_.end()) return false;
  configure_preset_ = std::move(name);
  if (configure_preset_.empty()) build_preset_.clear();
  else if (!build_preset_.empty()) {
    const auto build = std::ranges::find(build_presets_, build_preset_, &CMakeBuildPreset::name);
    if (build == build_presets_.end() || build->configure_preset != configure_preset_)
      build_preset_.clear();
  }
  updateBuildDirectory(default_build_directory);
  clearTargets();
  return true;
}

auto CMakeSession::selectBuildPreset(std::string name,
    const std::filesystem::path& default_build_directory) -> bool {
  if (name.empty()) {
    build_preset_.clear();
    return true;
  }
  const auto build = std::ranges::find(build_presets_, name, &CMakeBuildPreset::name);
  if (build == build_presets_.end()) return false;
  const auto configure = std::ranges::find(configure_presets_, build->configure_preset,
    &CMakeConfigurePreset::name);
  if (configure == configure_presets_.end()) return false;
  build_preset_ = std::move(name);
  configure_preset_ = build->configure_preset;
  updateBuildDirectory(default_build_directory);
  clearTargets();
  return true;
}

void CMakeSession::replaceTargets(std::vector<CMakeTarget> targets) {
  std::string previous_name = preferred_target_;
  std::string previous_configuration = preferred_configuration_;
  if (const auto* selected = selectedTarget()) {
    previous_name = selected->name;
    previous_configuration = selected->configuration;
  }
  targets_ = std::move(targets);
  selected_target_ = targets_.empty() ? std::nullopt : std::optional<std::size_t>{0};
  for (std::size_t index = 0; index < targets_.size(); ++index) {
    if (targets_[index].name == previous_name
        && targets_[index].configuration == previous_configuration) {
      selected_target_ = index;
      break;
    }
  }
}

auto CMakeSession::selectTarget(std::size_t index) -> bool {
  if (index >= targets_.size()) return false;
  selected_target_ = index;
  rememberTarget(targets_[index]);
  return true;
}

auto CMakeSession::selectedTarget() const noexcept -> const CMakeTarget* {
  return selected_target_ && *selected_target_ < targets_.size()
    ? &targets_[*selected_target_] : nullptr;
}

auto CMakeSession::targetNamed(std::string_view name) const noexcept -> const CMakeTarget* {
  const auto target = std::ranges::find(targets_, name, &CMakeTarget::name);
  return target == targets_.end() ? nullptr : &*target;
}

void CMakeSession::rememberTarget(const CMakeTarget& target) {
  preferred_target_ = target.name;
  preferred_configuration_ = target.configuration;
}

void CMakeSession::clearTargets() {
  targets_.clear();
  selected_target_.reset();
  preferred_target_.clear();
  preferred_configuration_.clear();
}

void CMakeSession::updateBuildDirectory(const std::filesystem::path& default_build_directory) {
  build_directory_ = default_build_directory;
  if (configure_preset_.empty()) return;
  const auto preset = std::ranges::find(configure_presets_, configure_preset_,
    &CMakeConfigurePreset::name);
  if (preset != configure_presets_.end() && !preset->binary_directory.empty())
    build_directory_ = preset->binary_directory;
}

}  // namespace tuiide
