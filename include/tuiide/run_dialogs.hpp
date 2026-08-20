#pragma once

#include "tuiide/cmake_model.hpp"
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

}  // namespace tuiide
