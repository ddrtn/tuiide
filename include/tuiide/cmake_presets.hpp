#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

enum class CMakePresetKind { Configure, Build };

struct CMakeConfigurePreset {
  std::string name;
  std::string display_name;
  std::string generator;
  std::filesystem::path binary_directory;
  std::filesystem::path source_file;
  bool user_editable{};
};

struct CMakeBuildPreset {
  std::string name;
  std::string display_name;
  std::string configure_preset;
  std::string configuration;
  std::vector<std::string> targets;
  bool clean_first{};
  bool verbose{};
  std::filesystem::path source_file;
  bool user_editable{};
};

struct CMakePresetEdit {
  CMakePresetKind kind{CMakePresetKind::Configure};
  std::string name;
  std::string display_name;
  std::vector<std::string> inherits;
  std::string generator;
  std::string binary_directory;
  std::string configure_preset;
  std::string configuration;
  std::vector<std::string> targets;
  bool clean_first{};
  bool verbose{};
  bool hidden{};
  std::filesystem::path source_file;
  bool user_editable{};
};

auto loadCMakeConfigurePresets(const std::filesystem::path& source_directory, std::string& error)
  -> std::vector<CMakeConfigurePreset>;
auto loadCMakeBuildPresets(const std::filesystem::path& source_directory, std::string& error)
  -> std::vector<CMakeBuildPreset>;
auto loadCMakePresetForEdit(const std::filesystem::path& source_directory,
  CMakePresetKind kind, const std::string& name, CMakePresetEdit& preset,
  std::string& error) -> bool;
auto loadCMakePresetsForEdit(const std::filesystem::path& source_directory,
  CMakePresetKind kind, std::string& error) -> std::vector<CMakePresetEdit>;
auto saveCMakeUserPreset(const std::filesystem::path& source_directory,
  const std::string& original_name, const CMakePresetEdit& preset,
  std::string& error) -> bool;
auto cloneCMakePresetToUser(const std::filesystem::path& source_directory,
  CMakePresetKind kind, const std::string& source_name, const std::string& new_name,
  std::string& error) -> bool;
auto deleteCMakeUserPreset(const std::filesystem::path& source_directory,
  CMakePresetKind kind, const std::string& name, std::string& error) -> bool;

}  // namespace tuiide
