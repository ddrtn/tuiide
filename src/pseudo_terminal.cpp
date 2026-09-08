#include "tuiide/pseudo_terminal.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
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
void closeDescriptor(int& fd) noexcept {
  if (fd < 0) return;
  (void)::close(fd);
  fd = -1;
}

auto terminalDimension(unsigned value) -> unsigned short {
  return static_cast<unsigned short>(std::clamp(value, 1U,
    static_cast<unsigned>(std::numeric_limits<unsigned short>::max())));
}

auto windowSize(unsigned columns, unsigned rows) -> winsize {
  return {terminalDimension(rows), terminalDimension(columns), 0, 0};
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
  if (running_ || session_) return false;
  stop();
  if (arguments.empty()) return false;
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
  if (::fcntl(master, F_SETFD, FD_CLOEXEC) != 0
      || ::fcntl(slave, F_SETFD, FD_CLOEXEC) != 0) {
    closeDescriptor(master); closeDescriptor(slave); return false;
  }
  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    closeDescriptor(master); closeDescriptor(slave); return false;
  }
  posix_spawnattr_t attributes;
  if (::posix_spawnattr_init(&attributes) != 0) {
    (void)::posix_spawn_file_actions_destroy(&actions);
    closeDescriptor(master); closeDescriptor(slave); return false;
  }
  int setup_error = ::posix_spawn_file_actions_adddup2(&actions, slave, STDIN_FILENO);
  if (setup_error == 0) setup_error = ::posix_spawn_file_actions_adddup2(&actions, slave, STDOUT_FILENO);
  if (setup_error == 0) setup_error = ::posix_spawn_file_actions_adddup2(&actions, slave, STDERR_FILENO);
  if (setup_error == 0) setup_error = ::posix_spawn_file_actions_addclose(&actions, master);
  if (setup_error == 0 && slave > STDERR_FILENO)
    setup_error = ::posix_spawn_file_actions_addclose(&actions, slave);
  if (setup_error == 0 && !working_directory.empty())
    setup_error = ::posix_spawn_file_actions_addchdir_np(&actions, working_directory.c_str());
  if (setup_error == 0)
    setup_error = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  if (setup_error == 0) setup_error = ::posix_spawnattr_setpgroup(&attributes, 0);
  pid_t child{-1};
  const auto spawn_error = setup_error == 0
    ? ::posix_spawn(&child, executable.c_str(), &actions, &attributes,
        argv.data(), envp.data())
    : setup_error;
  (void)::posix_spawn_file_actions_destroy(&actions);
  (void)::posix_spawnattr_destroy(&attributes);
  closeDescriptor(slave);
  if (spawn_error != 0) { closeDescriptor(master); return false; }
  {
    std::lock_guard lock(fd_mutex_);
    master_fd_ = master; slave_name_ = slave_name;
  }
  pid_ = child; process_group_ = child; exit_code_ = -1; session_ = false; running_ = true;
  try {
    waiter_ = std::jthread([this, child] { waiterLoop(child); });
  } catch (...) {
    signalProcessGroup(child, child, SIGKILL);
    int status{};
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    {
      std::lock_guard lock(fd_mutex_);
      closeDescriptor(master_fd_); slave_name_.clear();
    }
    pid_ = -1; process_group_ = -1; running_ = false; exit_code_ = -1;
    return false;
  }
  if (!startReader()) { stop(); return false; }
  return true;
}

auto PseudoTerminal::openSession(unsigned columns, unsigned rows) -> bool {
  if (running_ || session_) return false;
  stop();
  int master{-1};
  int slave{-1};
  char name[256]{};
  auto size = windowSize(columns, rows);
  if (::openpty(&master, &slave, name, nullptr, &size) != 0) return false;
  if (::fcntl(master, F_SETFD, FD_CLOEXEC) != 0
      || ::fcntl(slave, F_SETFD, FD_CLOEXEC) != 0) {
    closeDescriptor(master); closeDescriptor(slave); return false;
  }
  {
    std::lock_guard lock(fd_mutex_);
    master_fd_ = master; held_slave_fd_ = slave; slave_name_ = name;
  }
  pid_ = -1; process_group_ = -1; exit_code_ = -1; session_ = true; running_ = true;
  return true;
}

void PseudoTerminal::activateSession() {
  if (!running_ || !session_) return;
  {
    std::lock_guard lock(fd_mutex_);
    if (held_slave_fd_ < 0) return;
  }
  if (!startReader()) { stop(); return; }
  {
    std::lock_guard lock(fd_mutex_);
    if (held_slave_fd_ >= 0) { ::close(held_slave_fd_); held_slave_fd_ = -1; }
  }
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
  const auto process_group = process_group_.load();
  if (process_group > 0 && ::kill(-process_group, signal) == 0) return true;
  return running_ && child > 0 && ::kill(child, signal) == 0;
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
  const bool process_was_running = running_.exchange(false);
  const auto child = pid_.exchange(-1);
  const auto process_group = process_group_.exchange(-1);
  if (child > 0 || process_group > 0) {
    signalProcessGroup(process_group, process_was_running ? child : -1, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while ((exit_code_ < 0 || processGroupExists(process_group))
        && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (exit_code_ < 0 || processGroupExists(process_group))
      signalProcessGroup(process_group, process_was_running ? child : -1, SIGKILL);
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

auto PseudoTerminal::startReader() -> bool {
  int fd{-1};
  {
    std::lock_guard lock(fd_mutex_);
    if (master_fd_ < 0) return false;
    fd = ::fcntl(master_fd_, F_DUPFD_CLOEXEC, 0);
  }
  if (fd < 0) return false;
  try {
    reader_ = std::jthread([this, fd] { readerLoop(fd); });
  } catch (...) {
    closeDescriptor(fd);
    return false;
  }
  return true;
}

void PseudoTerminal::readerLoop(int fd) {
  char buffer[4096];
  for (;;) {
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
  closeDescriptor(fd);
}

void PseudoTerminal::waiterLoop(int child) {
  int status{};
  pid_t result{};
  do result = ::waitpid(child, &status, 0); while (result < 0 && errno == EINTR);
  if (result == child && WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
  else if (result == child && WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
  running_ = false;
}

}  // namespace tuiide
