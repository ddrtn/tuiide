#include "tuiide/pseudo_terminal.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <pty.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace tuiide {
namespace {
auto windowSize(unsigned columns, unsigned rows) -> winsize {
  return {static_cast<unsigned short>(rows), static_cast<unsigned short>(columns), 0, 0};
}

auto mergedEnvironment(const std::map<std::string, std::string>& overrides)
    -> std::vector<std::string> {
  std::map<std::string, std::string> values;
  for (auto entry = ::environ; entry && *entry; ++entry) {
    const std::string_view item(*entry);
    const auto separator = item.find('=');
    if (separator != std::string_view::npos)
      values[std::string(item.substr(0, separator))] = std::string(item.substr(separator + 1));
  }
  for (const auto& [name, value] : overrides) values[name] = value;
  values.try_emplace("TERM", "xterm-256color");
  std::vector<std::string> result;
  result.reserve(values.size());
  for (const auto& [name, value] : values) result.push_back(name + "=" + value);
  return result;
}

auto environmentValue(const std::vector<std::string>& environment, std::string_view name)
    -> std::string {
  const auto prefix = std::string(name) + "=";
  for (const auto& item : environment)
    if (item.starts_with(prefix)) return item.substr(prefix.size());
  return {};
}

auto resolveExecutable(const std::string& executable, const std::filesystem::path& working_directory,
    const std::vector<std::string>& environment) -> std::filesystem::path {
  if (executable.find('/') != std::string::npos) return executable;
  const auto path = environmentValue(environment, "PATH");
  std::size_t begin{};
  while (begin <= path.size()) {
    const auto end = path.find(':', begin);
    auto directory = path.substr(begin, end == std::string::npos ? path.size() - begin : end - begin);
    std::filesystem::path candidate;
    if (directory.empty()) candidate = working_directory.empty() ? std::filesystem::current_path() : working_directory;
    else {
      candidate = directory;
      if (candidate.is_relative() && !working_directory.empty()) candidate = working_directory / candidate;
    }
    candidate /= executable;
    if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return {};
}
}

PseudoTerminal::~PseudoTerminal() { stop(); }

auto PseudoTerminal::start(const std::vector<std::string>& arguments,
    const std::filesystem::path& working_directory,
    const std::map<std::string, std::string>& environment,
    unsigned columns, unsigned rows) -> bool {
  if (arguments.empty() || running_) return false;
  auto environment_values = mergedEnvironment(environment);
  const auto executable = resolveExecutable(arguments.front(), working_directory, environment_values);
  if (executable.empty()) return false;
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  std::vector<char*> envp;
  envp.reserve(environment_values.size() + 1);
  for (auto& entry : environment_values) envp.push_back(entry.data());
  envp.push_back(nullptr);
  auto size = windowSize(columns, rows);
  int master{-1};
  int slave{-1};
  char slave_name[256]{};
  if (::openpty(&master, &slave, slave_name, nullptr, &size) != 0) return false;
  posix_spawn_file_actions_t actions;
  posix_spawnattr_t attributes;
  if (::posix_spawn_file_actions_init(&actions) != 0 || ::posix_spawnattr_init(&attributes) != 0) {
    ::close(master); ::close(slave); return false;
  }
  (void)::posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  (void)::posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  (void)::posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  (void)::posix_spawn_file_actions_addclose(&actions, master);
  if (slave > STDERR_FILENO) (void)::posix_spawn_file_actions_addclose(&actions, slave);
  if (!working_directory.empty())
    (void)::posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
  (void)::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  (void)::posix_spawnattr_setpgroup(&attributes, 0);
  pid_t child{-1};
  const auto spawn_error = ::posix_spawn(&child, executable.c_str(), &actions, &attributes,
    argv.data(), envp.data());
  (void)::posix_spawn_file_actions_destroy(&actions);
  (void)::posix_spawnattr_destroy(&attributes);
  ::close(slave);
  if (spawn_error != 0) { ::close(master); return false; }
  {
    std::lock_guard lock(fd_mutex_);
    master_fd_ = master; slave_name_ = slave_name;
  }
  pid_ = child; exit_code_ = -1; session_ = false; running_ = true;
  startReader();
  waiter_ = std::jthread([this, child] { waiterLoop(child); });
  return true;
}

auto PseudoTerminal::openSession(unsigned columns, unsigned rows) -> bool {
  if (running_) return false;
  int master{-1};
  int slave{-1};
  char name[256]{};
  auto size = windowSize(columns, rows);
  if (::openpty(&master, &slave, name, nullptr, &size) != 0) return false;
  {
    std::lock_guard lock(fd_mutex_);
    master_fd_ = master; held_slave_fd_ = slave; slave_name_ = name;
  }
  pid_ = -1; exit_code_ = -1; session_ = true; running_ = true;
  return true;
}

void PseudoTerminal::activateSession() {
  if (!running_ || !session_) return;
  {
    std::lock_guard lock(fd_mutex_);
    if (held_slave_fd_ >= 0) { ::close(held_slave_fd_); held_slave_fd_ = -1; }
  }
  startReader();
}

auto PseudoTerminal::slaveName() const -> std::filesystem::path {
  std::lock_guard lock(fd_mutex_);
  return slave_name_;
}

auto PseudoTerminal::write(std::string_view data) -> bool {
  std::lock_guard lock(fd_mutex_);
  if (master_fd_ < 0) return false;
  std::size_t offset{};
  while (offset < data.size()) {
    const auto count = ::write(master_fd_, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

auto PseudoTerminal::resize(unsigned columns, unsigned rows) -> bool {
  std::lock_guard lock(fd_mutex_);
  if (master_fd_ < 0) return false;
  auto size = windowSize(columns, rows);
  return ::ioctl(master_fd_, TIOCSWINSZ, &size) == 0;
}

auto PseudoTerminal::sendSignal(int signal) -> bool {
  const auto child = pid_.load();
  return child > 0 && (::kill(-child, signal) == 0 || ::kill(child, signal) == 0);
}

auto PseudoTerminal::drain() -> std::vector<std::string> {
  std::lock_guard lock(output_mutex_);
  std::vector<std::string> result;
  result.swap(output_);
  return result;
}

auto PseudoTerminal::running() const -> bool { return running_; }
auto PseudoTerminal::exitCode() const -> std::optional<int> {
  if (running_ || session_ || exit_code_ < 0) return std::nullopt;
  return exit_code_.load();
}

void PseudoTerminal::stop() {
  running_ = false;
  const auto child = pid_.exchange(-1);
  if (child > 0) {
    (void)::kill(-child, SIGTERM);
    (void)::kill(child, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (exit_code_ < 0 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)::kill(-child, SIGKILL);
    (void)::kill(child, SIGKILL);
  }
  {
    std::lock_guard lock(fd_mutex_);
    if (held_slave_fd_ >= 0) { ::close(held_slave_fd_); held_slave_fd_ = -1; }
    if (master_fd_ >= 0) { ::close(master_fd_); master_fd_ = -1; }
    slave_name_.clear();
  }
  if (reader_.joinable()) reader_.join();
  if (waiter_.joinable()) waiter_.join();
  session_ = false;
}

void PseudoTerminal::startReader() {
  reader_ = std::jthread([this] { readerLoop(); });
}

void PseudoTerminal::readerLoop() {
  char buffer[4096];
  for (;;) {
    int fd{-1};
    { std::lock_guard lock(fd_mutex_); fd = master_fd_; }
    if (fd < 0) break;
    pollfd descriptor{fd, POLLIN, 0};
    const auto ready = ::poll(&descriptor, 1, 100);
    if (ready < 0 && errno == EINTR) continue;
    if (ready == 0) { if (!running_) break; continue; }
    if (ready < 0) { if (!running_) break; continue; }
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count < 0 && (errno == EINTR || (errno == EIO && running_))) continue;
    if (count <= 0) break;
    std::lock_guard lock(output_mutex_);
    output_.emplace_back(buffer, static_cast<std::size_t>(count));
  }
}

void PseudoTerminal::waiterLoop(int child) {
  int status{};
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
  else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
  running_ = false;
}

}  // namespace tuiide
