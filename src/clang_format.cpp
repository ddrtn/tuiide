#include "tuiide/clang_format.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace tuiide {

auto clangFormat(std::string_view source, const std::filesystem::path& filename,
    std::optional<FormatLineRange> lines, std::string executable) -> FormatResult {
  if (executable.empty()) return {false, {}, "clang-format executable is not configured"};
  if (lines && lines->last < lines->first) return {false, {}, "invalid formatting line range"};

  std::vector<std::string> arguments{std::move(executable),
    "--assume-filename=" + (filename.empty() ? std::string("untitled.cpp") : filename.string())};
  if (lines) arguments.push_back("--lines=" + std::to_string(lines->first + 1)
    + ":" + std::to_string(lines->last + 1));
  int input_pipe[2]{};
  int output_pipe[2]{};
  int error_pipe[2]{};
  if (::pipe(input_pipe) != 0) return {false, {}, "cannot create clang-format input pipe"};
  if (::pipe(output_pipe) != 0) {
    ::close(input_pipe[0]); ::close(input_pipe[1]);
    return {false, {}, "cannot create clang-format output pipe"};
  }
  if (::pipe(error_pipe) != 0) {
    ::close(input_pipe[0]); ::close(input_pipe[1]);
    ::close(output_pipe[0]); ::close(output_pipe[1]);
    return {false, {}, "cannot create clang-format error pipe"};
  }
  const auto child = ::fork();
  if (child < 0) {
    ::close(input_pipe[0]); ::close(input_pipe[1]);
    ::close(output_pipe[0]); ::close(output_pipe[1]);
    ::close(error_pipe[0]); ::close(error_pipe[1]);
    return {false, {}, "cannot start clang-format"};
  }
  if (child == 0) {
    (void)::dup2(input_pipe[0], STDIN_FILENO);
    (void)::dup2(output_pipe[1], STDOUT_FILENO);
    (void)::dup2(error_pipe[1], STDERR_FILENO);
    ::close(input_pipe[0]); ::close(input_pipe[1]);
    ::close(output_pipe[0]); ::close(output_pipe[1]);
    ::close(error_pipe[0]); ::close(error_pipe[1]);
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    ::execvp(argv.front(), argv.data());
    _exit(127);
  }
  ::close(input_pipe[0]);
  ::close(output_pipe[1]);
  ::close(error_pipe[1]);
  std::thread writer([fd = input_pipe[1], source] {
    sigset_t signals;
    ::sigemptyset(&signals); ::sigaddset(&signals, SIGPIPE);
    (void)::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::size_t offset{};
    while (offset < source.size()) {
      const auto count = ::write(fd, source.data() + offset, source.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      offset += static_cast<std::size_t>(count);
    }
    ::close(fd);
  });
  std::string error_output;
  std::thread error_reader([fd = error_pipe[0], &error_output] {
    char error_buffer[4096];
    for (;;) {
      const auto count = ::read(fd, error_buffer, sizeof(error_buffer));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      error_output.append(error_buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
  });
  std::string output;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(output_pipe[0], buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(output_pipe[0]);
  writer.join();
  error_reader.join();
  int status{};
  while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return {true, std::move(output), {}};
  if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
    return {false, {}, "clang-format was not found in PATH"};
  const auto code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  while (!error_output.empty() && (error_output.back() == '\n' || error_output.back() == '\r'))
    error_output.pop_back();
  return {false, {}, "clang-format failed with exit code " + std::to_string(code)
    + (error_output.empty() ? std::string{} : ": " + error_output)};
}

}  // namespace tuiide
