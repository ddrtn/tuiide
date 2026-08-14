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

AsyncProcess::~AsyncProcess() { stop(); }

auto AsyncProcess::start(const std::vector<std::string>& arguments, bool merge_stderr,
  const std::filesystem::path& working_directory,
  const std::map<std::string, std::string>& environment) -> bool {
  if (arguments.empty() || running_) return false;
  int input_pipe[2]{};
  int output_pipe[2]{};
  if (::pipe(input_pipe) != 0 || ::pipe(output_pipe) != 0) return false;
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    // Give the command and every process it spawns a private process group so
    // stop() can also terminate descendants that inherited our output pipe.
    (void)::setpgid(0, 0);
    if (!working_directory.empty() && ::chdir(working_directory.c_str()) != 0) _exit(126);
    for (const auto& [name, value] : environment)
      if (::setenv(name.c_str(), value.c_str(), 1) != 0) _exit(126);
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    if (merge_stderr) ::dup2(output_pipe[1], STDERR_FILENO);
    else {
      const int null_fd = ::open("/dev/null", O_WRONLY);
      if (null_fd >= 0) ::dup2(null_fd, STDERR_FILENO);
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
  exit_code_ = -1;
  running_ = true;
  reader_ = std::jthread([this, fd = output_pipe[0]] { readerLoop(fd); });
  waiter_ = std::jthread([this, child] { waiterLoop(child); });
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
  {
    std::lock_guard lock(input_mutex_);
    if (input_fd_ >= 0) { ::close(input_fd_); input_fd_ = -1; }
  }
  if (child > 0) {
    // The leader may already have exited while a descendant still holds the
    // output pipe open. Killing the group guarantees readerLoop can reach EOF.
    if (::kill(-child, SIGTERM) != 0 && errno != ESRCH) (void)::kill(child, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (running_ && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // clangd and build tools may delay SIGTERM while workers are active. A
    // bounded fallback prevents shutdown from waiting forever on inherited
    // pipes; sending it to a vanished group is harmless (ESRCH).
    (void)::kill(-child, SIGKILL);
    if (running_) (void)::kill(child, SIGKILL);
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
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
  running_ = false;
}

}  // namespace tuiide
