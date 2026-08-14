#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

struct CMakeConfigurePreset {
  std::string name;
  std::string display_name;
  std::string generator;
  std::filesystem::path binary_directory;
};

struct CMakeBuildPreset {
  std::string name;
  std::string display_name;
  std::string configure_preset;
  std::string configuration;
  std::vector<std::string> targets;
  bool clean_first{};
  bool verbose{};
};

auto loadCMakeConfigurePresets(const std::filesystem::path& source_directory, std::string& error)
  -> std::vector<CMakeConfigurePreset>;
auto loadCMakeBuildPresets(const std::filesystem::path& source_directory, std::string& error)
  -> std::vector<CMakeBuildPreset>;

}  // namespace tuiide
