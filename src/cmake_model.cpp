#include "tuiide/cmake_model.hpp"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace tuiide {
namespace {
auto readJson(const std::filesystem::path& path) -> nlohmann::json {
  std::ifstream input(path);
  if (!input) return {};
  return nlohmann::json::parse(input, nullptr, false);
}
}

auto createCMakeFileApiQuery(const std::filesystem::path& build_directory, std::string& error) -> bool {
  error.clear();
  const auto query_directory = build_directory / ".cmake/api/v1/query";
  std::error_code filesystem_error;
  std::filesystem::create_directories(query_directory, filesystem_error);
  if (filesystem_error) { error = "cannot create CMake File API query: " + filesystem_error.message(); return false; }
  std::ofstream query(query_directory / "codemodel-v2", std::ios::trunc);
  if (!query) { error = "cannot write CMake File API query"; return false; }
  return true;
}

auto loadCMakeExecutableTargets(const std::filesystem::path& build_directory, std::string& error) -> std::vector<CMakeTarget> {
  error.clear();
  const auto reply_directory = build_directory / ".cmake/api/v1/reply";
  std::filesystem::path newest_index;
  std::filesystem::file_time_type newest_time{};
  std::error_code filesystem_error;
  for (std::filesystem::directory_iterator iterator(reply_directory, filesystem_error), end; !filesystem_error && iterator != end; iterator.increment(filesystem_error)) {
    if (!iterator->is_regular_file() || !iterator->path().filename().string().starts_with("index-")) continue;
    const auto time = iterator->last_write_time(filesystem_error);
    if (!filesystem_error && (newest_index.empty() || time > newest_time)) { newest_index = iterator->path(); newest_time = time; }
  }
  if (newest_index.empty()) { error = "CMake File API reply is unavailable; configure the project first"; return {}; }
  const auto index = readJson(newest_index);
  if (!index.is_object()) { error = "invalid CMake File API index"; return {}; }
  try {
    const auto codemodel_file = index.at("reply").at("codemodel-v2").at("jsonFile").get<std::string>();
    const auto codemodel = readJson(reply_directory / codemodel_file);
    if (!codemodel.is_object()) throw std::runtime_error("invalid codemodel");
    std::vector<CMakeTarget> result;
    for (const auto& configuration : codemodel.at("configurations")) {
      const auto configuration_name = configuration.value("name", std::string{});
      for (const auto& reference : configuration.at("targets")) {
        const auto target = readJson(reply_directory / reference.at("jsonFile").get<std::string>());
        if (!target.is_object() || target.value("type", std::string{}) != "EXECUTABLE" || !target.contains("artifacts") || target["artifacts"].empty()) continue;
        auto artifact = std::filesystem::path(target["artifacts"][0].at("path").get<std::string>());
        if (artifact.is_relative()) artifact = build_directory / artifact;
        result.push_back({target.value("name", reference.value("name", std::string{})), configuration_name,
          std::filesystem::absolute(artifact).lexically_normal()});
      }
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
      return left.configuration == right.configuration ? left.name < right.name : left.configuration < right.configuration;
    });
    return result;
  } catch (const std::exception& exception) {
    error = "invalid CMake codemodel: " + std::string(exception.what());
    return {};
  }
}

}  // namespace tuiide
