#include "tuiide/document.hpp"
#include "tuiide/gdb_client.hpp"
#include "tuiide/lsp_client.hpp"
#include "tuiide/process.hpp"
#include "tuiide/pseudo_terminal.hpp"
#include "tuiide/project_creation.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <pty.h>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

struct TemporaryProject {
  std::filesystem::path path;
  ~TemporaryProject() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

template <typename Pump, typename Predicate>
auto waitFor(Pump pump, Predicate predicate, std::chrono::milliseconds timeout = 10s) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pump();
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  pump();
  return predicate();
}

auto runProcess(std::vector<std::string> arguments, const std::filesystem::path& working_directory,
  std::string& output) -> bool {
  tuiide::AsyncProcess process;
  if (!process.start(arguments, true, working_directory)) return false;
  const auto finished = waitFor([&] {
    for (auto& chunk : process.drain()) output += chunk;
  }, [&] { return !process.running(); }, 20s);
  if (!finished) { process.stop(); return false; }
  const auto code = process.exitCode();
  process.stop();
  for (auto& chunk : process.drain()) output += chunk;
  return code && *code == 0;
}

auto exercisePty(const std::filesystem::path& tuiide, const std::filesystem::path& project,
    bool exercise_resize = false) -> bool {
  int master{-1};
  winsize window{24, 80, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    const auto test_config = project / ".test-config";
    ::setenv("XDG_CONFIG_HOME", test_config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), project.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto flags = ::fcntl(master, F_GETFL, 0);
  if (flags >= 0) (void)::fcntl(master, F_SETFL, flags | O_NONBLOCK);
  std::string screen;
  const auto painted = waitFor([&] {
    char buffer[4096];
    const auto count = ::read(master, buffer, sizeof(buffer));
    if (count > 0) screen.append(buffer, static_cast<std::size_t>(count));
  }, [&] { return screen.size() > 100 && screen.find("Open files") != std::string::npos; }, 5s);
  if (!painted) {
    std::string printable;
    for (const unsigned char value : screen)
      if (value >= 0x20 && value < 0x7f) printable.push_back(static_cast<char>(value));
    std::cerr << "Initial PTY text: " << printable.substr(0, 4000) << '\n';
    ::kill(child, SIGKILL);
    (void)::waitpid(child, nullptr, 0);
    ::close(master);
    return false;
  }
  screen.clear();
  const auto pumpScreen = [&] {
    char buffer[4096];
    const auto count = ::read(master, buffer, sizeof(buffer));
    if (count > 0) screen.append(buffer, static_cast<std::size_t>(count));
  };
  constexpr std::string_view f10{"\033[21~"};
  (void)::write(master, f10.data(), f10.size());
  const auto menu_focused = waitFor(pumpScreen, [&] { return screen.find("File") != std::string::npos; }, 5s);
  screen.clear();
  const char enter = '\r';
  (void)::write(master, &enter, 1);
  const auto menu_opened = waitFor(pumpScreen, [&] { return screen.find("pen...") != std::string::npos; }, 5s);
  const char escape = 27;
  screen.clear();
  (void)::write(master, &escape, 1);
  const auto menu_closed = waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 5s);
  std::this_thread::sleep_for(150ms);
  pumpScreen();
  constexpr std::string_view command_palette_key{"\033k"};
  screen.clear();
  (void)::write(master, command_palette_key.data(), command_palette_key.size());
  const auto command_palette_opened = waitFor(pumpScreen, [&] {
    return screen.find("Command palette") != std::string::npos
      && screen.find("File: New") != std::string::npos
      && screen.find("Search: Find") != std::string::npos;
  }, 5s);
  constexpr std::string_view palette_filter{"build"};
  screen.clear();
  (void)::write(master, palette_filter.data(), palette_filter.size());
  const auto command_palette_filtered = waitFor(pumpScreen, [&] {
    return screen.find("Run: Build") != std::string::npos
      && screen.find("File: New") == std::string::npos;
  }, 5s);
  screen.clear();
  (void)::write(master, &escape, 1);
  (void)waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 5s);
  std::this_thread::sleep_for(150ms);
  pumpScreen();
  screen.clear();
  const char open = 15;
  (void)::write(master, &open, 1);
  const auto files_visible = waitFor(pumpScreen, [&] { return screen.find("main.cpp") != std::string::npos; }, 5s);
  std::this_thread::sleep_for(150ms);
  const std::string open_main{"\177main.cpp\r"};
  screen.clear();
  (void)::write(master, open_main.data(), open_main.size());
  const auto file_opened = waitFor(pumpScreen, [&] {
    return screen.find("1 file(s)") != std::string::npos
      && screen.find("界") != std::string::npos && screen.find("🙂") != std::string::npos;
  }, 5s);
  screen.clear();
  constexpr std::string_view editor_end{"\033[F"};
  (void)::write(master, editor_end.data(), editor_end.size());
  const auto unicode_horizontal_scroll = waitFor(pumpScreen, [&] {
    return screen.find("combining-e") != std::string::npos && screen.find("尾") != std::string::npos;
  }, 5s);
  constexpr std::string_view editor_home{"\033[H"};
  (void)::write(master, editor_home.data(), editor_home.size());
  screen.clear();
  (void)::write(master, &open, 1);
  const auto files_visible_again = waitFor(pumpScreen, [&] { return screen.find("main.cpp") != std::string::npos; }, 5s);
  std::this_thread::sleep_for(150ms);
  screen.clear();
  (void)::write(master, open_main.data(), open_main.size());
  const auto second_open_completed = waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 5s);
  const char close_file = 23;
  screen.clear();
  (void)::write(master, &close_file, 1);
  const auto file_deduplicated = waitFor(pumpScreen, [&] {
    return screen.find("No file") != std::string::npos || screen.find("0 file(s)") != std::string::npos;
  }, 5s);
  constexpr std::string_view alt_file_close{"\033f"};
  screen.clear();
  (void)::write(master, alt_file_close.data(), alt_file_close.size());
  const auto reopen_menu_visible = waitFor(pumpScreen, [&] {
    return screen.find("Reopen Close") != std::string::npos;
  }, 5s);
  const char reopen_closed = 'd';
  screen.clear();
  (void)::write(master, &reopen_closed, 1);
  const auto file_reopened_from_history = waitFor(pumpScreen, [&] {
    return screen.find("Reopened:") != std::string::npos && screen.find("1 file(s)") != std::string::npos;
  }, 5s);
  screen.clear();
  (void)::write(master, &close_file, 1);
  const auto history_file_closed = waitFor(pumpScreen, [&] {
    return screen.find("No file") != std::string::npos || screen.find("0 file(s)") != std::string::npos;
  }, 5s);

  // Open the same fixture through the Project FListView tree. This exercises
  // the item-to-path mapping rather than the independent Ctrl+O file dialog.
  screen.clear();
  const char focus_project = 5;  // Ctrl+E
  (void)::write(master, &focus_project, 1);
  const auto project_tree_visible = waitFor(pumpScreen, [&] {
    return screen.find("main.cpp") != std::string::npos;
  }, 5s);
  constexpr std::string_view context_menu_key{"\033[29~"};
  screen.clear();
  (void)::write(master, context_menu_key.data(), context_menu_key.size());
  const auto project_context_visible = waitFor(pumpScreen, [&] {
    return screen.find("New from") != std::string::npos
      && screen.find("Rename / move") != std::string::npos
      && screen.find("Remove / delete") != std::string::npos;
  }, 5s);
  screen.clear();
  (void)::write(master, &escape, 1);
  (void)waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 5s);
  std::this_thread::sleep_for(150ms);
  pumpScreen();
  constexpr std::string_view end_key{"\033[F"};
  (void)::write(master, end_key.data(), end_key.size());
  (void)::write(master, &enter, 1);
  const auto project_file_opened = waitFor(pumpScreen, [&] { return screen.find("1 file(s)") != std::string::npos; }, 5s);
  const char new_file = 14;  // Ctrl+N
  screen.clear();
  (void)::write(master, &close_file, 1);
  const auto project_file_closed = waitFor(pumpScreen, [&] {
    return screen.find("No file") != std::string::npos || screen.find("0 file(s)") != std::string::npos;
  }, 5s);

  screen.clear();
  (void)::write(master, &focus_project, 1);
  (void)waitFor(pumpScreen, [&] { return screen.find("main.cpp") != std::string::npos; }, 5s);
  constexpr std::string_view insert_key{"\033[2~"};
  screen.clear();
  (void)::write(master, insert_key.data(), insert_key.size());
  const auto template_picker_opened = waitFor(pumpScreen, [&] {
    return screen.find("C header") != std::string::npos && screen.find("C++ class") != std::string::npos;
  }, 5s);
  screen.clear();
  (void)::write(master, &enter, 1);
  const auto path_dialog_opened = waitFor(pumpScreen, [&] {
    return screen.find("new_header.h") != std::string::npos;
  }, 5s);
  screen.clear();
  (void)::write(master, &escape, 1);
  const auto path_dialog_closed = waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 5s);

  screen.clear();
  (void)::write(master, &open, 1);
  const auto files_visible_for_reopen = waitFor(pumpScreen, [&] { return screen.find("main.cpp") != std::string::npos; }, 5s);
  std::this_thread::sleep_for(150ms);
  screen.clear();
  (void)::write(master, open_main.data(), open_main.size());
  const auto file_reopened = waitFor(pumpScreen, [&] { return screen.find("1 file(s)") != std::string::npos; }, 5s);
  const char find = 6;  // Ctrl+F
  screen.clear();
  (void)::write(master, &find, 1);
  const auto search_dialog_opened = waitFor(pumpScreen, [&] {
    return screen.find("Find and replace") != std::string::npos && screen.find("Whole word") != std::string::npos;
  }, 5s);
  if (exercise_resize) {
    winsize compact_window{18, 60, 0, 0};
    screen.clear();
    const auto compact_resize_sent = ::ioctl(master, TIOCSWINSZ, &compact_window) == 0;
    const auto search_dialog_survived_resize = waitFor(pumpScreen, [&] { return screen.size() > 20; }, 2s);
    const std::string search_compute{"compute\r"};
    screen.clear();
    (void)::write(master, search_compute.data(), search_compute.size());
    const auto search_match_selected = waitFor(pumpScreen, [&] {
      return screen.find("main.cpp") != std::string::npos
        && screen.find("compute") != std::string::npos;
    }, 5s);
    winsize normal_window{24, 80, 0, 0};
    screen.clear();
    (void)::ioctl(master, TIOCSWINSZ, &normal_window);
    (void)waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 2s);
    const char quit_after_resize = 4;
    (void)::write(master, &quit_after_resize, 1);
    int resize_status{};
    const auto resize_exited = waitFor(pumpScreen, [&] {
      return ::waitpid(child, &resize_status, WNOHANG) == child;
    }, 5s);
    if (!resize_exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &resize_status, 0); }
    ::close(master);
    return search_dialog_opened && compact_resize_sent && search_dialog_survived_resize
      && search_match_selected && resize_exited && WIFEXITED(resize_status) && WEXITSTATUS(resize_status) == 0;
  }
  const std::string search_compute{"compute\r"};
  screen.clear();
  (void)::write(master, search_compute.data(), search_compute.size());
  const auto search_match_selected = waitFor(pumpScreen, [&] {
    return screen.find("Find: match 1 of") != std::string::npos;
  }, 5s);
  constexpr std::string_view right_key{"\033[C"};
  (void)::write(master, right_key.data(), right_key.size());
  const char edit = ' ';
  (void)::write(master, &edit, 1);
  std::this_thread::sleep_for(150ms);
  pumpScreen();
  screen.clear();
  constexpr std::string_view alt_file{"\033f"};
  (void)::write(master, alt_file.data(), alt_file.size());
  const auto save_all_visible = waitFor(pumpScreen, [&] { return screen.find("Save A") != std::string::npos; }, 5s);
  const char save_all = 'l';
  screen.clear();
  (void)::write(master, &save_all, 1);
  const auto all_saved = waitFor(pumpScreen, [&] { return screen.find("Save All: saved 1 document") != std::string::npos; }, 5s);
  constexpr std::string_view f7{"\033[18~"};
  screen.clear();
  (void)::write(master, f7.data(), f7.size());
  const auto build_completed = waitFor(pumpScreen, [&] {
    return screen.find("2 parallel jobs") != std::string::npos
      && screen.find("Built target smoke_app") != std::string::npos;
  }, 45s);
  // Give the asynchronous process waiter one UI tick after the final CMake
  // output before issuing Run; the build itself is already complete here.
  const auto build_finalized = waitFor(pumpScreen, [&] {
    return screen.find("Build finished with exit code 0") != std::string::npos;
  }, 10s);
  const auto build_screen = screen;
  constexpr std::string_view f6{"\033[17~"};
  screen.clear();
  (void)::write(master, f6.data(), f6.size());
  const auto run_completed = waitFor(pumpScreen, [&] {
    return screen.find("Run finished with exit code 0") != std::string::npos;
  }, 15s);
  const auto run_screen = screen;
  screen.clear();
  (void)::write(master, &new_file, 1);
  const auto close_all_ready = waitFor(pumpScreen, [&] { return screen.find("2 file(s)") != std::string::npos; }, 5s);
  screen.clear();
  constexpr std::string_view close_all_key{"\033W"};
  (void)::write(master, close_all_key.data(), close_all_key.size());
  const auto all_documents_closed = waitFor(pumpScreen, [&] {
    return screen.find("Close All: closed 2 document") != std::string::npos
      && (screen.find("No file") != std::string::npos || screen.find("0 file(s)") != std::string::npos);
  }, 5s);
  screen.clear();
  (void)::write(master, alt_file.data(), alt_file.size());
  (void)waitFor(pumpScreen, [&] { return screen.find("Close Pro") != std::string::npos; }, 5s);
  const char close_project = 'j';
  screen.clear();
  (void)::write(master, &close_project, 1);
  const auto project_closed = waitFor(pumpScreen, [&] { return screen.find("No project") != std::string::npos; }, 5s);
  const char save_without_document = 19;  // Ctrl+S
  screen.clear();
  (void)::write(master, &save_without_document, 1);
  const auto unavailable_reported = waitFor(pumpScreen, [&] {
    return screen.find("Save unavailable") != std::string::npos;
  }, 5s);
  const auto unavailableCommand = [&](std::string_view key, std::string_view message) {
    screen.clear();
    (void)::write(master, key.data(), key.size());
    return waitFor(pumpScreen, [&] { return screen.find(message) != std::string::npos; }, 3s);
  };
  const std::string close_key(1, static_cast<char>(23));
  const std::string find_key(1, static_cast<char>(6));
  const bool close_unavailable = unavailableCommand(close_key, "Close unavailable");
  const bool find_unavailable = unavailableCommand(find_key, "Find unavailable");
  const bool lsp_unavailable = unavailableCommand("\033OP", "Symbol information unavailable");
  const bool debug_unavailable = unavailableCommand("\033[15~", "Debug unavailable");
  const bool run_unavailable = unavailableCommand("\033[17~", "Run unavailable");
  const bool build_unavailable = unavailableCommand("\033[18~", "Build unavailable");
  const bool breakpoint_unavailable = unavailableCommand("\033[20~", "Breakpoint unavailable");
  const bool step_unavailable = unavailableCommand("\033[23~", "Step into unavailable");
  const bool preset_unavailable = unavailableCommand("\033p", "Configure preset unavailable");
  const bool watch_unavailable = unavailableCommand("\033w", "Add watch unavailable");
  const bool registers_unavailable = unavailableCommand("\033r", "Registers unavailable");
  const char quit = 4;
  screen.clear();
  (void)::write(master, &quit, 1);
  int status{};
  const auto exited = waitFor(pumpScreen, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 5s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);
  const auto success = painted && files_visible && file_opened && unicode_horizontal_scroll
    && files_visible_again && second_open_completed
    && file_deduplicated && reopen_menu_visible && file_reopened_from_history && history_file_closed
    && file_reopened
    && project_tree_visible && project_context_visible && project_file_opened && project_file_closed
    && template_picker_opened && path_dialog_opened
    && search_dialog_opened
    && save_all_visible && all_saved && build_completed && run_completed
    && close_all_ready
    && unavailable_reported
    && menu_focused && menu_opened && menu_closed && command_palette_opened && command_palette_filtered;
  const auto unavailable_complete = close_unavailable && find_unavailable && lsp_unavailable
    && debug_unavailable && run_unavailable && build_unavailable && breakpoint_unavailable
    && step_unavailable && preset_unavailable && watch_unavailable && registers_unavailable;
  if (!success || !unavailable_complete)
    std::cerr << "PTY state: painted=" << painted << " files=" << files_visible << " opened=" << file_opened
              << " unicode_scroll=" << unicode_horizontal_scroll
              << " files2=" << files_visible_again << " second=" << second_open_completed
              << " deduplicated=" << file_deduplicated << " files3=" << files_visible_for_reopen
              << " reopen_menu=" << reopen_menu_visible << " history_reopen=" << file_reopened_from_history
              << " history_closed=" << history_file_closed
              << " reopened=" << file_reopened
              << " project_tree=" << project_tree_visible
              << " project_context=" << project_context_visible
              << " project_opened=" << project_file_opened << " project_closed=" << project_file_closed
              << " template_picker=" << template_picker_opened << " path_dialog=" << path_dialog_opened
              << " path_closed=" << path_dialog_closed
              << " search_dialog=" << search_dialog_opened << " search_match=" << search_match_selected
              << " save_all_visible=" << save_all_visible << " all_saved=" << all_saved
              << " build_completed=" << build_completed << " build_finalized=" << build_finalized
              << " run_completed=" << run_completed
              << " close_all_ready=" << close_all_ready << " all_closed=" << all_documents_closed
              << " project_unloaded=" << project_closed
              << " unavailable=" << unavailable_reported
              << " focused=" << menu_focused << " menu=" << menu_opened << " closed=" << menu_closed
              << " palette=" << command_palette_opened
              << " palette_filter=" << command_palette_filtered
              << " unavailable_commands=" << unavailable_complete
              << " exited=" << exited << " unsaved=" << (screen.find("Unsaved") != std::string::npos)
              << " status=" << status << '\n';
  if (!build_completed) {
    const auto begin = build_screen.size() > 4000 ? build_screen.size() - 4000 : 0;
    std::cerr << "Build screen tail:\n" << build_screen.substr(begin) << '\n';
  }
  if (!run_completed) {
    std::string printable;
    for (const unsigned char value : run_screen)
      if (value == '\n' || (value >= 0x20 && value < 0x7f)) printable.push_back(static_cast<char>(value));
    const auto begin = printable.size() > 4000 ? printable.size() - 4000 : 0;
    std::cerr << "Run screen tail:\n" << printable.substr(begin) << '\n';
  }
  return success && unavailable_complete;
}

auto exerciseEmptyStartup(const std::filesystem::path& tuiide, const std::filesystem::path& config) -> bool {
  int master{-1};
  winsize window{24, 80, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  const auto flags = ::fcntl(master, F_GETFL, 0);
  if (flags >= 0) (void)::fcntl(master, F_SETFL, flags | O_NONBLOCK);
  std::string screen;
  const auto pump = [&] {
    char buffer[4096];
    const auto count = ::read(master, buffer, sizeof(buffer));
    if (count > 0) screen.append(buffer, static_cast<std::size_t>(count));
  };
  const auto ready = waitFor(pump, [&] {
    return screen.find("No project") != std::string::npos
      && screen.find("Welcome to TUI IDE") != std::string::npos
      && screen.find("clangd: off") != std::string::npos;
  }, 5s);
  const char quit = 4;
  (void)::write(master, &quit, 1);
  int status{};
  const auto exited = waitFor(pump, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 5s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);
  return ready && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
}  // namespace

int main(int argc, char** argv) {
  expect(argc == 2, "path to tuiide executable is provided");
  const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  TemporaryProject project{std::filesystem::temp_directory_path() / ("tuiide-integration-" + unique)};
  const auto build = project.path / "build";
  std::filesystem::create_directories(project.path);
  {
    std::ofstream file(project.path / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(smoke LANGUAGES CXX)\n"
            "set(CMAKE_CXX_STANDARD 20)\nadd_executable(smoke_app main.cpp pair.cpp)\n";
  }
  {
    std::ofstream file(project.path / "main.cpp");
    file << "#include <string> // UTF-8:\t界🙂e\u0301 ................................ combining-e 尾\n\n"
            "int compute(int value) {\n  int local = value + 1;\n  return local;\n}\n\n"
            "int main() {\n  int result = compute(41); struct Pair { int left; int right; } pair{result, 7};\n"
            "  return pair.left == 42 ? 0 : 1;\n}\n";
  }
  { std::ofstream file(project.path / "pair.hpp"); file << "#pragma once\nint pair_value();\n"; }
  { std::ofstream file(project.path / "types.hpp"); file << "#pragma once\nclass Base {};\nclass Derived : public Base {};\n"; }
  { std::ofstream file(project.path / "pair.cpp"); file << "#include \"pair.hpp\"\n#include \"types.hpp\"\nint pair_value() { Derived value; return sizeof(value) + 6; }\n"; }
  {
    std::ofstream file(project.path / ".tuiide-project.json");
    file << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":2}\n";
  }

  std::string process_output;
  expect(runProcess({"cmake", "-S", project.path.string(), "-B", build.string(),
    "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"}, project.path, process_output),
    "fixture configures with CMake");
  expect(runProcess({"cmake", "--build", build.string(), "--parallel", "1"}, project.path, process_output),
    "fixture builds in one thread");
  std::filesystem::copy_file(build / "compile_commands.json", project.path / "compile_commands.json",
    std::filesystem::copy_options::overwrite_existing);

  if (std::getenv("TUIIDE_PTY_ONLY") != nullptr) {
    expect(exercisePty(std::filesystem::absolute(argv[1]), project.path, true),
      "Final Cut TUI survives its pseudo-terminal interaction and resize scenario");
    std::cout << "PTY integration test passed\n";
    return 0;
  }

  tuiide::NewProjectOptions generated_options;
  generated_options.name = "generated_core";
  generated_options.project_directory = project.path / "generated";
  generated_options.build_directory = project.path / "generated-build";
  generated_options.target_type = tuiide::ProjectTargetType::StaticLibrary;
  generated_options.build_type = "Release";
  generated_options.install_layout = tuiide::ProjectInstallLayout::Gnu;
  std::string generated_error;
  expect(tuiide::createNewProject(generated_options, generated_error),
    "project wizard fixture is generated");
  expect(runProcess({"cmake", "-S", generated_options.project_directory.string(), "-B",
      generated_options.build_directory.string(), "-DCMAKE_BUILD_TYPE=Release"}, project.path, process_output),
    "generated project passes immediate CMake validation");
  expect(runProcess({"cmake", "--build", generated_options.build_directory.string(), "--parallel", "1"},
      project.path, process_output),
    "generated project builds in one thread");
  std::error_code generated_cleanup_error;
  std::filesystem::remove_all(generated_options.project_directory, generated_cleanup_error);
  std::filesystem::remove_all(generated_options.build_directory, generated_cleanup_error);

  tuiide::Document document;
  std::string error;
  expect(document.load(project.path / "main.cpp", error), "fixture document loads");
  tuiide::LspClient lsp;
  expect(lsp.start(project.path, {}, {}, build), "clangd starts with the selected CMake compilation database");
  lsp.open(document);
  lsp.setActiveDocument(&document);
  expect(waitFor([&] { lsp.poll(); }, [&] { return lsp.ready(); }), "clangd initializes");
  expect(waitFor([&] { lsp.poll(); }, [&] { return !lsp.semanticTokens().empty(); }), "clangd returns semantic tokens");
  document.setCursor({8, 17});
  lsp.requestDefinition(document);
  std::vector<tuiide::SourceLocation> definitions;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    definitions = lsp.takeDefinitions();
    return !definitions.empty();
  }), "clangd resolves a definition");
  document.setText("#include <string>\nint main() { std::string value; value.si; }\n");
  document.setCursor({1, document.line(1).find("value.si") + 8});
  lsp.change(document);
  lsp.requestCompletion(document);
  std::vector<tuiide::LspCompletionItem> completions;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    completions = lsp.takeCompletions();
    return !completions.empty();
  }), "clangd returns completions");
  expect(std::any_of(completions.begin(), completions.end(), [](const auto& item) {
    return !item.label.empty() && !item.insertion.empty() && item.kind > 0 && item.source_version > 0;
  }), "clangd completion retains kind, insertion text, and source revision");
  document.setText("#include <string>\nint main() { std::string value; value.si; }\n");
  document.setCursor({1, document.line(1).find("value.si") + 8});
  lsp.change(document);
  lsp.requestCompletion(document);
  document.setText("#include <string>\nint main() { std::string other; other.si; }\n");
  document.setCursor({1, document.line(1).find("other.si") + 8});
  lsp.change(document);
  lsp.requestCompletion(document);
  completions.clear();
  expect(waitFor([&] { lsp.poll(); }, [&] {
    completions = lsp.takeCompletions(); return !completions.empty();
  }), "new completion supersedes an in-flight request after a rapid document change");
  expect(std::all_of(completions.begin(), completions.end(), [&document](const auto& item) {
    return item.source_path == document.path() && item.source_version == document.version();
  }), "only completion results for the current document revision are published");
  document.setText("int compute(int value) { return value + 1; }\nint main() { return compute(41); }\n");
  const auto call = document.line(1).find("compute(");
  document.setCursor({1, call + 8});
  lsp.change(document);
  lsp.requestSignatureHelp(document);
  std::vector<tuiide::LspSignature> signatures;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    signatures = lsp.takeSignatures();
    return !signatures.empty();
  }), "clangd returns signature help");
  expect(std::any_of(signatures.begin(), signatures.end(), [](const auto& signature) {
    return signature.label.find("compute") != std::string::npos && !signature.parameters.empty();
  }), "signature help retains function label and parameters");
  lsp.requestDocumentSymbols(document);
  std::optional<tuiide::LspDocumentSymbols> document_symbols;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    document_symbols = lsp.takeDocumentSymbols();
    return document_symbols.has_value();
  }), "clangd returns document symbols for the outline");
  expect(document_symbols->path == document.path() && document_symbols->version == document.version()
      && std::any_of(document_symbols->symbols.begin(), document_symbols->symbols.end(), [](const auto& symbol) {
        return symbol.name == "compute" && symbol.kind == 12;
      }), "documentSymbol response retains source revision and function positions");
  lsp.requestWorkspaceSymbols("compute");
  std::vector<tuiide::LspNavigationItem> workspace_symbols;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    workspace_symbols = lsp.takeWorkspaceSymbols(); return !workspace_symbols.empty();
  }), "clangd searches symbols across the workspace index");
  expect(std::any_of(workspace_symbols.begin(), workspace_symbols.end(), [](const auto& symbol) {
    return symbol.name == "compute" && !symbol.path.empty();
  }), "workspace symbol results retain names and source locations");

  document.setCursor({0, 5});
  lsp.requestCallHierarchy(document);
  std::optional<tuiide::LspHierarchy> call_hierarchy;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    call_hierarchy = lsp.takeCallHierarchy(); return call_hierarchy.has_value();
  }), "clangd completes incoming and outgoing call hierarchy requests");
  expect(call_hierarchy->root.find("compute") != std::string::npos
      && std::any_of(call_hierarchy->items.begin(), call_hierarchy->items.end(), [](const auto& item) {
        return item.relation == "incoming" && item.name == "main";
      }), "call hierarchy identifies the caller and its navigation location");

  document.setCursor({0, 5});
  lsp.requestRename(document, "calculate");
  std::optional<tuiide::WorkspaceEdit> rename_edit;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    rename_edit = lsp.takeRenameEdit();
    return rename_edit.has_value();
  }), "clangd returns a workspace edit for rename preview");
  expect(!rename_edit->files.empty() && std::any_of(rename_edit->files.begin(), rename_edit->files.end(), [](const auto& file) {
    return std::any_of(file.edits.begin(), file.edits.end(), [](const auto& edit) { return edit.text == "calculate"; });
  }), "rename workspace edit retains the replacement text");
  document.setText("int main() { return 0; }\n");
  document.setCursor({0, 0});
  lsp.change(document);
  lsp.requestDefinition(document);
  std::vector<tuiide::LspFeedback> lsp_feedback;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    lsp_feedback = lsp.takeFeedback();
    return std::any_of(lsp_feedback.begin(), lsp_feedback.end(), [](const auto& item) {
      return item.operation == tuiide::LspOperation::Definition && !item.error;
    });
  }), "clangd reports an explicit empty definition result");
  document.setText("int main() { return missing_symbol; }\n");
  lsp.change(document);
  expect(waitFor([&] { lsp.poll(); }, [&] { return !lsp.diagnostics().empty(); }), "clangd publishes diagnostics");
  lsp.requestCodeActions(document, {0, 0}, {0, document.line(0).size()});
  std::vector<tuiide::LspCodeAction> code_actions;
  std::vector<tuiide::LspFeedback> code_action_feedback;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    code_actions = lsp.takeCodeActions();
    auto feedback = lsp.takeFeedback();
    code_action_feedback.insert(code_action_feedback.end(), feedback.begin(), feedback.end());
    return !code_actions.empty() || std::any_of(code_action_feedback.begin(), code_action_feedback.end(), [](const auto& item) {
      return item.operation == tuiide::LspOperation::CodeActions;
    });
  }), "clangd completes a code action request with edits or an explicit empty result");
  if (!code_actions.empty())
    expect(!code_actions.front().title.empty() && !code_actions.front().edit.files.empty()
        && code_actions.front().source_path == document.path(),
      "clangd code action retains its title, workspace edit, and source revision");

  tuiide::Document pair_document;
  expect(pair_document.load(project.path / "pair.cpp", error), "header/source fixture loads");
  lsp.open(pair_document);
  lsp.setActiveDocument(&pair_document);
  lsp.requestSwitchSourceHeader(pair_document);
  std::optional<std::filesystem::path> switched_header;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    switched_header = lsp.takeSwitchedSourceHeader(); return switched_header.has_value();
  }), "clangd switches from a source file to its matching header");
  expect(std::filesystem::equivalent(*switched_header, project.path / "pair.hpp"),
    "header/source switch returns the matching project path");
  lsp.close(pair_document);
  lsp.setActiveDocument(&document);

  tuiide::Document type_document;
  expect(type_document.load(project.path / "types.hpp", error), "type hierarchy fixture loads");
  lsp.open(type_document);
  lsp.setActiveDocument(&type_document);
  type_document.setCursor({2, type_document.line(2).find("Derived") + 2});
  lsp.requestTypeHierarchy(type_document);
  std::optional<tuiide::LspHierarchy> type_hierarchy;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    type_hierarchy = lsp.takeTypeHierarchy(); return type_hierarchy.has_value();
  }), "clangd completes supertype and subtype hierarchy requests");
  expect(std::any_of(type_hierarchy->items.begin(), type_hierarchy->items.end(), [](const auto& item) {
    return item.relation == "supertype" && item.name == "Base";
  }), "type hierarchy identifies the direct base type");
  lsp.close(type_document);
  const auto lsp_stop_started = std::chrono::steady_clock::now();
  lsp.stop();
  expect(std::chrono::steady_clock::now() - lsp_stop_started < 2s,
    "clangd stops promptly while a document is still open");

  const auto fake_bin = project.path / "fake-bin";
  std::filesystem::create_directories(fake_bin);
  {
    std::ofstream fake_clangd(fake_bin / "clangd");
    fake_clangd << "#!/bin/sh\nexit 37\n";
  }
  std::filesystem::permissions(fake_bin / "clangd",
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write
      | std::filesystem::perms::owner_exec);
  const auto* current_path = ::getenv("PATH");
  const std::string saved_path = current_path ? current_path : "";
  ::setenv("PATH", fake_bin.c_str(), 1);
  tuiide::LspClient failed_lsp;
  expect(failed_lsp.start(project.path), "failing clangd process starts before reporting its exit");
  std::vector<tuiide::LspFeedback> failed_feedback;
  expect(waitFor([&] { failed_lsp.poll(); }, [&] {
    auto feedback = failed_lsp.takeFeedback();
    failed_feedback.insert(failed_feedback.end(), std::make_move_iterator(feedback.begin()),
      std::make_move_iterator(feedback.end()));
    return std::any_of(failed_feedback.begin(), failed_feedback.end(), [](const auto& item) {
      return item.operation == tuiide::LspOperation::Server && item.error
        && item.message.find("code 37") != std::string::npos;
    });
  }), "clangd process failure produces explicit server feedback");
  failed_lsp.stop();
  ::setenv("PATH", saved_path.c_str(), 1);

  tuiide::GdbClient gdb;
  expect(gdb.addBreakpoint(project.path / "main.cpp", 10), "GDB breakpoint is queued");
  expect(gdb.addWatch("result"), "GDB watch is queued");
  gdb.setRegistersEnabled(true);
  expect(gdb.start(build / "smoke_app"), "GDB starts");
  gdb.run();
  std::vector<std::string> gdb_log;
  bool ptrace_denied{};
  bool gdb_ready{};
  const auto gdb_finished = waitFor([&] {
    gdb.poll();
    auto lines = gdb.takeOutput();
    for (const auto& line : lines)
      if (line.find("ptrace: Operation not permitted") != std::string::npos) ptrace_denied = true;
    gdb_log.insert(gdb_log.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
  }, [&] {
    const auto watch_ready = !gdb.watches().empty() && gdb.watches()[0].error.empty();
    const auto pair_ready = std::any_of(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
      return variable.name == "pair" && variable.expandable && !variable.object.empty();
    });
    gdb_ready = gdb.stopped() && !gdb.frames().empty() && !gdb.variables().empty()
      && pair_ready && watch_ready && !gdb.registers().empty();
    return gdb_ready || ptrace_denied;
  });
  if (!gdb_finished || (!gdb_ready && !ptrace_denied)) {
    std::cerr << "GDB state: stopped=" << gdb.stopped() << " frames=" << gdb.frames().size()
              << " variables=" << gdb.variables().size() << " registers=" << gdb.registers().size();
    if (!gdb.watches().empty())
      std::cerr << " watch-value='" << gdb.watches()[0].value << "' watch-error='" << gdb.watches()[0].error << "'";
    std::cerr << '\n';
    for (const auto& line : gdb_log) std::cerr << "MI: " << line << '\n';
  }
  expect(gdb_finished && (gdb_ready || ptrace_denied), "GDB reaches a result or reports a ptrace restriction");
  if (ptrace_denied) std::cout << "GDB execution skipped: ptrace is restricted\n";
  else {
    expect(gdb.breakpoints().size() == 1 && gdb.breakpoints()[0].verified
        && gdb.breakpoints()[0].error.empty(),
      "GDB marks a resolved breakpoint as verified");
    expect(gdb.active() && gdb.stopped(), "GDB distinguishes an active stopped inferior from its own process");
    expect(std::any_of(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
      return variable.name == "result";
    }), "GDB exposes the local variable");
    expect(gdb.watches()[0].value.find("42") != std::string::npos, "GDB evaluates the watch expression");
    expect(gdb.selectFrame(0) && gdb.selectedFrame() == 0,
      "GDB explicitly selects a stack frame before refreshing its context");
    expect(waitFor([&] { gdb.poll(); }, [&] {
      return std::any_of(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
        return variable.name == "pair" && variable.expandable && !variable.object.empty();
      }) && !gdb.watches().empty() && gdb.watches()[0].error.empty();
    }), "selecting a stack frame refreshes locals and watches");
    const auto pair = std::find_if(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
      return variable.name == "pair";
    });
    const auto pair_index = static_cast<std::size_t>(std::distance(gdb.variables().begin(), pair));
    expect(pair != gdb.variables().end() && gdb.toggleVariable(pair_index),
      "compound local variable requests its children");
    expect(waitFor([&] { gdb.poll(); }, [&] {
      return std::any_of(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
        return variable.depth == 1 && variable.name == "left" && variable.value.find("42") != std::string::npos;
      });
    }), "GDB variable tree exposes a structured child value");
    expect(gdb.toggleVariable(pair_index)
        && std::none_of(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
          return variable.depth > 0;
        }), "collapsing a variable removes its visible descendants");
    expect(gdb.assign("result", "100"), "GDB accepts a variable assignment while stopped");
    std::vector<tuiide::DebugResult> debug_results;
    expect(waitFor([&] {
      gdb.poll();
      auto results = gdb.takeResults();
      debug_results.insert(debug_results.end(), std::make_move_iterator(results.begin()),
        std::make_move_iterator(results.end()));
    }, [&] {
      return std::any_of(debug_results.begin(), debug_results.end(), [](const auto& result) {
        return result.kind == tuiide::DebugResultKind::Assignment && result.expression == "result"
          && result.error.empty() && result.value.find("100") != std::string::npos;
      });
    }), "GDB changes a local variable and returns the assigned value");
    expect(waitFor([&] { gdb.poll(); }, [&] {
      return !gdb.watches().empty() && gdb.watches()[0].error.empty()
        && gdb.watches()[0].value.find("100") != std::string::npos;
    }), "variable assignment refreshes watches in the selected frame");
    expect(gdb.evaluate("result + pair.right"), "GDB accepts an arbitrary expression while stopped");
    expect(waitFor([&] {
      gdb.poll();
      auto results = gdb.takeResults();
      debug_results.insert(debug_results.end(), std::make_move_iterator(results.begin()),
        std::make_move_iterator(results.end()));
    }, [&] {
      return std::any_of(debug_results.begin(), debug_results.end(), [](const auto& result) {
        return result.kind == tuiide::DebugResultKind::Evaluation && result.error.empty()
          && result.expression == "result + pair.right" && result.value.find("107") != std::string::npos;
      });
    }), "GDB evaluates an expression in the selected stack frame");
    expect(gdb.disassemble("$pc", 96), "GDB accepts a bounded disassembly request");
    expect(waitFor([&] {
      gdb.poll();
      auto results = gdb.takeResults();
      debug_results.insert(debug_results.end(), std::make_move_iterator(results.begin()),
        std::make_move_iterator(results.end()));
    }, [&] {
      return std::any_of(debug_results.begin(), debug_results.end(), [](const auto& result) {
        return result.kind == tuiide::DebugResultKind::Disassembly && result.error.empty()
          && result.value.find("0x") != std::string::npos && result.value.find("main") != std::string::npos;
      });
    }), "GDB returns formatted disassembly around the program counter");
    expect(gdb.readMemory("$sp", 32), "GDB accepts a bounded memory request");
    expect(waitFor([&] {
      gdb.poll();
      auto results = gdb.takeResults();
      debug_results.insert(debug_results.end(), std::make_move_iterator(results.begin()),
        std::make_move_iterator(results.end()));
    }, [&] {
      return std::any_of(debug_results.begin(), debug_results.end(), [](const auto& result) {
        return result.kind == tuiide::DebugResultKind::Memory && result.error.empty()
          && result.value.find("0x") != std::string::npos && result.value.find('|') != std::string::npos;
      });
    }), "GDB returns a formatted hex and ASCII memory dump");
    expect(!gdb.readMemory("$sp", 4097) && !gdb.disassemble("$pc", 4097),
      "debug memory operations reject oversized requests");
    expect(!gdb.toggleBreakpoint(project.path / "main.cpp", 10), "GDB breakpoint is removed before continuing to exit");
    gdb.continueExecution();
    expect(waitFor([&] { gdb.poll(); }, [&] { return gdb.running() && gdb.exited() && !gdb.active(); }),
      "GDB reports a completed inferior separately from the live debugger process");
    expect(!gdb.stopped() && gdb.frames().empty() && gdb.variables().empty() && gdb.threads().empty(),
      "inferior exit clears stale stack, locals, and threads");
    gdb.stop();
    expect(!gdb.running() && !gdb.active() && !gdb.exited(), "Debug Stop clears the process lifecycle state");
    expect(gdb.addBreakpoint(project.path / "main.cpp", 10), "logpoint is queued for the restarted session");
    auto logpoint = gdb.breakpoints().front();
    logpoint.log_message = "result logpoint";
    expect(gdb.updateBreakpoint(logpoint), "logpoint properties are accepted");
    tuiide::PseudoTerminal debug_terminal;
    expect(debug_terminal.openSession(60, 12), "debug console PTY opens");
    expect(gdb.start(build / "smoke_app", {}, {}, {}, {}, debug_terminal.slaveName()),
      "GDB can start again with a dedicated inferior TTY after Debug Stop");
    debug_terminal.activateSession();
    gdb.run();
    std::vector<std::string> logpoint_output;
    const auto logpoint_finished = waitFor([&] {
      gdb.poll();
      auto lines = gdb.takeOutput();
      logpoint_output.insert(logpoint_output.end(), std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
    }, [&] { return gdb.exited() && !gdb.active(); });
    if (!logpoint_finished) {
      std::cerr << "Logpoint state: running=" << gdb.running() << " active=" << gdb.active()
                << " stopped=" << gdb.stopped() << " exited=" << gdb.exited() << '\n';
      for (const auto& line : logpoint_output) std::cerr << "Logpoint MI: " << line << '\n';
    }
    expect(logpoint_finished, "restarted debug session reaches a clean exit through its inferior TTY");
    expect(std::any_of(logpoint_output.begin(), logpoint_output.end(), [](const auto& line) {
      return line.find("result logpoint") != std::string::npos;
    }), "GDB logpoint prints its message and continues without stopping");
    debug_terminal.stop();
  }
  gdb.stop();

  std::error_code configure_cleanup_error;
  std::filesystem::remove(build / "CMakeCache.txt", configure_cleanup_error);
  std::filesystem::remove_all(build / "CMakeFiles", configure_cleanup_error);

  expect(exercisePty(std::filesystem::absolute(argv[1]), project.path),
    "Final Cut TUI menu, file chooser, and document deduplication work through a pseudo-terminal");
  expect(exerciseEmptyStartup(std::filesystem::absolute(argv[1]), project.path / "empty-config"),
    "starting without a path shows a no-project screen and does not start clangd");
  std::cout << "Integration smoke test passed\n";
}
