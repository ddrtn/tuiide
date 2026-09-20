#pragma once

#include "tuiide/gdb_client.hpp"
#include "tuiide/lldb_dap_client.hpp"

#include <filesystem>
#include <string>

namespace tuiide {

enum class DebugBackend { GdbMi, LldbDap };

[[nodiscard]] auto debugBackendName(DebugBackend backend) -> std::string;
[[nodiscard]] auto parseDebugBackend(std::string_view value) -> DebugBackend;

/** Единый фасад GDB/MI и LLDB/DAP для IDE и DebugUiController. */
class DebugClient {
 public:
  void configure(DebugBackend backend, std::filesystem::path adapter = {});
  [[nodiscard]] auto backend() const -> DebugBackend;
  [[nodiscard]] auto backendName() const -> std::string;

  auto start(const std::filesystem::path& executable,
    const std::filesystem::path& working_directory = {},
    const std::map<std::string, std::string>& environment = {},
    const std::vector<std::string>& arguments = {},
    const std::filesystem::path& stdin_file = {},
    const std::filesystem::path& inferior_tty = {}) -> bool;
  auto attach(int pid, const std::filesystem::path& working_directory = {}) -> bool;
  auto openCore(const std::filesystem::path& executable,
    const std::filesystem::path& core_file,
    const std::filesystem::path& working_directory = {}) -> bool;
  auto stop() -> bool;
  void clearSessionState();
  void run();
  void interrupt();
  void continueExecution();
  void next();
  void step();
  void finish();
  auto sendSignal(std::string_view signal) -> bool;
  auto setSignalPolicy(std::string_view signal, bool stop, bool print, bool pass) -> bool;
  auto inspectSignals() -> bool;
  void selectThread(const std::string& id);
  auto selectFrame(int level) -> bool;
  auto toggleVariable(std::size_t index) -> bool;
  auto evaluate(std::string expression) -> bool;
  auto assign(std::string expression, std::string value) -> bool;
  auto disassemble(std::string address, std::size_t bytes = 128) -> bool;
  auto readMemory(std::string address, std::size_t bytes) -> bool;
  auto addWatch(std::string expression) -> bool;
  auto removeWatch(std::size_t index) -> bool;
  void setRegistersEnabled(bool enabled);
  auto toggleBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool;
  auto addBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool;
  auto updateBreakpoint(const DebugBreakpoint& breakpoint) -> bool;
  auto removeBreakpoint(const std::filesystem::path& file, std::size_t line) -> bool;
  void clearBreakpoints();
  [[nodiscard]] auto hasBreakpoint(const std::filesystem::path& file,
    std::size_t line) const -> bool;
  [[nodiscard]] auto breakpoints() const -> std::vector<DebugBreakpoint>;
  void refreshState();
  void poll();
  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto active() const -> bool;
  [[nodiscard]] auto stopped() const -> bool;
  [[nodiscard]] auto exited() const -> bool;
  [[nodiscard]] auto mode() const -> DebugSessionMode;
  [[nodiscard]] auto frames() const -> const std::vector<DebugFrame>&;
  [[nodiscard]] auto variables() const -> const std::vector<DebugVariable>&;
  [[nodiscard]] auto threads() const -> const std::vector<DebugThread>&;
  [[nodiscard]] auto watches() const -> const std::vector<DebugWatch>&;
  [[nodiscard]] auto registers() const -> const std::vector<DebugRegister>&;
  [[nodiscard]] auto registersEnabled() const -> bool;
  [[nodiscard]] auto selectedFrame() const -> int;
  auto takeResults() -> std::vector<DebugResult>;
  auto takeOutput() -> std::vector<std::string>;

 private:
  void migrateModel(DebugBackend target);
  [[nodiscard]] auto useLldb() const -> bool;

  DebugBackend backend_{DebugBackend::GdbMi};
  std::filesystem::path adapter_;
  GdbClient gdb_;
  LldbDapClient lldb_;
};

}  // namespace tuiide
