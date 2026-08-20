#include "tuiide/debug_ui_controller.hpp"

#include <utility>

namespace tuiide {

auto DebugUiController::capture(const GdbClient& client) -> DebugSnapshot {
  return {
    .running = client.running(),
    .stopped = client.stopped(),
    .exited = client.exited(),
    .registers_enabled = client.registersEnabled(),
    .selected_frame = client.selectedFrame(),
    .threads = client.threads(),
    .frames = client.frames(),
    .variables = client.variables(),
    .watches = client.watches(),
    .registers = client.registers()
  };
}

auto DebugUiController::updateDebug(DebugSnapshot snapshot) -> bool {
  std::string signature = snapshot.exited ? "exited"
    : snapshot.stopped ? "stopped" : snapshot.running ? "running" : "off";
  for (const auto& thread : snapshot.threads)
    signature += "|" + thread.id + thread.name + thread.state + (thread.current ? "*" : "");
  for (const auto& frame : snapshot.frames)
    signature += "|" + std::to_string(frame.level) + frame.function
      + frame.file.string() + std::to_string(frame.line);
  signature += "|frame:" + std::to_string(snapshot.selected_frame);
  for (const auto& variable : snapshot.variables)
    signature += "|" + std::to_string(variable.depth) + variable.name + variable.value
      + variable.type + variable.object + (variable.expandable ? "+" : "-")
      + (variable.expanded ? "open" : "closed");
  for (const auto& watch : snapshot.watches)
    signature += "|watch:" + watch.expression + watch.value + watch.error;
  signature += snapshot.registers_enabled ? "|registers:on" : "|registers:off";
  for (const auto& reg : snapshot.registers) signature += "|reg:" + reg.name + reg.value;
  if (signature == debug_signature_) return false;
  debug_signature_ = std::move(signature);
  debug_rows_.clear();
  const auto addRow = [this](std::string label) -> DebugPanelRow& {
    debug_rows_.emplace_back();
    debug_rows_.back().label = std::move(label);
    return debug_rows_.back();
  };
  if (snapshot.threads.empty() && snapshot.frames.empty() && snapshot.variables.empty()) {
    addRow(snapshot.exited ? "Program exited"
      : snapshot.running ? (snapshot.stopped ? "Loading state..." : "Program running")
      : "Debugger not started");
  }
  for (std::size_t index = 0; index < snapshot.watches.size(); ++index) {
    const auto& watch = snapshot.watches[index];
    addRow("Watch " + watch.expression + " = "
      + (watch.error.empty() ? watch.value : "<" + watch.error + ">")).watch_index = index;
  }
  if (snapshot.registers_enabled && snapshot.registers.empty()) {
    addRow(snapshot.stopped ? "Registers: loading..." : "Registers: debugger not stopped");
  }
  for (const auto& reg : snapshot.registers)
    addRow("Reg " + reg.name + " = " + reg.value);
  for (const auto& thread : snapshot.threads) {
    auto label = std::string(thread.current ? "> Thread " : "  Thread ") + thread.id;
    if (!thread.name.empty()) label += " " + thread.name;
    if (!thread.state.empty()) label += " [" + thread.state + "]";
    addRow(std::move(label)).thread_id = thread.id;
  }
  for (const auto& frame : snapshot.frames) {
    const auto file = frame.file.empty() ? std::string{}
      : frame.file.filename().string() + ":" + std::to_string(frame.line);
    auto& row = addRow(std::string(frame.level == snapshot.selected_frame ? "> #" : "  #")
      + std::to_string(frame.level) + " " + frame.function + " " + file);
    row.file = frame.file;
    row.line = frame.line;
    row.frame_level = frame.level;
  }
  for (std::size_t index = 0; index < snapshot.variables.size(); ++index) {
    const auto& variable = snapshot.variables[index];
    auto label = std::string(variable.depth * 2, ' ');
    label += variable.expandable ? (variable.expanded ? "[-] " : "[+] ") : "    ";
    label += variable.name + " = " + variable.value;
    if (!variable.type.empty()) label += " : " + variable.type;
    addRow(std::move(label)).variable_index = index;
  }
  return true;
}

auto DebugUiController::updateBreakpoints(std::vector<DebugBreakpoint> breakpoints,
    bool debugger_running, const std::filesystem::path& project_root) -> bool {
  std::string signature = debugger_running ? "breakpoints:running" : "breakpoints:off";
  for (const auto& item : breakpoints)
    signature += item.file.string() + ':' + std::to_string(item.line)
      + (item.enabled ? ":on:" : ":off:") + item.condition + ':'
      + std::to_string(item.hit_count) + ':' + item.log_message
      + (item.verified ? ":verified:" : ":unverified:") + item.error;
  if (signature == breakpoint_signature_) return false;
  breakpoint_signature_ = std::move(signature);
  breakpoint_rows_.clear();
  breakpoint_rows_.reserve(breakpoints.size());
  for (auto& item : breakpoints) {
    std::error_code error;
    auto path = project_root.empty() ? item.file : std::filesystem::relative(item.file, project_root, error);
    if (error) path = item.file;
    auto label = std::string(item.enabled ? "[x] " : "[ ] ")
      + path.generic_string() + ':' + std::to_string(item.line);
    if (!item.condition.empty()) label += " if " + item.condition;
    if (item.hit_count != 0) label += " after " + std::to_string(item.hit_count) + " hit(s)";
    if (!item.log_message.empty()) label += " log: " + item.log_message;
    label += item.verified ? " [verified]" : debugger_running ? " [unresolved]" : " [not started]";
    breakpoint_rows_.push_back({std::move(label), std::move(item)});
  }
  return true;
}

auto DebugUiController::observeActive(bool active, bool exited) noexcept -> bool {
  const bool finished = active_observed_ && !active && exited;
  active_observed_ = active;
  return finished;
}

void DebugUiController::reset() noexcept {
  debug_signature_.clear();
  breakpoint_signature_.clear();
  debug_rows_.clear();
  breakpoint_rows_.clear();
  active_observed_ = false;
}

auto DebugUiController::debugRow(std::size_t index) const noexcept -> const DebugPanelRow* {
  return index < debug_rows_.size() ? &debug_rows_[index] : nullptr;
}

auto DebugUiController::breakpointRow(std::size_t index) const noexcept -> const BreakpointPanelRow* {
  return index < breakpoint_rows_.size() ? &breakpoint_rows_[index] : nullptr;
}

}  // namespace tuiide
