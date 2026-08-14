#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

struct CMakeTarget {
  std::string name;
  std::string configuration;
  std::filesystem::path artifact;
};

auto createCMakeFileApiQuery(const std::filesystem::path& build_directory, std::string& error) -> bool;
auto loadCMakeExecutableTargets(const std::filesystem::path& build_directory, std::string& error) -> std::vector<CMakeTarget>;

}  // namespace tuiide
