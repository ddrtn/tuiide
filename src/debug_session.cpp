#include "tuiide/debug_session.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace tuiide {

auto loadDebugSession(const std::filesystem::path& path, DebugSession& session, std::string& error) -> bool {
  error.clear();
  session = {};
  if (!std::filesystem::exists(path)) return true;
  std::ifstream input(path);
  if (!input) { error = "cannot open " + path.string(); return false; }
  const auto json = nlohmann::json::parse(input, nullptr, false);
  if (json.is_discarded() || !json.is_object() || json.value("version", 0) != 1) {
    error = "invalid or unsupported debug session: " + path.string();
    return false;
  }
  try {
    for (const auto& item : json.at("breakpoints")) {
      const auto file = item.at("file").get<std::string>();
      const auto line = item.at("line").get<std::size_t>();
      if (file.empty() || line == 0) throw std::runtime_error("invalid breakpoint");
      session.breakpoints.push_back({file, line, item.value("enabled", true),
        item.value("condition", std::string{}), item.value("hit_count", 0U),
        item.value("log_message", std::string{})});
    }
    for (const auto& item : json.at("watches")) {
      const auto expression = item.get<std::string>();
      if (expression.empty()) throw std::runtime_error("empty watch");
      session.watches.push_back(expression);
    }
    session.registers_enabled = json.value("registers_enabled", false);
    session.cmake_target = json.value("cmake_target", std::string{});
    session.cmake_configuration = json.value("cmake_configuration", std::string{});
    session.cmake_configure_preset = json.value("cmake_configure_preset", std::string{});
    session.cmake_build_preset = json.value("cmake_build_preset", std::string{});
    session.sidebar_width = json.value("sidebar_width", std::size_t{});
    session.lower_panel_height = json.value("lower_panel_height", std::size_t{});
  } catch (const std::exception& exception) {
    session = {};
    error = "invalid debug session: " + std::string(exception.what());
    return false;
  }
  return true;
}

auto saveDebugSession(const std::filesystem::path& path, const DebugSession& session, std::string& error) -> bool {
  error.clear();
  std::error_code filesystem_error;
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) { error = "cannot create session directory: " + filesystem_error.message(); return false; }
  nlohmann::json json{{"version", 1}, {"breakpoints", nlohmann::json::array()},
    {"watches", session.watches}, {"registers_enabled", session.registers_enabled},
    {"cmake_target", session.cmake_target}, {"cmake_configuration", session.cmake_configuration},
    {"cmake_configure_preset", session.cmake_configure_preset},
    {"cmake_build_preset", session.cmake_build_preset},
    {"sidebar_width", session.sidebar_width}, {"lower_panel_height", session.lower_panel_height}};
  for (const auto& breakpoint : session.breakpoints) {
    json["breakpoints"].push_back({{"file", breakpoint.file.string()}, {"line", breakpoint.line},
      {"enabled", breakpoint.enabled}, {"condition", breakpoint.condition},
      {"hit_count", breakpoint.hit_count}, {"log_message", breakpoint.log_message}});
  }
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) { error = "cannot write " + temporary.string(); return false; }
    output << json.dump(2) << '\n';
    if (!output) { error = "failed writing " + temporary.string(); return false; }
  }
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary);
    error = "cannot replace session file: " + filesystem_error.message();
    return false;
  }
  return true;
}

}  // namespace tuiide
