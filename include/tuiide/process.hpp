#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tuiide {

/**
 * Асинхронный POSIX-процесс с отдельным reader-потоком stdout/stderr.
 * Потомок запускается в собственной process group, поэтому stop() завершает и
 * инструменты, порождённые CMake, clangd или shell-обёрткой.
 */
class AsyncProcess {
 public:
  AsyncProcess() = default;
  ~AsyncProcess();
  AsyncProcess(const AsyncProcess&) = delete;
  auto operator=(const AsyncProcess&) -> AsyncProcess& = delete;

  auto start(const std::vector<std::string>& arguments, bool merge_stderr = true,
    const std::filesystem::path& working_directory = {},
    const std::map<std::string, std::string>& environment = {}) -> bool;
  auto write(std::string_view data) -> bool;
  void closeInput();
  auto drain() -> std::vector<std::string>;
  [[nodiscard]] auto running() const -> bool;
  [[nodiscard]] auto exitCode() const -> std::optional<int>;
  /** Посылает SIGTERM группе и использует ограниченный SIGKILL fallback. */
  void stop();

 private:
  void readerLoop(int fd);
  void waiterLoop(int pid);

  int input_fd_{-1};
  std::atomic<int> pid_{-1};
  std::atomic<int> process_group_{-1};
  std::atomic<bool> running_{false};
  std::atomic<int> exit_code_{-1};
  std::mutex input_mutex_;
  std::mutex output_mutex_;
  std::vector<std::string> output_;
  std::jthread reader_;
  std::jthread waiter_;
};

}  // namespace tuiide
