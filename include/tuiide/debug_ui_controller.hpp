#pragma once

#include "tuiide/gdb_client.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tuiide {

struct DebugSnapshot {
  bool running{};
  bool stopped{};
  bool exited{};
  bool registers_enabled{};
  int selected_frame{};
  std::vector<DebugThread> threads;
  std::vector<DebugFrame> frames;
  std::vector<DebugVariable> variables;
  std::vector<DebugWatch> watches;
  std::vector<DebugRegister> registers;
};

struct DebugPanelRow {
  std::string label;
  std::filesystem::path file;
  std::size_t line{};  // one-based, zero means no source location
  std::optional<std::string> thread_id;
  std::optional<std::size_t> watch_index;
  std::optional<int> frame_level;
  std::optional<std::size_t> variable_index;
};

struct BreakpointPanelRow {
  std::string label;
  DebugBreakpoint breakpoint;
};

class DebugUiController {
 public:
  [[nodiscard]] static auto capture(const GdbClient& client) -> DebugSnapshot;
  [[nodiscard]] auto updateDebug(DebugSnapshot snapshot) -> bool;
  [[nodiscard]] auto updateBreakpoints(std::vector<DebugBreakpoint> breakpoints,
    bool debugger_running, const std::filesystem::path& project_root) -> bool;
  [[nodiscard]] auto observeActive(bool active, bool exited) noexcept -> bool;
  void invalidateDebug() noexcept { debug_signature_.clear(); }
  void invalidateBreakpoints() noexcept { breakpoint_signature_.clear(); }
  void reset() noexcept;

  [[nodiscard]] auto debugRows() const noexcept -> const std::vector<DebugPanelRow>& {
    return debug_rows_;
  }
  [[nodiscard]] auto breakpointRows() const noexcept -> const std::vector<BreakpointPanelRow>& {
    return breakpoint_rows_;
  }
  [[nodiscard]] auto debugRow(std::size_t index) const noexcept -> const DebugPanelRow*;
  [[nodiscard]] auto breakpointRow(std::size_t index) const noexcept -> const BreakpointPanelRow*;

 private:
  std::string debug_signature_;
  std::string breakpoint_signature_;
  std::vector<DebugPanelRow> debug_rows_;
  std::vector<BreakpointPanelRow> breakpoint_rows_;
  bool active_observed_{};
};

}  // namespace tuiide
