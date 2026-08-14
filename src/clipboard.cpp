#include "tuiide/clipboard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tuiide {
namespace {
using namespace std::chrono_literals;
constexpr std::size_t max_clipboard_bytes = 1024U * 1024U;
constexpr std::size_t max_osc52_bytes = 100U * 1024U;

auto disabled(const char* name) -> bool {
  const auto* value = std::getenv(name);
  return value && (std::string_view(value) == "0" || std::string_view(value) == "false"
    || std::string_view(value) == "off");
}

auto writeAll(int fd, std::string_view value) -> bool {
  std::size_t offset{};
  while (offset < value.size()) {
    const auto count = ::write(fd, value.data() + offset, value.size() - offset);
    if (count <= 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

auto writeCommand(const std::vector<std::string>& arguments, std::string_view input) -> bool {
  if (arguments.empty()) return false;
  int pipe_fds[2]{};
  if (::pipe(pipe_fds) != 0) return false;
  const auto child = ::fork();
  if (child < 0) { ::close(pipe_fds[0]); ::close(pipe_fds[1]); return false; }
  if (child == 0) {
    ::dup2(pipe_fds[0], STDIN_FILENO);
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) { ::dup2(null_fd, STDOUT_FILENO); ::dup2(null_fd, STDERR_FILENO); }
    ::close(pipe_fds[0]); ::close(pipe_fds[1]);
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv.front(), argv.data());
    _exit(127);
  }
  ::close(pipe_fds[0]);
  sigset_t blocked{};
  sigset_t previous{};
  ::sigemptyset(&blocked);
  ::sigaddset(&blocked, SIGPIPE);
  (void)::pthread_sigmask(SIG_BLOCK, &blocked, &previous);
  const auto written = writeAll(pipe_fds[1], input);
  ::close(pipe_fds[1]);
  if (!::sigismember(&previous, SIGPIPE)) {
    timespec immediate{};
    (void)::sigtimedwait(&blocked, nullptr, &immediate);
  }
  (void)::pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  int status{};
  for (int attempt = 0; attempt < 10; ++attempt) {
    const auto result = ::waitpid(child, &status, WNOHANG);
    if (result == child) return written && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    std::this_thread::sleep_for(5ms);
  }
  std::thread([child] { int ignored{}; (void)::waitpid(child, &ignored, 0); }).detach();
  return written;
}

auto readCommand(const std::vector<std::string>& arguments) -> std::string {
  if (arguments.empty()) return {};
  int pipe_fds[2]{};
  if (::pipe(pipe_fds) != 0) return {};
  const auto child = ::fork();
  if (child < 0) { ::close(pipe_fds[0]); ::close(pipe_fds[1]); return {}; }
  if (child == 0) {
    ::dup2(pipe_fds[1], STDOUT_FILENO);
    const int null_input = ::open("/dev/null", O_RDONLY);
    const int null_error = ::open("/dev/null", O_WRONLY);
    if (null_input >= 0) ::dup2(null_input, STDIN_FILENO);
    if (null_error >= 0) ::dup2(null_error, STDERR_FILENO);
    ::close(pipe_fds[0]); ::close(pipe_fds[1]);
    std::vector<char*> argv;
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execvp(argv.front(), argv.data());
    _exit(127);
  }
  ::close(pipe_fds[1]);
  std::string output;
  std::array<char, 4096> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + 300ms;
  while (output.size() < max_clipboard_bytes && std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{pipe_fds[0], POLLIN | POLLHUP, 0};
    const auto ready = ::poll(&descriptor, 1, 20);
    if (ready < 0) break;
    if (ready == 0) continue;
    const auto count = ::read(pipe_fds[0], buffer.data(), std::min(buffer.size(), max_clipboard_bytes - output.size()));
    if (count <= 0) break;
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  ::close(pipe_fds[0]);
  int status{};
  for (int attempt = 0; attempt < 10 && ::waitpid(child, &status, WNOHANG) == 0; ++attempt)
    std::this_thread::sleep_for(5ms);
  if (::waitpid(child, &status, WNOHANG) == 0) {
    ::kill(child, SIGTERM);
    (void)::waitpid(child, &status, 0);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
  return output;
}

void copyNative(std::string_view text) {
  if (disabled("TUIIDE_CLIPBOARD_NATIVE")) return;
  if (std::getenv("WAYLAND_DISPLAY") && writeCommand({"wl-copy", "--type", "text/plain;charset=utf-8"}, text)) return;
  if (!std::getenv("DISPLAY")) return;
  if (writeCommand({"xclip", "-selection", "clipboard", "-in"}, text)) return;
  (void)writeCommand({"xsel", "--clipboard", "--input"}, text);
}

auto pasteNative() -> std::string {
  if (disabled("TUIIDE_CLIPBOARD_NATIVE")) return {};
  if (std::getenv("WAYLAND_DISPLAY")) {
    auto value = readCommand({"wl-paste", "--no-newline"});
    if (!value.empty()) return value;
  }
  if (!std::getenv("DISPLAY")) return {};
  auto value = readCommand({"xclip", "-selection", "clipboard", "-out"});
  if (!value.empty()) return value;
  return readCommand({"xsel", "--clipboard", "--output"});
}
}  // namespace

auto base64Encode(std::string_view value) -> std::string {
  static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve(((value.size() + 2) / 3) * 4);
  for (std::size_t offset = 0; offset < value.size(); offset += 3) {
    const auto first = static_cast<unsigned char>(value[offset]);
    const auto second = offset + 1 < value.size() ? static_cast<unsigned char>(value[offset + 1]) : 0U;
    const auto third = offset + 2 < value.size() ? static_cast<unsigned char>(value[offset + 2]) : 0U;
    const auto bits = (static_cast<unsigned int>(first) << 16U) | (static_cast<unsigned int>(second) << 8U) | third;
    result.push_back(alphabet[(bits >> 18U) & 0x3fU]);
    result.push_back(alphabet[(bits >> 12U) & 0x3fU]);
    result.push_back(offset + 1 < value.size() ? alphabet[(bits >> 6U) & 0x3fU] : '=');
    result.push_back(offset + 2 < value.size() ? alphabet[bits & 0x3fU] : '=');
  }
  return result;
}

auto osc52CopySequence(std::string_view value, bool tmux_passthrough) -> std::string {
  const auto sequence = "\033]52;c;" + base64Encode(value) + "\a";
  if (!tmux_passthrough) return sequence;
  return "\033Ptmux;\033" + sequence + "\033\\";
}

void SystemClipboard::copy(std::string text) {
  internal_ = std::move(text);
  copyNative(internal_);
  if (internal_.size() > max_osc52_bytes || disabled("TUIIDE_OSC52") || !::isatty(STDOUT_FILENO)) return;
  const auto* term = std::getenv("TERM");
  if (term && std::string_view(term) == "dumb") return;
  const auto sequence = osc52CopySequence(internal_, std::getenv("TMUX") != nullptr);
  (void)writeAll(STDOUT_FILENO, sequence);
}

auto SystemClipboard::paste() -> std::string {
  auto native = pasteNative();
  return native.empty() ? internal_ : std::move(native);
}

auto SystemClipboard::internal() const -> const std::string& { return internal_; }

}  // namespace tuiide
