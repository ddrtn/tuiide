#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace tuiide {

/** Сериализуемые свойства breakpoint, независимые от временного номера GDB. */
struct DebugSessionBreakpoint {
  std::filesystem::path file;
  std::size_t line{};
  bool enabled{true};
  std::string condition;
  unsigned hit_count{};
  std::string log_message;
};

/** Восстанавливаемое состояние Debug/Breakpoints и размеров панелей проекта. */
struct DebugSession {
  std::vector<DebugSessionBreakpoint> breakpoints;
  std::vector<std::string> watches;
  bool registers_enabled{};
  std::string cmake_target;
  std::string cmake_configuration;
  std::string cmake_configure_preset;
  std::string cmake_build_preset;
  std::size_t sidebar_width{};
  std::size_t lower_panel_height{};
};

auto loadDebugSession(const std::filesystem::path& path, DebugSession& session, std::string& error) -> bool;
/** Атомарно сохраняет сессию; несовместимые старые поля игнорируются при чтении. */
auto saveDebugSession(const std::filesystem::path& path, const DebugSession& session, std::string& error) -> bool;

}  // namespace tuiide
