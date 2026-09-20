#pragma once

#include "tuiide/gdb_client.hpp"
#include "tuiide/process.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace tuiide {

/**
 * Асинхронный клиент Debug Adapter Protocol для lldb-dap.
 * Публичная модель намеренно совпадает с GdbClient: UI не зависит от wire-
 * протокола выбранного отладчика.
 */
class LldbDapClient {
 public:
  ~LldbDapClient();

  auto start(const std::filesystem::path& adapter,
    const std::filesystem::path& executable,
    const std::filesystem::path& working_directory = {},
    const std::map<std::string, std::string>& environment = {},
    const std::vector<std::string>& arguments = {},
    const std::filesystem::path& inferior_tty = {}) -> bool;
  auto stop() -> bool;
  void clearSessionState();
  void run();
  void interrupt();
  void continueExecution();
  void next();
  void step();
  void finish();
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
  struct PendingRequest {
    std::string command;
    std::string context;
  };
  auto request(std::string command, nlohmann::json arguments = {}) -> int;
  auto request(std::string command, nlohmann::json arguments,
    std::string context) -> int;
  void handleMessage(const nlohmann::json& message);
  void handleResponse(const nlohmann::json& message);
  void handleEvent(const nlohmann::json& message);
  void sendLaunch();
  void configureBreakpoints();
  void requestFrames();
  void requestScopes();
  void refreshWatches();
  void markWatchesUnavailable(std::string message);
  static auto breakpointKey(const std::filesystem::path& file,
    std::size_t line) -> std::string;

  AsyncProcess process_;
  std::string input_;
  std::vector<std::string> output_;
  std::unordered_map<int, PendingRequest> pending_;
  int next_sequence_{1};
  bool initialized_{};
  bool launch_sent_{};
  bool configured_{};
  bool active_{};
  bool stopped_{};
  bool exited_{};
  int selected_thread_{};
  int selected_frame_id_{};
  int selected_frame_{};
  std::filesystem::path executable_;
  std::filesystem::path working_directory_;
  std::map<std::string, std::string> environment_;
  std::vector<std::string> arguments_;
  std::filesystem::path inferior_tty_;
  std::unordered_map<std::string, DebugBreakpoint> breakpoints_;
  std::vector<DebugFrame> frames_;
  std::vector<int> frame_ids_;
  std::vector<DebugVariable> variables_;
  std::vector<DebugThread> threads_;
  std::vector<DebugWatch> watches_;
  std::vector<DebugRegister> registers_;
  std::vector<DebugResult> results_;
  bool registers_enabled_{};
};

}  // namespace tuiide
