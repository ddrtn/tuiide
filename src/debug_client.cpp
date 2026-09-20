#include "tuiide/debug_client.hpp"

#include <utility>

namespace tuiide {

auto debugBackendName(DebugBackend backend) -> std::string {
  return backend == DebugBackend::LldbDap ? "lldb-dap" : "gdb-mi";
}

auto parseDebugBackend(std::string_view value) -> DebugBackend {
  return value == "lldb-dap" ? DebugBackend::LldbDap : DebugBackend::GdbMi;
}

void DebugClient::configure(DebugBackend backend, std::filesystem::path adapter) {
  if (running() || (backend_ == backend && adapter_ == adapter)) return;
  migrateModel(backend);
  backend_ = backend;
  adapter_ = std::move(adapter);
}

auto DebugClient::backend() const -> DebugBackend { return backend_; }
auto DebugClient::backendName() const -> std::string { return debugBackendName(backend_); }

auto DebugClient::start(const std::filesystem::path& executable,
    const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& stdin_file,
    const std::filesystem::path& inferior_tty) -> bool {
  if (useLldb())
    return lldb_.start(adapter_.empty() ? std::filesystem::path("lldb-dap") : adapter_,
      executable, working_directory, environment, arguments, inferior_tty);
  return gdb_.start(executable, working_directory, environment, arguments, stdin_file, inferior_tty);
}

auto DebugClient::attach(int pid, const std::filesystem::path& working_directory) -> bool {
  if (useLldb()) return false;
  return gdb_.attach(pid, working_directory);
}

auto DebugClient::openCore(const std::filesystem::path& executable,
    const std::filesystem::path& core_file,
    const std::filesystem::path& working_directory) -> bool {
  if (useLldb()) return false;
  return gdb_.openCore(executable, core_file, working_directory);
}

auto DebugClient::stop() -> bool { return useLldb() ? lldb_.stop() : gdb_.stop(); }

void DebugClient::clearSessionState() {
  gdb_.clearSessionState(); lldb_.clearSessionState();
}

void DebugClient::run() { useLldb() ? lldb_.run() : gdb_.run(); }
void DebugClient::interrupt() { useLldb() ? lldb_.interrupt() : gdb_.interrupt(); }
void DebugClient::continueExecution() { useLldb() ? lldb_.continueExecution() : gdb_.continueExecution(); }
void DebugClient::next() { useLldb() ? lldb_.next() : gdb_.next(); }
void DebugClient::step() { useLldb() ? lldb_.step() : gdb_.step(); }
void DebugClient::finish() { useLldb() ? lldb_.finish() : gdb_.finish(); }
auto DebugClient::sendSignal(std::string_view signal) -> bool {
  return !useLldb() && gdb_.sendSignal(signal);
}
auto DebugClient::setSignalPolicy(std::string_view signal, bool stop, bool print, bool pass) -> bool {
  return !useLldb() && gdb_.setSignalPolicy(signal, stop, print, pass);
}
auto DebugClient::inspectSignals() -> bool { return !useLldb() && gdb_.inspectSignals(); }
void DebugClient::selectThread(const std::string& id) {
  useLldb() ? lldb_.selectThread(id) : gdb_.selectThread(id);
}
auto DebugClient::selectFrame(int level) -> bool {
  return useLldb() ? lldb_.selectFrame(level) : gdb_.selectFrame(level);
}
auto DebugClient::toggleVariable(std::size_t index) -> bool {
  return useLldb() ? lldb_.toggleVariable(index) : gdb_.toggleVariable(index);
}
auto DebugClient::evaluate(std::string expression) -> bool {
  return useLldb() ? lldb_.evaluate(std::move(expression)) : gdb_.evaluate(std::move(expression));
}
auto DebugClient::assign(std::string expression, std::string value) -> bool {
  return useLldb() ? lldb_.assign(std::move(expression), std::move(value))
    : gdb_.assign(std::move(expression), std::move(value));
}
auto DebugClient::disassemble(std::string address, std::size_t bytes) -> bool {
  return useLldb() ? lldb_.disassemble(std::move(address), bytes)
    : gdb_.disassemble(std::move(address), bytes);
}
auto DebugClient::readMemory(std::string address, std::size_t bytes) -> bool {
  return useLldb() ? lldb_.readMemory(std::move(address), bytes)
    : gdb_.readMemory(std::move(address), bytes);
}
auto DebugClient::addWatch(std::string expression) -> bool {
  return useLldb() ? lldb_.addWatch(std::move(expression)) : gdb_.addWatch(std::move(expression));
}
auto DebugClient::removeWatch(std::size_t index) -> bool {
  return useLldb() ? lldb_.removeWatch(index) : gdb_.removeWatch(index);
}
void DebugClient::setRegistersEnabled(bool enabled) {
  useLldb() ? lldb_.setRegistersEnabled(enabled) : gdb_.setRegistersEnabled(enabled);
}
auto DebugClient::toggleBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool {
  return useLldb() ? lldb_.toggleBreakpoint(file, line) : gdb_.toggleBreakpoint(file, line);
}
auto DebugClient::addBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool {
  return useLldb() ? lldb_.addBreakpoint(file, line) : gdb_.addBreakpoint(file, line);
}
auto DebugClient::updateBreakpoint(const DebugBreakpoint& breakpoint) -> bool {
  return useLldb() ? lldb_.updateBreakpoint(breakpoint) : gdb_.updateBreakpoint(breakpoint);
}
auto DebugClient::removeBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool {
  return useLldb() ? lldb_.removeBreakpoint(file, line) : gdb_.removeBreakpoint(file, line);
}
void DebugClient::clearBreakpoints() { useLldb() ? lldb_.clearBreakpoints() : gdb_.clearBreakpoints(); }
auto DebugClient::hasBreakpoint(const std::filesystem::path& file, std::size_t line) const -> bool {
  return useLldb() ? lldb_.hasBreakpoint(file, line) : gdb_.hasBreakpoint(file, line);
}
auto DebugClient::breakpoints() const -> std::vector<DebugBreakpoint> {
  return useLldb() ? lldb_.breakpoints() : gdb_.breakpoints();
}
void DebugClient::refreshState() { useLldb() ? lldb_.refreshState() : gdb_.refreshState(); }
void DebugClient::poll() { useLldb() ? lldb_.poll() : gdb_.poll(); }
auto DebugClient::running() const -> bool { return useLldb() ? lldb_.running() : gdb_.running(); }
auto DebugClient::active() const -> bool { return useLldb() ? lldb_.active() : gdb_.active(); }
auto DebugClient::stopped() const -> bool { return useLldb() ? lldb_.stopped() : gdb_.stopped(); }
auto DebugClient::exited() const -> bool { return useLldb() ? lldb_.exited() : gdb_.exited(); }
auto DebugClient::mode() const -> DebugSessionMode {
  return useLldb() ? (running() ? DebugSessionMode::Launch : DebugSessionMode::None) : gdb_.mode();
}
auto DebugClient::frames() const -> const std::vector<DebugFrame>& {
  return useLldb() ? lldb_.frames() : gdb_.frames();
}
auto DebugClient::variables() const -> const std::vector<DebugVariable>& {
  return useLldb() ? lldb_.variables() : gdb_.variables();
}
auto DebugClient::threads() const -> const std::vector<DebugThread>& {
  return useLldb() ? lldb_.threads() : gdb_.threads();
}
auto DebugClient::watches() const -> const std::vector<DebugWatch>& {
  return useLldb() ? lldb_.watches() : gdb_.watches();
}
auto DebugClient::registers() const -> const std::vector<DebugRegister>& {
  return useLldb() ? lldb_.registers() : gdb_.registers();
}
auto DebugClient::registersEnabled() const -> bool {
  return useLldb() ? lldb_.registersEnabled() : gdb_.registersEnabled();
}
auto DebugClient::selectedFrame() const -> int {
  return useLldb() ? lldb_.selectedFrame() : gdb_.selectedFrame();
}
auto DebugClient::takeResults() -> std::vector<DebugResult> {
  return useLldb() ? lldb_.takeResults() : gdb_.takeResults();
}
auto DebugClient::takeOutput() -> std::vector<std::string> {
  return useLldb() ? lldb_.takeOutput() : gdb_.takeOutput();
}

void DebugClient::migrateModel(DebugBackend target) {
  if (backend_ == target) return;
  const auto old_breakpoints = breakpoints();
  const auto old_watches = watches();
  const auto old_registers = registersEnabled();
  if (target == DebugBackend::LldbDap) {
    lldb_.clearSessionState();
    for (const auto& item : old_breakpoints) {
      lldb_.addBreakpoint(item.file, item.line); lldb_.updateBreakpoint(item);
    }
    for (const auto& item : old_watches) lldb_.addWatch(item.expression);
    lldb_.setRegistersEnabled(old_registers);
  } else {
    gdb_.clearSessionState();
    for (const auto& item : old_breakpoints) {
      gdb_.addBreakpoint(item.file, item.line); gdb_.updateBreakpoint(item);
    }
    for (const auto& item : old_watches) gdb_.addWatch(item.expression);
    gdb_.setRegistersEnabled(old_registers);
  }
}

auto DebugClient::useLldb() const -> bool { return backend_ == DebugBackend::LldbDap; }

}  // namespace tuiide
