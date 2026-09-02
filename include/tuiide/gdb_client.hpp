#pragma once

#include "tuiide/process.hpp"

#include <filesystem>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tuiide {

/** Один кадр стека GDB; строка имеет привычную для пользователя нумерацию с 1. */
struct DebugFrame {
  int level{};
  std::string function;
  std::filesystem::path file;
  std::size_t line{};
};

/** Локальная переменная или дочерний элемент compound-значения GDB. */
struct DebugVariable {
  std::string name;
  std::string value;
  std::string type;
  std::string expression;
  std::string object;
  std::size_t depth{};
  bool expandable{};
  bool expanded{};
};

struct DebugThread {
  std::string id;
  std::string name;
  std::string state;
  bool current{};
};

struct DebugWatch {
  std::string expression;
  std::string value;
  std::string error;
};

struct DebugRegister {
  std::string name;
  std::string value;
};

enum class DebugResultKind { Evaluation, Assignment, Disassembly, Memory };
struct DebugResult {
  DebugResultKind kind{DebugResultKind::Evaluation};
  std::string expression;
  std::string value;
  std::string error;
};

/** Состояние breakpoint/logpoint, синхронизируемое с GDB и проектной сессией. */
struct DebugBreakpoint {
  std::filesystem::path file;
  std::size_t line{};
  bool enabled{true};
  std::string condition;
  unsigned hit_count{};
  std::string log_message;
  bool verified{};
  std::string error;
};

[[nodiscard]] auto gdbEvaluateCommand(std::string_view expression) -> std::string;
[[nodiscard]] auto gdbDisassembleCommand(std::string_view address,
  std::size_t bytes) -> std::string;
[[nodiscard]] auto gdbReadMemoryCommand(std::string_view address,
  std::size_t bytes) -> std::string;

/**
 * Асинхронный клиент GDB/MI.
 * Управляет процессом GDB, маркирует запросы токенами и превращает MI-ответы в
 * типизированные данные для панелей отладки; raw-протокол не выходит в UI.
 */
class GdbClient {
 public:
  /** Запускает GDB с выбранным executable и необязательным PTY для inferior. */
  auto start(const std::filesystem::path& executable,
    const std::filesystem::path& working_directory = {},
    const std::map<std::string, std::string>& environment = {},
    const std::vector<std::string>& arguments = {},
    const std::filesystem::path& stdin_file = {},
    const std::filesystem::path& inferior_tty = {}) -> bool;
  /** Останавливает GDB и сбрасывает запросы, не оставляя фонового процесса. */
  void stop();
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
  [[nodiscard]] auto hasBreakpoint(const std::filesystem::path& file, std::size_t line) const -> bool;
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
  auto command(std::string value) -> int;
  void insertBreakpoint(const std::string& key);
  void startRequestedRun();
  void refreshWatches();
  void refreshRegisters();
  void refreshVariables();
  void markWatchesUnavailable(std::string message);
  static auto breakpointKey(const std::filesystem::path& file, std::size_t line) -> std::string;
  static auto quote(const std::filesystem::path& value) -> std::string;

  AsyncProcess process_;
  std::string partial_;
  std::vector<std::string> output_;
  int next_token_{1};
  bool inferior_active_{};
  bool stopped_{};
  bool inferior_exited_{};
  std::unordered_map<std::string, DebugBreakpoint> desired_breakpoints_;
  std::unordered_map<std::string, std::string> breakpoint_numbers_;
  std::unordered_map<int, std::string> pending_breakpoints_;
  std::unordered_map<int, std::string> pending_breakpoint_commands_;
  std::unordered_map<int, std::string> pending_watches_;
  struct PendingVariable {
    std::uint64_t generation{};
    std::string expression;
  };
  struct PendingChildren {
    std::uint64_t generation{};
    std::string object;
  };
  std::unordered_map<int, PendingVariable> pending_variables_;
  std::unordered_map<int, PendingChildren> pending_children_;
  struct PendingExpression {
    DebugResultKind kind{DebugResultKind::Evaluation};
    std::string expression;
  };
  std::unordered_map<int, PendingExpression> pending_expressions_;
  std::vector<DebugFrame> frames_;
  std::vector<DebugVariable> variables_;
  std::vector<DebugThread> threads_;
  std::vector<DebugWatch> watches_;
  std::vector<std::string> register_names_;
  std::vector<DebugRegister> registers_;
  std::vector<DebugResult> results_;
  bool registers_enabled_{};
  std::uint64_t variable_generation_{};
  int selected_frame_{};
  std::filesystem::path stdin_file_;
  bool run_requested_{};
};

}  // namespace tuiide
