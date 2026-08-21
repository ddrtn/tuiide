#include "tuiide/run_session.hpp"

#include <iterator>
#include <utility>

namespace tuiide {

auto RunSession::start(std::vector<std::string> arguments,
    RunTransport transport, const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment,
    unsigned columns, unsigned rows) -> bool {
  stop();
  mode_ = transport == RunTransport::Process ? Mode::Process : Mode::Terminal;
  run_active_ = mode_ == Mode::Process
    ? process_.start(arguments, true, working_directory, environment)
    : terminal_.start(arguments, working_directory, environment, columns, rows);
  if (!run_active_) mode_ = Mode::Idle;
  return run_active_;
}

auto RunSession::openDebugConsole(unsigned columns, unsigned rows) -> bool {
  stop();
  if (!terminal_.openSession(columns, rows)) return false;
  mode_ = Mode::DebugTerminal;
  return true;
}

void RunSession::activateDebugConsole() {
  if (mode_ == Mode::DebugTerminal) terminal_.activateSession();
}

auto RunSession::debugTerminal() const -> std::filesystem::path {
  return mode_ == Mode::DebugTerminal ? terminal_.slaveName() : std::filesystem::path{};
}

auto RunSession::poll() -> RunPollResult {
  RunPollResult result;
  result.output = process_.drain();
  result.console_output = terminal_.drain();
  if (!run_active_) return result;

  const auto exit_code = mode_ == Mode::Process
    ? process_.exitCode() : terminal_.exitCode();
  if (!exit_code) return result;
  result.completion = exit_code;
  process_.stop();
  terminal_.stop();
  auto trailing_output = process_.drain();
  result.output.insert(result.output.end(),
    std::make_move_iterator(trailing_output.begin()),
    std::make_move_iterator(trailing_output.end()));
  auto trailing_console_output = terminal_.drain();
  result.console_output.insert(result.console_output.end(),
    std::make_move_iterator(trailing_console_output.begin()),
    std::make_move_iterator(trailing_console_output.end()));
  mode_ = Mode::Idle;
  run_active_ = false;
  return result;
}

auto RunSession::writeConsole(std::string_view data) -> bool {
  return terminal_.write(data);
}

auto RunSession::resizeConsole(unsigned columns, unsigned rows) -> bool {
  return terminal_.resize(columns, rows);
}

auto RunSession::signalConsole(int signal) -> bool {
  return terminal_.sendSignal(signal);
}

auto RunSession::consoleRunning() const -> bool { return terminal_.running(); }

void RunSession::stop() {
  process_.stop();
  terminal_.stop();
  (void)process_.drain();
  (void)terminal_.drain();
  mode_ = Mode::Idle;
  run_active_ = false;
}

}  // namespace tuiide
