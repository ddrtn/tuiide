#pragma once

#include "tuiide/process.hpp"
#include "tuiide/pseudo_terminal.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tuiide {

/** Способ запуска: захваченный stdout/stderr либо интерактивный псевдотерминал. */
enum class RunTransport { Process, Terminal };

struct RunPollResult {
  std::vector<std::string> output;
  std::vector<std::string> console_output;
  std::optional<int> completion;
};

/**
 * Владеет взаимоисключающими жизненными циклами Run-процесса и PTY, а также
 * отдельным PTY для консоли inferior в GDB. Это не позволяет смешать вывод
 * обычного запуска с отладочной консолью.
 */
class RunSession {
 public:
  [[nodiscard]] auto start(std::vector<std::string> arguments,
    RunTransport transport, const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment,
    unsigned columns = 80, unsigned rows = 24) -> bool;
  [[nodiscard]] auto openDebugConsole(unsigned columns = 80,
    unsigned rows = 24) -> bool;
  void activateDebugConsole();
  [[nodiscard]] auto debugTerminal() const -> std::filesystem::path;

  /** Возвращает накопленный вывод и однократно сообщает код завершения. */
  [[nodiscard]] auto poll() -> RunPollResult;
  [[nodiscard]] auto writeConsole(std::string_view data) -> bool;
  [[nodiscard]] auto resizeConsole(unsigned columns, unsigned rows) -> bool;
  [[nodiscard]] auto signalConsole(int signal) -> bool;
  [[nodiscard]] auto running() const noexcept -> bool { return run_active_; }
  [[nodiscard]] auto consoleRunning() const -> bool;
  void stop();

 private:
  enum class Mode { Idle, Process, Terminal, DebugTerminal };

  AsyncProcess process_;
  PseudoTerminal terminal_;
  Mode mode_{Mode::Idle};
  bool run_active_{};
};

}  // namespace tuiide
