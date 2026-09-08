#include "tuiide/process.hpp"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace tuiide {
namespace {

void closeDescriptor(int& fd) noexcept {
  if (fd < 0) return;
  (void)::close(fd);
  fd = -1;
}

auto processGroupExists(int process_group) noexcept -> bool {
  if (process_group <= 0) return false;
  if (::kill(-process_group, 0) == 0) return true;
  return errno == EPERM;
}

void signalProcessGroup(int process_group, int leader, int signal) noexcept {
  if (process_group > 0 && ::kill(-process_group, signal) == 0) return;
  if (leader > 0) (void)::kill(leader, signal);
}

}  // namespace

AsyncProcess::~AsyncProcess() { stop(); }

auto AsyncProcess::start(const std::vector<std::string>& arguments, bool merge_stderr,
  const std::filesystem::path& working_directory,
  const std::map<std::string, std::string>& environment) -> bool {
  if (running_) return false;
  // Завершившийся лидер мог оставить потомков, reader и входной descriptor.
  // Перед повторным запуском полностью освобождаем ресурсы прежней группы.
  stop();
  if (arguments.empty()) return false;
  int input_pipe[2]{-1, -1};
  int output_pipe[2]{-1, -1};
  if (::pipe2(input_pipe, O_CLOEXEC) != 0) return false;
  if (::pipe2(output_pipe, O_CLOEXEC) != 0) {
    closeDescriptor(input_pipe[0]); closeDescriptor(input_pipe[1]);
    return false;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    closeDescriptor(input_pipe[0]); closeDescriptor(input_pipe[1]);
    closeDescriptor(output_pipe[0]); closeDescriptor(output_pipe[1]);
    return false;
  }
  if (child == 0) {
    // Команда и все её потомки получают отдельную process group. Поэтому stop()
    // завершает также процессы, унаследовавшие pipe (например, worker CMake).
    if (::setpgid(0, 0) != 0) _exit(126);
    if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0) _exit(126);
    for (const auto& [name, value] : environment)
      if (::setenv(name.c_str(), value.c_str(), 1) != 0) _exit(126);
    if (::dup2(input_pipe[0], STDIN_FILENO) < 0
        || ::dup2(output_pipe[1], STDOUT_FILENO) < 0)
      _exit(126);
    if (merge_stderr) {
      if (::dup2(output_pipe[1], STDERR_FILENO) < 0) _exit(126);
    }
    else {
      const int null_fd = ::open("/dev/null", O_WRONLY);
      if (null_fd < 0 || ::dup2(null_fd, STDERR_FILENO) < 0) _exit(126);
      ::close(null_fd);
    }
    ::close(input_pipe[0]); ::close(input_pipe[1]);
    ::close(output_pipe[0]); ::close(output_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv.front(), argv.data());
    _exit(127);
  }
  (void)::setpgid(child, child);
  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  input_fd_ = input_pipe[1];
  pid_ = child;
  process_group_ = child;
  exit_code_ = -1;
  running_ = true;
  try {
    reader_ = std::jthread([this, fd = output_pipe[0]] { readerLoop(fd); });
    output_pipe[0] = -1;  // readerLoop владеет этим концом pipe.
    waiter_ = std::jthread([this, child] { waiterLoop(child); });
  } catch (...) {
    closeInput();
    closeDescriptor(output_pipe[0]);
    signalProcessGroup(child, child, SIGKILL);
    int status{};
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (reader_.joinable()) reader_.join();
    pid_ = -1; process_group_ = -1; running_ = false; exit_code_ = -1;
    return false;
  }
  return true;
}

auto AsyncProcess::write(std::string_view data) -> bool {
  std::lock_guard lock(input_mutex_);
  if (input_fd_ < 0) return false;
  std::size_t offset{};
  while (offset < data.size()) {
    const auto count = ::write(input_fd_, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

void AsyncProcess::closeInput() {
  std::lock_guard lock(input_mutex_);
  if (input_fd_ >= 0) { ::close(input_fd_); input_fd_ = -1; }
}

auto AsyncProcess::drain() -> std::vector<std::string> {
  std::lock_guard lock(output_mutex_);
  std::vector<std::string> result;
  result.swap(output_);
  return result;
}

auto AsyncProcess::running() const -> bool { return running_; }
auto AsyncProcess::exitCode() const -> std::optional<int> {
  if (running_ || exit_code_ < 0) return std::nullopt;
  return exit_code_.load();
}

void AsyncProcess::stop() {
  const int child = pid_.exchange(-1);
  const int process_group = process_group_.exchange(-1);
  {
    std::lock_guard lock(input_mutex_);
    if (input_fd_ >= 0) { ::close(input_fd_); input_fd_ = -1; }
  }
  if (child > 0 || process_group > 0) {
    // Лидер мог завершиться, пока потомок удерживает output pipe. Убийство всей
    // группы гарантирует EOF для readerLoop и не оставляет поток ожидания висеть.
    signalProcessGroup(process_group, running_ ? child : -1, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while ((running_ || processGroupExists(process_group))
        && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // clangd и сборочные утилиты могут отложить SIGTERM из-за worker-процессов.
    // Ограниченный fallback не даёт shutdown ждать pipe бесконечно; ESRCH для
    // уже исчезнувшей группы безопасен.
    if (processGroupExists(process_group) || running_)
      signalProcessGroup(process_group, running_ ? child : -1, SIGKILL);
  }
  if (reader_.joinable()) reader_.join();
  if (waiter_.joinable()) waiter_.join();
  running_ = false;
}

void AsyncProcess::readerLoop(int fd) {
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    std::lock_guard lock(output_mutex_);
    output_.emplace_back(buffer, static_cast<std::size_t>(count));
  }
  ::close(fd);
}

void AsyncProcess::waiterLoop(int child) {
  int status{};
  pid_t result{};
  do result = ::waitpid(child, &status, 0); while (result < 0 && errno == EINTR);
  if (result == child && WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
  else if (result == child && WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
  running_ = false;
}

}  // namespace tuiide
