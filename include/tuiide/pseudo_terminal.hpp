#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tuiide {

/**
 * Linux PTY с потоками чтения и ожидания дочернего процесса.
 * Методы drain() и resize() связывают видимый размер консоли UI с процессом,
 * а stop() завершает жизненный цикл без утечки дескрипторов.
 */
class PseudoTerminal {
 public:
  PseudoTerminal() = default;
  ~PseudoTerminal();
  PseudoTerminal(const PseudoTerminal&) = delete;
  auto operator=(const PseudoTerminal&) -> PseudoTerminal& = delete;

  auto start(const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory = {},
    const std::map<std::string, std::string>& environment = {},
    unsigned columns = 80, unsigned rows = 24) -> bool;
  /** Создаёт PTY без процесса: slave передаётся GDB как tty inferior. */
  auto openSession(unsigned columns = 80, unsigned rows = 24) -> bool;
  void activateSession();
  [[nodiscard]] auto slaveName() const -> std::filesystem::path;
  auto write(std::string_view data) -> bool;
  auto resize(unsigned columns, unsigned rows) -> bool;
  auto sendSignal(int signal) -> bool;
  auto drain() -> std::vector<std::string>;
  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto exitCode() const -> std::optional<int>;
  void stop();

 private:
  void startReader();
  void readerLoop();
  void waiterLoop(int pid);

  mutable std::mutex fd_mutex_;
  int master_fd_{-1};
  int held_slave_fd_{-1};
  std::filesystem::path slave_name_;
  std::atomic<int> pid_{-1};
  std::atomic<bool> running_{false};
  std::atomic<bool> session_{false};
  std::atomic<int> exit_code_{-1};
  std::mutex output_mutex_;
  std::vector<std::string> output_;
  std::jthread reader_;
  std::jthread waiter_;
};

}  // namespace tuiide
