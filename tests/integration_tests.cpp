#include "tuiide/cmake_model.hpp"
#include "tuiide/document.hpp"
#include "tuiide/gdb_client.hpp"
#include "tuiide/git_session.hpp"
#include "tuiide/lsp_client.hpp"
#include "tuiide/process.hpp"
#include "tuiide/pseudo_terminal.hpp"
#include "tuiide/project_creation.hpp"
#include "tuiide/recovery.hpp"

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
#include <sys/prctl.h>
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

auto exerciseGitSession(const std::filesystem::path& path) -> bool {
  std::string output;
  if (!runProcess({"git", "--version"}, path, output)) return true;  // Git необязателен.
  if (!runProcess({"git", "init", "-q", path.string()}, path, output)) return false;
  const auto source = path / "имя [1] с пробелом.cpp";
  { std::ofstream file(source); file << "int value = 1;\n"; }
  tuiide::GitSession session;
  session.setRoot(path);
  const auto finish = [&]() -> std::optional<tuiide::GitUpdate> {
    std::optional<tuiide::GitUpdate> update;
    const bool completed = waitFor([&] { update = session.poll(); }, [&] { return update.has_value(); });
    return completed ? std::move(update) : std::nullopt;
  };
  if (!session.startStatus()) return false;
  const auto untracked = finish();
  if (!untracked || untracked->exit_code != 0 || untracked->files.size() != 1
      || !untracked->files[0].untracked() || untracked->files[0].path != source) return false;
  if (session.startStage(path.parent_path() / "outside.cpp")) return false;
  if (!session.startStage(source)) return false;
  const auto staged = finish();
  if (!staged || staged->exit_code != 0 || !session.startStatus()) return false;
  const auto staged_status = finish();
  if (!staged_status || staged_status->files.size() != 1 || !staged_status->files[0].staged()) return false;
  if (!session.startDiff(source, true)) return false;
  const auto diff = finish();
  if (!diff || diff->exit_code != 0 || diff->output.find("int value = 1") == std::string::npos) return false;
  if (!session.startUnstage(source, true)) return false;
  const auto unstaged = finish();
  if (!unstaged || unstaged->exit_code != 0 || !session.startStatus()) return false;
  const auto final_status = finish();
  if (!final_status || final_status->files.size() != 1 || !final_status->files[0].untracked()
      || !session.startStage(source)) return false;
  const auto restaged = finish();
  if (!restaged || restaged->exit_code != 0) return false;
  output.clear();
  if (!runProcess({"git", "-C", path.string(), "-c", "user.name=Test",
        "-c", "user.email=test@example.invalid", "commit", "-qm", "Add Unicode source"}, path, output))
    return false;
  if (!session.startHistory(source)) return false;
  const auto history = finish();
  return history && history->exit_code == 0
    && history->output.find("Add Unicode source") != std::string::npos;
}

void exerciseLspOutcomeMatrix(const std::filesystem::path& fake_server,
    const std::filesystem::path& project) {
  const auto fake_bin = project / "fake-lsp-matrix-bin";
  std::filesystem::create_directories(fake_bin);
  std::error_code link_error;
  std::filesystem::create_symlink(fake_server, fake_bin / "clangd", link_error);
  expect(!link_error, "fake clangd endpoint is installed for the response matrix");

  tuiide::Document document;
  std::string error;
  expect(document.load(project / "main.cpp", error), "LSP response matrix document loads");
  tuiide::LspClient lsp;
  expect(lsp.start(project, {}, {{"PATH", fake_bin.string()},
      {"TUIIDE_FAKE_NULLABLE_DIAGNOSTICS", "1"}}),
    "deterministic LSP response endpoint starts");
  lsp.open(document);
  lsp.setActiveDocument(&document);
  expect(waitFor([&] { lsp.poll(); }, [&] { return lsp.ready(); }),
    "deterministic LSP response endpoint initializes");
  expect(waitFor([&] { lsp.poll(); }, [&] { return !lsp.diagnostics().empty(); })
      && lsp.diagnostics().front().severity == 0
      && lsp.diagnostics().front().message.empty(),
    "nullable diagnostic fields use safe defaults without dropping the message");

  const auto expectFeedback = [&](tuiide::LspOperation operation, bool is_error) {
    std::vector<tuiide::LspFeedback> feedback;
    const auto received = waitFor([&] {
      lsp.poll();
      auto batch = lsp.takeFeedback();
      feedback.insert(feedback.end(), std::make_move_iterator(batch.begin()),
        std::make_move_iterator(batch.end()));
    }, [&] {
      return std::any_of(feedback.begin(), feedback.end(), [&](const auto& item) {
        return item.operation == operation && item.error == is_error;
      });
    });
    if (!received)
      std::cerr << "Missing LSP feedback: operation=" << static_cast<int>(operation)
                << " error=" << is_error << '\n';
    expect(received, is_error ? "LSP operation exposes JSON-RPC errors"
                              : "LSP operation exposes an explicit empty result");
  };
  const auto setScenario = [&](std::size_t line) { document.setCursor({line, 0}); };

  setScenario(0); lsp.requestCompletion(document);
  std::vector<tuiide::LspCompletionItem> completions;
  expect(waitFor([&] { lsp.poll(); }, [&] {
    completions = lsp.takeCompletions(); return !completions.empty();
  }) && completions.front().label == "matrixItem", "completion has a normal result");
  setScenario(1); lsp.requestCompletion(document); expectFeedback(tuiide::LspOperation::Completion, false);
  setScenario(2); lsp.requestCompletion(document); expectFeedback(tuiide::LspOperation::Completion, true);

  setScenario(0); lsp.requestHover(document);
  std::string hover;
  expect(waitFor([&] { lsp.poll(); }, [&] { hover = lsp.takeHover(); return !hover.empty(); })
      && hover == "matrix hover", "hover has a normal result");
  setScenario(1); lsp.requestHover(document); expectFeedback(tuiide::LspOperation::Hover, false);
  setScenario(2); lsp.requestHover(document); expectFeedback(tuiide::LspOperation::Hover, true);

  const auto exerciseLocations = [&](tuiide::LspOperation operation, const auto& request,
      const auto& take, const char* normal_message) {
    setScenario(0); request();
    std::vector<tuiide::SourceLocation> locations;
    expect(waitFor([&] { lsp.poll(); }, [&] { locations = take(); return !locations.empty(); }), normal_message);
    setScenario(1); request(); expectFeedback(operation, false);
    setScenario(2); request(); expectFeedback(operation, true);
  };
  exerciseLocations(tuiide::LspOperation::Definition,
    [&] { lsp.requestDefinition(document); }, [&] { return lsp.takeDefinitions(); },
    "definition navigation has a normal result");
  exerciseLocations(tuiide::LspOperation::References,
    [&] { lsp.requestReferences(document); }, [&] { return lsp.takeReferences(); },
    "reference navigation has a normal result");

  setScenario(0); lsp.requestRename(document, "renamed");
  std::optional<tuiide::WorkspaceEdit> rename;
  expect(waitFor([&] { lsp.poll(); }, [&] { rename = lsp.takeRenameEdit(); return rename.has_value(); })
      && !rename->files.empty(), "rename has a normal workspace edit");
  setScenario(1); lsp.requestRename(document, "renamed"); expectFeedback(tuiide::LspOperation::Rename, false);
  setScenario(2); lsp.requestRename(document, "renamed"); expectFeedback(tuiide::LspOperation::Rename, true);

  setScenario(0); lsp.requestCodeActions(document, {0, 0}, {0, 6});
  std::vector<tuiide::LspCodeAction> actions;
  expect(waitFor([&] { lsp.poll(); }, [&] { actions = lsp.takeCodeActions(); return !actions.empty(); })
      && actions.front().title == "Matrix quick fix", "quick fix has a normal workspace edit");
  setScenario(1); lsp.requestCodeActions(document, {1, 0}, {1, 0});
  expectFeedback(tuiide::LspOperation::CodeActions, false);
  setScenario(2); lsp.requestCodeActions(document, {2, 0}, {2, 0});
  expectFeedback(tuiide::LspOperation::CodeActions, true);

  lsp.stop();
}

auto exercisePty(const std::filesystem::path& tuiide, const std::filesystem::path& project,
    bool exercise_resize = false, const std::filesystem::path& secondary_project = {}) -> bool {
  const auto test_config = project / ".test-config";
  if (!secondary_project.empty()) {
    std::filesystem::create_directories(test_config / "tuiide");
    std::ofstream history(test_config / "tuiide/recent-projects.json");
    history << "{\"version\":1,\"projects\":[\"" << secondary_project.string() << "\"]}\n";
  }
  int master{-1};
  winsize window{24, 80, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
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
  constexpr std::string_view alt_file{"\033f"};
  (void)::write(master, alt_file.data(), alt_file.size());
  // Начальная горячая буква O может быть отделена ANSI-кодами оформления.
  const auto menu_opened = waitFor(pumpScreen, [&] { return screen.find("pen Project...") != std::string::npos; }, 5s);
  const auto menu_focused = menu_opened;
  const char enter = '\r';
  const char escape = 27;
  struct MenuCheck {
    std::string_view key;
    std::string_view marker;
  };
  constexpr MenuCheck menu_checks[]{
    {"\033e", "Undo"}, {"\033s", "Find..."}, {"\033p", "Refresh tree"},
    {"\033r", "Configure"}, {"\033d", "Start / Continue"},
    {"\033t", "Completion"}, {"\033w", "Previous file"},
    {"\033h", "Keyboard shortcuts"}};
  bool all_top_menus_visible = menu_opened;
  for (const auto& check : menu_checks) {
    screen.clear();
    (void)::write(master, &escape, 1);
    (void)waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 2s);
    std::this_thread::sleep_for(150ms); pumpScreen();
    screen.clear();
    (void)::write(master, check.key.data(), check.key.size());
    const auto marker_visible = waitFor(pumpScreen, [&] {
      return screen.find(check.marker) != std::string::npos;
    }, 3s);
    if (!marker_visible) std::cerr << "PTY menu marker missing: " << check.marker << '\n';
    all_top_menus_visible = marker_visible && all_top_menus_visible;
  }
  screen.clear();
  (void)::write(master, &escape, 1);
  const auto menu_closed = waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 5s);
  std::this_thread::sleep_for(150ms);
  pumpScreen();
  screen.clear();
  constexpr std::string_view alt_project{"\033p"};
  (void)::write(master, alt_project.data(), alt_project.size());
  const auto project_menu_opened = waitFor(pumpScreen, [&] {
    return screen.find("Refresh tree") != std::string::npos;
  }, 3s);
  const char refresh_project = 'r';
  screen.clear();
  (void)::write(master, &refresh_project, 1);
  const auto project_refresh_reported = waitFor(pumpScreen, [&] {
    return screen.find("Project tree refreshed:") != std::string::npos;
  }, 5s);
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
  const char paste = 22;  // Ctrl+V
  screen.clear();
  (void)::write(master, &paste, 1);
  const auto empty_paste_reported = waitFor(pumpScreen, [&] {
    return screen.find("Paste unavailable: clipboard is empty") != std::string::npos;
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
  const char copy = 3;  // Ctrl+C
  screen.clear();
  (void)::write(master, &copy, 1);
  const auto copy_reported = waitFor(pumpScreen, [&] {
    return screen.find("Copied selection to clipboard") != std::string::npos;
  }, 5s);
  constexpr std::string_view right_key{"\033[C"};
  (void)::write(master, right_key.data(), right_key.size());
  const char edit = ' ';
  (void)::write(master, &edit, 1);
  std::this_thread::sleep_for(150ms);
  pumpScreen();
  screen.clear();
  (void)::write(master, alt_file.data(), alt_file.size());
  const auto save_all_visible = waitFor(pumpScreen, [&] { return screen.find("Save A") != std::string::npos; }, 5s);
  const char save_all = 'l';
  screen.clear();
  (void)::write(master, &save_all, 1);
  const auto all_saved = waitFor(pumpScreen, [&] { return screen.find("Save All: saved 1 document") != std::string::npos; }, 5s);
  constexpr std::string_view build_key{"\002"};  // Ctrl+Shift+B is Ctrl+B in a terminal.
  screen.clear();
  (void)::write(master, build_key.data(), build_key.size());
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
  constexpr std::string_view f9{"\033[20~"};
  screen.clear();
  (void)::write(master, f9.data(), f9.size());
  const auto breakpoint_created = waitFor(pumpScreen, [&] {
    return screen.find("Breakpoint set:") != std::string::npos;
  }, 5s);
  screen.clear();
  (void)::write(master, &new_file, 1);
  const auto close_all_ready = waitFor(pumpScreen, [&] { return screen.find("2 file(s)") != std::string::npos; }, 5s);
  screen.clear();
  constexpr std::string_view close_all_key{"\033W"};
  (void)::write(master, close_all_key.data(), close_all_key.size());
  const auto all_documents_closed = waitFor(pumpScreen, [&] {
    return screen.find("No file") != std::string::npos || screen.find("0 file(s)") != std::string::npos;
  }, 5s);
  const auto created_directory = project / "zz_pty_created";
  const auto renamed_directory = project / "zz_pty_renamed";
  const auto outside_directory = project.parent_path()
    / ("tuiide-pty-outside-" + std::to_string(static_cast<long long>(::getpid())));
  constexpr std::string_view new_directory_key{"\033n"};
  constexpr std::string_view f2{"\033OQ"};
  constexpr std::string_view delete_key{"\033[3~"};
  const auto focusLastProjectEntry = [&] {
    screen.clear();
    (void)::write(master, &focus_project, 1);
    const auto visible = waitFor(pumpScreen, [&] { return screen.find("main.cpp") != std::string::npos; }, 3s);
    (void)::write(master, end_key.data(), end_key.size());
    std::this_thread::sleep_for(200ms); pumpScreen();
    return visible;
  };

  screen.clear();
  (void)::write(master, new_directory_key.data(), new_directory_key.size());
  const auto create_cancel_dialog = waitFor(pumpScreen, [&] {
    return screen.find("New project directory") != std::string::npos;
  }, 3s);
  (void)::write(master, &escape, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool create_cancelled = create_cancel_dialog && !std::filesystem::exists(created_directory);

  screen.clear();
  (void)::write(master, new_directory_key.data(), new_directory_key.size());
  (void)waitFor(pumpScreen, [&] { return screen.find("New project directory") != std::string::npos; }, 3s);
  const auto invalid_directory = std::string("../") + outside_directory.filename().string() + "\r";
  (void)::write(master, invalid_directory.data(), invalid_directory.size());
  const auto create_error = waitFor(pumpScreen, [&] {
    return screen.find("Directory path must be inside the project root") != std::string::npos;
  }, 3s);
  (void)::write(master, &enter, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool create_error_safe = create_error && !std::filesystem::exists(outside_directory);

  screen.clear();
  (void)::write(master, new_directory_key.data(), new_directory_key.size());
  (void)waitFor(pumpScreen, [&] { return screen.find("New project directory") != std::string::npos; }, 3s);
  constexpr std::string_view created_name{"zz_pty_created\r"};
  (void)::write(master, created_name.data(), created_name.size());
  const auto directory_created = waitFor(pumpScreen, [&] {
    return std::filesystem::is_directory(created_directory)
      && screen.find("Project directory created:") != std::string::npos;
  }, 5s);

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, f2.data(), f2.size());
  const auto rename_cancel_dialog = waitFor(pumpScreen, [&] {
    return screen.find("Rename / Move project entry") != std::string::npos;
  }, 3s);
  (void)::write(master, &escape, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool rename_cancelled = std::filesystem::is_directory(created_directory)
    && !std::filesystem::exists(renamed_directory);

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, f2.data(), f2.size());
  (void)waitFor(pumpScreen, [&] { return screen.find("Rename / Move project entry") != std::string::npos; }, 3s);
  const auto invalid_rename = std::string("../") + outside_directory.filename().string() + "\r";
  (void)::write(master, invalid_rename.data(), invalid_rename.size());
  const auto rename_error = waitFor(pumpScreen, [&] {
    return screen.find("inside the project root") != std::string::npos;
  }, 3s);
  (void)::write(master, &enter, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool rename_error_safe = rename_error && std::filesystem::is_directory(created_directory)
    && !std::filesystem::exists(outside_directory);

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, f2.data(), f2.size());
  (void)waitFor(pumpScreen, [&] { return screen.find("Rename / Move project entry") != std::string::npos; }, 3s);
  constexpr std::string_view renamed_name{"zz_pty_renamed\r"};
  (void)::write(master, renamed_name.data(), renamed_name.size());
  const auto directory_renamed = waitFor(pumpScreen, [&] {
    return !std::filesystem::exists(created_directory)
      && std::filesystem::is_directory(renamed_directory)
      && screen.find("Project entry moved:") != std::string::npos;
  }, 5s);

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, delete_key.data(), delete_key.size());
  const auto delete_cancel_dialog = waitFor(pumpScreen, [&] {
    return screen.find("Delete empty directory from disk") != std::string::npos;
  }, 3s);
  const char no = 'n'; (void)::write(master, &no, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool delete_cancelled = delete_cancel_dialog && std::filesystem::is_directory(renamed_directory);

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, delete_key.data(), delete_key.size());
  const auto delete_dialog = waitFor(pumpScreen, [&] {
    return screen.find("Delete empty directory from disk") != std::string::npos;
  }, 3s);
  const char yes = 'y'; (void)::write(master, &yes, 1);
  const auto directory_deleted = waitFor(pumpScreen, [&] {
    return !std::filesystem::exists(renamed_directory)
      && screen.find("Deleted empty project directory:") != std::string::npos;
  }, 5s);
  const bool project_mutation_dialogs = create_cancelled && create_error_safe && directory_created
    && rename_cancelled && rename_error_safe && directory_renamed
    && delete_cancelled && delete_dialog && directory_deleted;

  const auto generated_header = project / "zz_pty_generated.h";
  const auto cmakeContainsGeneratedHeader = [&] {
    std::ifstream input(project / "CMakeLists.txt");
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find("zz_pty_generated.h") != std::string::npos;
  };
  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, insert_key.data(), insert_key.size());
  const auto template_normal_picker = waitFor(pumpScreen, [&] {
    return screen.find("C header") != std::string::npos;
  }, 3s);
  (void)::write(master, &enter, 1);
  const auto template_path = waitFor(pumpScreen, [&] {
    return screen.find("new_header.h") != std::string::npos;
  }, 3s);
  const std::string replace_default_name(30, '\177');
  (void)::write(master, replace_default_name.data(), replace_default_name.size());
  constexpr std::string_view generated_name{"zz_pty_generated.h\r"};
  (void)::write(master, generated_name.data(), generated_name.size());
  const auto template_created = waitFor(pumpScreen, [&] {
    return std::filesystem::is_regular_file(generated_header) && cmakeContainsGeneratedHeader()
      && screen.find("Project: created 1 file") != std::string::npos;
  }, 5s);

  screen.clear(); (void)::write(master, &focus_project, 1);
  (void)waitFor(pumpScreen, [&] { return screen.find("zz_pty_generated.h") != std::string::npos; }, 3s);
  (void)::write(master, insert_key.data(), insert_key.size());
  const auto duplicate_picker = waitFor(pumpScreen, [&] { return screen.find("C header") != std::string::npos; }, 3s);
  (void)::write(master, &enter, 1);
  const auto duplicate_path = waitFor(pumpScreen, [&] { return screen.find("new_header.h") != std::string::npos; }, 3s);
  (void)::write(master, replace_default_name.data(), replace_default_name.size());
  (void)::write(master, generated_name.data(), generated_name.size());
  const auto template_duplicate_error = waitFor(pumpScreen, [&] {
    return screen.find("selected file already exists") != std::string::npos;
  }, 3s);
  (void)::write(master, &enter, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  (void)::write(master, &escape, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool duplicate_safe = duplicate_picker && duplicate_path && template_duplicate_error
    && std::filesystem::is_regular_file(generated_header) && cmakeContainsGeneratedHeader();

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, delete_key.data(), delete_key.size());
  const auto remove_cancel_picker = waitFor(pumpScreen, [&] {
    return screen.find("Remove from CMake project only") != std::string::npos;
  }, 3s);
  (void)::write(master, &escape, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool remove_cancelled = std::filesystem::is_regular_file(generated_header)
    && cmakeContainsGeneratedHeader();

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, &enter, 1);
  const auto generated_opened = waitFor(pumpScreen, [&] {
    return screen.find("zz_pty_generated.h") != std::string::npos
      && screen.find("1 file(s)") != std::string::npos;
  }, 3s);
  const char modify_header = ' '; (void)::write(master, &modify_header, 1);
  std::this_thread::sleep_for(300ms); pumpScreen();
  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, delete_key.data(), delete_key.size());
  const auto remove_modified_picker = waitFor(pumpScreen, [&] {
    return screen.find("Remove from CMake project only") != std::string::npos;
  }, 3s);
  constexpr std::string_view down{"\033[B"};
  (void)::write(master, down.data(), down.size()); (void)::write(master, &enter, 1);
  const auto remove_modified_error = waitFor(pumpScreen, [&] {
    return screen.find("Save or close the modified file") != std::string::npos;
  }, 5s);
  (void)::write(master, &enter, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  (void)::write(master, &close_file, 1);
  const auto unsaved_header_prompt = waitFor(pumpScreen, [&] {
    return screen.find("Save this document before closing") != std::string::npos;
  }, 5s);
  const char no_save = 'n'; (void)::write(master, &no_save, 1);
  std::this_thread::sleep_for(150ms); pumpScreen();
  const bool modified_delete_safe = remove_modified_error && unsaved_header_prompt
    && std::filesystem::is_regular_file(generated_header) && cmakeContainsGeneratedHeader();

  (void)focusLastProjectEntry();
  screen.clear(); (void)::write(master, delete_key.data(), delete_key.size());
  const auto remove_normal_picker = waitFor(pumpScreen, [&] {
    return screen.find("Remove from CMake project only") != std::string::npos;
  }, 3s);
  (void)::write(master, down.data(), down.size()); (void)::write(master, &enter, 1);
  const auto remove_confirmation = waitFor(pumpScreen, [&] {
    return screen.find("Permanently delete from disk") != std::string::npos;
  }, 3s);
  (void)::write(master, &yes, 1);
  const auto generated_deleted = waitFor(pumpScreen, [&] {
    return !std::filesystem::exists(generated_header) && !cmakeContainsGeneratedHeader();
  }, 5s);
  const bool template_and_file_dialogs = template_normal_picker && template_path && template_created
    && duplicate_safe && remove_cancelled && modified_delete_safe
    && remove_normal_picker && remove_confirmation && generated_deleted;
  bool project_switched = secondary_project.empty();
  bool secondary_settings_loaded = secondary_project.empty();
  bool old_breakpoints_cleared = secondary_project.empty();
  bool recent_menu_seen = secondary_project.empty();
  bool recent_picker_seen = secondary_project.empty();
  if (!secondary_project.empty()) {
    screen.clear();
    (void)::write(master, alt_file.data(), alt_file.size());
    recent_menu_seen = waitFor(pumpScreen, [&] {
      return screen.find("Project") != std::string::npos;
    }, 2s);
    const char recent_projects = 'r';
    screen.clear();
    (void)::write(master, &recent_projects, 1);
    recent_picker_seen = waitFor(pumpScreen, [&] {
      return screen.find("Recent projects") != std::string::npos
        && screen.find(secondary_project.string()) != std::string::npos;
    }, 5s);
    // История содержит ровно две записи: текущую и заранее подготовленную.
    // Два Down выбирают вторую при начальном индексе как 0, так и 1.
    (void)::write(master, down.data(), down.size());
    (void)::write(master, down.data(), down.size());
    (void)::write(master, &enter, 1);
    project_switched = recent_picker_seen && waitFor(pumpScreen, [&] {
      // Быстрый путь, когда после закрытия picker активна панель Output.
      return screen.find("Project opened: " + secondary_project.string()) != std::string::npos;
    }, 10s);
    old_breakpoints_cleared = screen.find("Previous project state cleared") != std::string::npos
      && screen.find("Project state cleanup incomplete") == std::string::npos;

    screen.clear();
    constexpr std::string_view launch_settings{"\033l"};
    (void)::write(master, launch_settings.data(), launch_settings.size());
    (void)waitFor(pumpScreen, [&] {
      return screen.find("Run/Debug configurations") != std::string::npos;
    }, 5s);
    constexpr std::string_view edit_launch{"\033e"};
    screen.clear();
    (void)::write(master, edit_launch.data(), edit_launch.size());
    secondary_settings_loaded = waitFor(pumpScreen, [&] {
      return screen.find("SECOND_PROJECT_ARGUMENT") != std::string::npos;
    }, 5s);
    // Настройки второго проекта и очистка состояния первого — проверяемые
    // постусловия смены проекта даже при скрытой панели Output.
    project_switched = project_switched
      || (recent_picker_seen && secondary_settings_loaded && old_breakpoints_cleared);
    (void)::write(master, &escape, 1);
    std::this_thread::sleep_for(150ms); pumpScreen();
    (void)::write(master, &escape, 1);
    (void)waitFor(pumpScreen, [&] { return screen.find("TUI IDE") != std::string::npos; }, 3s);
    std::this_thread::sleep_for(300ms);
    pumpScreen();
  }
  screen.clear();
  (void)::write(master, alt_file.data(), alt_file.size());
  (void)waitFor(pumpScreen, [&] { return screen.find("Close Pro") != std::string::npos; }, 5s);
  const char close_project = 'j';
  screen.clear();
  (void)::write(master, &close_project, 1);
  const auto project_closed = waitFor(pumpScreen, [&] {
    return screen.find("No project") != std::string::npos
      || screen.find("Previous project state cleared") != std::string::npos;
  }, 5s);
  const char save_without_document = 19;  // Ctrl+S
  screen.clear();
  (void)::write(master, &save_without_document, 1);
  const auto unavailable_reported = waitFor(pumpScreen, [&] {
    return screen.find("Save unavailable") != std::string::npos;
  }, 5s);
  const auto unavailableCommand = [&](std::string_view key, std::string_view message) {
    screen.clear();
    (void)::write(master, key.data(), key.size());
    return waitFor(pumpScreen, [&] { return screen.find(message) != std::string::npos; }, 5s);
  };
  const std::string close_key(1, static_cast<char>(23));
  const std::string find_key(1, static_cast<char>(6));
  const bool close_unavailable = unavailableCommand(close_key, "Close unavailable");
  const bool find_unavailable = unavailableCommand(find_key, "Find unavailable");
  const bool lsp_unavailable = unavailableCommand("\033OP", "Symbol information unavailable");
  const bool debug_unavailable = unavailableCommand("\033[15~", "Debug unavailable");
  const bool run_unavailable = unavailableCommand("\033[17~", "Run unavailable");
  const bool build_unavailable = unavailableCommand("\002", "Build unavailable");
  const bool breakpoint_unavailable = unavailableCommand("\033[20~", "Breakpoint unavailable");
  const bool step_unavailable = unavailableCommand("\033[18~", "Step into unavailable");
  const bool next_unavailable = unavailableCommand("\033[19~", "Next unavailable");
  const bool finish_unavailable = unavailableCommand("\033[19;3~", "Step out unavailable");
  const bool diagnostic_unavailable = unavailableCommand(
    "\033[19;5~", "No build, analysis, or clangd diagnostics");
  const bool preset_unavailable = unavailableCommand("\020", "Configure preset unavailable");
  const bool watch_unavailable = unavailableCommand("\033U", "Add watch unavailable");
  const bool registers_unavailable = unavailableCommand("\022", "Registers unavailable");
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
    && search_dialog_opened && empty_paste_reported && copy_reported
    && save_all_visible && all_saved && build_completed && run_completed
    && breakpoint_created && close_all_ready && project_switched
    && project_mutation_dialogs && template_and_file_dialogs
    && secondary_settings_loaded && old_breakpoints_cleared
    && all_documents_closed && project_closed && unavailable_reported
    && menu_focused && menu_opened && all_top_menus_visible && menu_closed
    && project_menu_opened && project_refresh_reported
    && command_palette_opened && command_palette_filtered
    && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  const auto unavailable_complete = close_unavailable && find_unavailable && lsp_unavailable
    && debug_unavailable && run_unavailable && build_unavailable && breakpoint_unavailable
    && step_unavailable && next_unavailable && finish_unavailable && diagnostic_unavailable
    && preset_unavailable && watch_unavailable && registers_unavailable;
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
              << " empty_paste=" << empty_paste_reported << " copy=" << copy_reported
              << " save_all_visible=" << save_all_visible << " all_saved=" << all_saved
              << " build_completed=" << build_completed << " build_finalized=" << build_finalized
              << " run_completed=" << run_completed
              << " breakpoint_created=" << breakpoint_created
              << " close_all_ready=" << close_all_ready << " all_closed=" << all_documents_closed
              << " mutation_dialogs=" << project_mutation_dialogs
              << " create_cancel=" << create_cancelled << " create_error=" << create_error_safe
              << " created=" << directory_created << " rename_cancel_dialog=" << rename_cancel_dialog
              << " rename_cancel=" << rename_cancelled
              << " rename_error=" << rename_error_safe << " renamed=" << directory_renamed
              << " delete_cancel=" << delete_cancelled << " delete_dialog=" << delete_dialog
              << " deleted=" << directory_deleted
              << " template_file_dialogs=" << template_and_file_dialogs
              << " template_picker=" << template_normal_picker << " template_path=" << template_path
              << " template_created=" << template_created << " duplicate_safe=" << duplicate_safe
              << " remove_cancel_picker=" << remove_cancel_picker << " remove_cancel=" << remove_cancelled
              << " generated_opened=" << generated_opened
              << " modified_picker=" << remove_modified_picker
              << " modified_error=" << remove_modified_error
              << " unsaved_prompt=" << unsaved_header_prompt
              << " modified_safe=" << modified_delete_safe
              << " remove_picker=" << remove_normal_picker << " remove_confirm=" << remove_confirmation
              << " generated_deleted=" << generated_deleted
              << " switched=" << project_switched << " settings=" << secondary_settings_loaded
              << " breakpoints_cleared=" << old_breakpoints_cleared
              << " recent_menu=" << recent_menu_seen << " recent_picker=" << recent_picker_seen
              << " project_unloaded=" << project_closed
              << " unavailable=" << unavailable_reported
              << " focused=" << menu_focused << " menu=" << menu_opened << " closed=" << menu_closed
              << " all_menus=" << all_top_menus_visible
              << " project_menu=" << project_menu_opened << " project_refresh=" << project_refresh_reported
              << " palette=" << command_palette_opened
              << " palette_filter=" << command_palette_filtered
              << " unavailable_commands=" << unavailable_complete
              << " [close=" << close_unavailable << " find=" << find_unavailable
              << " lsp=" << lsp_unavailable << " debug=" << debug_unavailable
              << " run=" << run_unavailable << " build=" << build_unavailable
              << " breakpoint=" << breakpoint_unavailable << " step=" << step_unavailable
              << " next=" << next_unavailable << " finish=" << finish_unavailable
              << " diagnostic=" << diagnostic_unavailable
              << " preset=" << preset_unavailable << " watch=" << watch_unavailable
              << " registers=" << registers_unavailable << "]"
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
  const auto log = config / "diagnostic.log";
  int master{-1};
  winsize window{24, 80, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--diagnostic", "--log-file",
      log.c_str(), static_cast<char*>(nullptr));
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
      && screen.find("clangd: off") != std::string::npos;
  }, 5s);
  const char quit = 4;
  (void)::write(master, &quit, 1);
  int status{};
  const auto exited = waitFor(pump, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 5s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);
  std::ifstream log_input(log);
  const std::string log_text((std::istreambuf_iterator<char>(log_input)),
    std::istreambuf_iterator<char>());
  const bool diagnostic_logged =
    log_text.find("\toutput\tsystem\tinformation\tDiagnostic mode enabled") != std::string::npos
    && log_text.find("Diagnostic: CMake found at") != std::string::npos
    && log_text.find("Welcome to TUI IDE") != std::string::npos;
  const bool success = ready && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0
    && diagnostic_logged;
  if (!success) {
    std::cerr << "Empty startup: ready=" << ready << " exited=" << exited
      << " status=" << status << " diagnostic_logged=" << diagnostic_logged
      << "\nLog:\n" << log_text << "\nCaptured terminal bytes=" << screen.size() << '\n';
  }
  return success;
}

auto exerciseImportLifecyclePty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace, const std::filesystem::path& source,
    const std::filesystem::path& build, bool debugger_restricted) -> bool {
  const auto log = workspace / "lifecycle.log";
  int master{-1};
  winsize window{24, 80, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("XDG_CONFIG_HOME", (source / ".config").c_str(), 1);
    if (::chdir(workspace.c_str()) != 0) _exit(126);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--log-file", log.c_str(),
      static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const auto logged = [&](std::string_view value) {
    std::ifstream input(log);
    const std::string text((std::istreambuf_iterator<char>(input)),
      std::istreambuf_iterator<char>());
    return text.find(value) != std::string::npos;
  };

  const bool started = visible("No project");
  constexpr std::string_view down{"\033[B"};
  screen.clear(); send("\033f"); visible("New Project", 3s); send(down); send("\r");
  const bool project_picker = visible("Open CMake project");
  screen.clear(); send("\r");
  const bool source_entered = visible(source.string());
  screen.clear(); send("\033s");
  const bool import_offer = visible("CMakeLists.txt was not found");
  screen.clear(); send("\r");
  const bool import_settings = visible("Import source directory");
  screen.clear(); send("\033p");
  const bool build_picker = visible("Select import build directory");
  screen.clear(); send("\033d");
  const bool directory_prompt = visible("Directory name:");
  screen.clear(); send(build.filename().string()); send("\r");
  const bool build_entered = visible(build.string());
  screen.clear(); send("\033s");
  const bool preview = visible("Import preview");
  screen.clear(); send("\033a");
  const bool imported = visible("Imported 1 source and header files", 10s);

  constexpr std::string_view build_key{"\002"};
  screen.clear(); send(build_key);
  const bool built = waitFor(pump, [&] {
    return logged("Build finished with exit code 0");
  }, 45s);
  const bool artifact = std::filesystem::is_regular_file(build / "ui_import");

  screen.clear(); send("\033l");
  const bool launch_manager = visible("Run/Debug configurations");
  screen.clear(); send("\033e");
  const bool launch_dialog = visible("Launch configuration");
  send("ui-argument");
  screen.clear(); send("\033s");
  const bool launch_edited = visible("Run/Debug configurations");
  screen.clear(); send("\033s");
  const bool launch_saved = visible("TUI IDE");

  constexpr std::string_view f6{"\033[17~"};
  screen.clear(); send(f6);
  std::this_thread::sleep_for(300ms); pump();
  send("run-input\r");
  const bool run_finished = visible("Run finished with exit code 0", 10s);
  const bool run_waiting = logged("ui_import ui-argument");
  const bool run_input = run_finished;

  constexpr std::string_view f5{"\033[15~"};
  screen.clear(); send(f5);
  const bool debug_console = visible("stdin>", 5s);
  send("debug-input\r");
  const bool debug_started = waitFor(pump, [&] {
    return logged("GDB:") || logged("ptrace: Operation not permitted");
  }, 15s);
  bool ptrace_restricted = debugger_restricted
    || logged("ptrace: Operation not permitted");
  bool debug_input{};
  bool debug_finished{};
  if (!ptrace_restricted) {
    debug_input = visible("INPUT=debug-input", 15s);
    debug_finished = debug_input && visible("Debuggee finished", 10s);
  }
  if (ptrace_restricted) std::cout << "UI lifecycle debug execution skipped: ptrace is restricted\n";

  screen.clear(); send("\033f");
  (void)visible("Exit", 3s);
  send("x");
  int status{};
  const auto exited = waitFor(pump, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 8s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);

  const bool files_created = std::filesystem::is_regular_file(source / "CMakeLists.txt")
    && std::filesystem::is_regular_file(source / ".tuiide-project.json");
  std::ifstream settings_input(source / ".tuiide-project.json");
  const std::string settings_text((std::istreambuf_iterator<char>(settings_input)),
    std::istreambuf_iterator<char>());
  const bool argument_saved = settings_text.find("ui-argument") != std::string::npos;
  ptrace_restricted = ptrace_restricted || logged("ptrace: Operation not permitted");
  const bool success = started && project_picker && import_offer
    && import_settings && build_picker && directory_prompt && preview
    && imported && built && artifact && launch_manager && launch_dialog && launch_edited
    && launch_saved && argument_saved && run_waiting
    && run_input && run_finished && debug_started
    && (debug_finished || ptrace_restricted)
    && files_created && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::cerr << "UI import lifecycle: started=" << started << " picker=" << project_picker
      << " source=" << source_entered << " offer=" << import_offer
      << " settings=" << import_settings << " build_picker=" << build_picker
      << " prompt=" << directory_prompt << " build_dir=" << build_entered
      << " preview=" << preview << " imported=" << imported << " built=" << built
      << " artifact=" << artifact << " launch_manager=" << launch_manager
      << " launch=" << launch_dialog << " launch_edited=" << launch_edited
      << " launch_saved=" << launch_saved << " argument_saved=" << argument_saved
      << " run_waiting=" << run_waiting
      << " run_input=" << run_input << " run_finished=" << run_finished
      << " debug_console=" << debug_console << " debug_started=" << debug_started
      << " debug_input=" << debug_input
      << " debug_finished=" << debug_finished << " ptrace=" << ptrace_restricted
      << " files=" << files_created << " exited=" << exited << " status=" << status << '\n';
  }
  return success;
}

auto exerciseProjectDialogsPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace) -> bool {
  const auto settings_project = workspace / "settings-project";
  const auto original_build = workspace / "settings-build";
  const auto updated_build = workspace / "settings-build-updated";
  std::filesystem::create_directories(settings_project);
  {
    std::ofstream file(settings_project / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(settings_fixture LANGUAGES CXX)\n"
            "add_executable(settings_fixture main.cpp)\n";
  }
  { std::ofstream file(settings_project / "main.cpp"); file << "int main() { return 0; }\n"; }
  {
    std::ofstream file(settings_project / ".tuiide-project.json");
    file << "{\"version\":1,\"buildDirectory\":\"" << original_build.string()
      << "\",\"buildJobs\":1}\n";
  }
  const auto settings_file = settings_project / ".tuiide-project.json";
  const auto readFile = [](const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  };
  const auto original_settings = readFile(settings_file);

  const auto runSettings = [&] {
    int master{-1};
    winsize window{26, 90, 0, 0};
    const auto child = ::forkpty(&master, nullptr, nullptr, &window);
    if (child < 0) return false;
    if (child == 0) {
      ::setenv("TERM", "xterm-256color", 1);
      ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
      ::setenv("TUIIDE_OSC52", "0", 1);
      const auto config = workspace / "settings-config";
      ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
      ::execl(tuiide.c_str(), tuiide.c_str(), settings_project.c_str(), static_cast<char*>(nullptr));
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
    const auto send = [&](std::string_view value) {
      return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
    };
    const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
      return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
    };
    const auto openSettings = [&] {
      screen.clear(); send("\033[21~");
      const bool menu_bar = visible("File", 3s);
      send("\r");
      const bool file_menu = visible("Open Project", 3s);
      for (int index = 0; index < 6; ++index) {
        send("\033[C");
        std::this_thread::sleep_for(50ms);
        pump();
      }
      (void)visible("Completion", 3s);
      screen.clear(); send("\033[A\r");
      const bool dialog = visible("Build directory:", 5s);
      (void)menu_bar;
      (void)file_menu;
      return dialog;
    };
    const auto clear_field = std::string(240, '\177');

    const bool started = visible("Open files");
    const bool cancel_opened = openSettings();
    screen.clear(); send("\033");
    const bool cancelled = visible("TUI IDE", 3s) && readFile(settings_file) == original_settings;
    std::this_thread::sleep_for(200ms); pump();

    const bool error_opened = openSettings();
    send(clear_field); send(settings_project.string());
    screen.clear(); send("\033s");
    const bool validation_error = visible("Build directory must differ from the source directory", 5s);
    screen.clear(); send("o");
    (void)visible("TUI IDE", 3s);
    std::this_thread::sleep_for(200ms); pump();
    const bool error_safe = readFile(settings_file) == original_settings;

    const bool normal_opened = openSettings();
    send(clear_field); send(updated_build.string());
    screen.clear(); send("\033s");
    const bool saved = visible("Project settings saved; build directory:", 8s);
    const auto updated_settings = readFile(settings_file);
    const bool persisted = updated_settings.find(updated_build.string()) != std::string::npos
      && updated_settings.find("\"buildJobs\": 1") != std::string::npos;

    screen.clear(); send("\033f"); (void)visible("Exit", 3s); send("x");
    int status{};
    const auto exited = waitFor(pump, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 8s);
    if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
    ::close(master);
    const bool success = started && cancel_opened && cancelled && error_opened
      && validation_error && error_safe && normal_opened && saved && persisted
      && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!success) {
      std::string printable;
      for (const unsigned char value : screen)
        if (value >= 0x20 && value < 0x7f) printable.push_back(static_cast<char>(value));
      std::cerr << "Project settings PTY: started=" << started << " cancel_opened=" << cancel_opened
        << " cancelled=" << cancelled << " error_opened=" << error_opened
        << " validation_error=" << validation_error << " error_safe=" << error_safe
        << " normal_opened=" << normal_opened << " saved=" << saved
        << " persisted=" << persisted << " exited=" << exited << " status=" << status
        << "\nScreen: " << printable.substr(0, 4000) << '\n';
    }
    return success;
  };

  const auto runWizard = [&] {
    const auto wizard_workspace = workspace / "wizard-workspace";
    const auto generated_project = wizard_workspace / "wizard-project";
    const auto generated_build = wizard_workspace / "wizard-build";
    std::filesystem::create_directories(wizard_workspace);
    int master{-1};
    winsize window{26, 90, 0, 0};
    const auto child = ::forkpty(&master, nullptr, nullptr, &window);
    if (child < 0) return false;
    if (child == 0) {
      ::setenv("TERM", "xterm-256color", 1);
      ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
      ::setenv("TUIIDE_OSC52", "0", 1);
      const auto config = workspace / "wizard-config";
      ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
      if (::chdir(wizard_workspace.c_str()) != 0) _exit(126);
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
    const auto send = [&](std::string_view value) {
      return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
    };
    const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
      return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
    };
    const auto openWizard = [&] {
      screen.clear(); send("\033f");
      const bool file_menu = visible("New Project", 3s);
      screen.clear(); send("p");
      return file_menu && visible("New project settings", 5s);
    };
    const bool started = visible("No project");
    const bool cancel_opened = openWizard();
    screen.clear(); send("\033");
    const bool cancelled = visible("No project", 3s)
      && !std::filesystem::exists(generated_project);

    const bool error_opened = openWizard();
    send("wizard_app"); send("\033n");
    const bool error_picker = visible("Select empty project directory", 5s);
    screen.clear(); send("\033d");
    const bool error_prompt = visible("Directory name:", 3s);
    screen.clear(); send("../outside"); send("\r");
    const bool path_error = visible("Enter one valid directory name", 5s);
    const bool error_safe = !std::filesystem::exists(workspace / "outside")
      && !std::filesystem::exists(generated_project);
    ::kill(child, SIGKILL);
    int error_status{};
    (void)::waitpid(child, &error_status, 0);
    ::close(master);
    const bool error_scenario = started && cancel_opened && cancelled && error_opened
      && error_picker && error_prompt && path_error && error_safe;
    if (!error_scenario) return false;

    int normal_master{-1};
    const auto normal_child = ::forkpty(&normal_master, nullptr, nullptr, &window);
    if (normal_child < 0) return false;
    if (normal_child == 0) {
      ::setenv("TERM", "xterm-256color", 1);
      ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
      ::setenv("TUIIDE_OSC52", "0", 1);
      const auto config = workspace / "wizard-normal-config";
      ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
      if (::chdir(wizard_workspace.c_str()) != 0) _exit(126);
      ::execl(tuiide.c_str(), tuiide.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    const auto normal_flags = ::fcntl(normal_master, F_GETFL, 0);
    if (normal_flags >= 0)
      (void)::fcntl(normal_master, F_SETFL, normal_flags | O_NONBLOCK);
    std::string normal_screen;
    const auto normalPump = [&] {
      char buffer[4096];
      const auto count = ::read(normal_master, buffer, sizeof(buffer));
      if (count > 0) normal_screen.append(buffer, static_cast<std::size_t>(count));
    };
    const auto normalSend = [&](std::string_view value) {
      return ::write(normal_master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
    };
    const auto normalVisible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
      return waitFor(normalPump, [&] { return normal_screen.find(value) != std::string::npos; }, timeout);
    };
    const auto normalCreateDirectory = [&](std::string_view name, std::string_view expected_path) {
      normal_screen.clear(); normalSend("\033d");
      const bool prompt = normalVisible("Directory name:", 3s);
      normal_screen.clear(); normalSend(name); normalSend("\r");
      return prompt && waitFor(normalPump, [&] {
        return std::filesystem::is_directory(std::filesystem::path(expected_path));
      }, 5s);
    };

    const bool normal_started = normalVisible("No project");
    normal_screen.clear(); normalSend("\033f");
    const bool normal_file_menu = normalVisible("New Project", 3s);
    normal_screen.clear(); normalSend("p");
    const bool normal_opened = normalVisible("New project settings", 5s);
    normalSend("wizard_app"); normalSend("\033n");
    const bool project_picker = normalVisible("Select empty project directory", 5s);
    const bool project_directory = normalCreateDirectory("wizard-project", generated_project.string());
    normal_screen.clear(); normalSend("\033s");
    const bool build_picker = normalVisible("Select build directory", 5s);
    const bool build_directory = normalCreateDirectory("wizard-build", generated_build.string());
    normal_screen.clear(); normalSend("\033s");
    const bool opened = waitFor(normalPump, [&] {
      return std::filesystem::is_regular_file(generated_build / "CMakeCache.txt");
    }, 20s);
    const bool generated = std::filesystem::is_regular_file(generated_project / "CMakeLists.txt")
      && std::filesystem::is_regular_file(generated_project / "src/main.cpp")
      && std::filesystem::is_regular_file(generated_project / ".tuiide-project.json");
    const auto settings = readFile(generated_project / ".tuiide-project.json");
    const bool build_saved = settings.find(generated_build.string()) != std::string::npos;

    normal_screen.clear(); normalSend("\033f");
    (void)normalVisible("Exit", 3s); normalSend("x");
    int status{};
    const auto exited = waitFor(normalPump, [&] {
      return ::waitpid(normal_child, &status, WNOHANG) == normal_child;
    }, 10s);
    if (!exited) { ::kill(normal_child, SIGKILL); (void)::waitpid(normal_child, &status, 0); }
    ::close(normal_master);
    const bool success = normal_started && normal_file_menu && normal_opened && project_picker
      && project_directory && build_picker && build_directory && opened && generated
      && build_saved && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!success) {
      std::cerr << "New project PTY: normal_started=" << normal_started
        << " file_menu=" << normal_file_menu << " normal_opened=" << normal_opened
        << " project_picker=" << project_picker << " project_directory=" << project_directory
        << " build_picker=" << build_picker << " build_directory=" << build_directory
        << " opened=" << opened << " generated=" << generated
        << " build_saved=" << build_saved << " exited=" << exited << " status=" << status << '\n';
    }
    return success;
  };

  return runSettings() && runWizard();
}

auto exerciseWindowHelpPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace) -> bool {
  const auto project = workspace / "window-help-project";
  const auto log = workspace / "window-help.log";
  std::filesystem::create_directories(project);
  {
    std::ofstream file(project / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(window_help LANGUAGES CXX)\n"
            "add_executable(window_help main.cpp)\n";
  }
  { std::ofstream file(project / "main.cpp"); file << "int main() { return 0; }\n"; }
  {
    std::ofstream file(project / ".tuiide-project.json");
    if (std::getenv("TUIIDE_LAUNCH_CONFIGS_ONLY") != nullptr) {
      file << R"({"version":1,"buildDirectory":"build","buildJobs":1,)"
              R"("activeLaunchConfiguration":"Default","launchConfigurations":[)"
              R"({"name":"Default","configuration":{"arguments":[]}},)"
              R"({"name":"Second","configuration":{"arguments":["--second"]}}]})"
           << '\n';
    } else {
      file << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":1}\n";
    }
  }
  const auto config = workspace / "window-help-config";
  const auto settings_file = config / "tuiide/settings.json";
  if (std::getenv("TUIIDE_LOCALE_ONLY") != nullptr) {
    std::filesystem::create_directories(settings_file.parent_path());
    std::ofstream settings(settings_file);
    settings << "{\"version\":1,\"language\":\"ru\"}\n";
  }
  if (std::getenv("TUIIDE_RECENT_FILES_ONLY") != nullptr) {
    std::filesystem::create_directories(settings_file.parent_path());
    std::ofstream settings(settings_file);
    settings << "{\"version\":1,\"recentFiles\":[\"" << (project / "main.cpp").string()
      << "\",\"" << (project / "main.cpp").string() << "\",\""
      << (project / "removed.cpp").string() << "\"]}\n";
  }

  int master{-1};
  winsize window{26, 90, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--log-file", log.c_str(), project.c_str(),
      static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const auto settle = [&] {
    std::this_thread::sleep_for(180ms);
    pump();
  };
  const auto logged = [&](std::string_view value) {
    std::ifstream input(log);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find(value) != std::string::npos;
  };
  const auto sessionText = [&] {
    std::ifstream input(project / "build/.tuiide-session.json");
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  };
  const auto openTopMenu = [&](int index, std::string_view marker) {
    constexpr std::string_view menu_keys[]{
      "\033f", "\033e", "\033s", "\033r", "\033p",
      "\033d", "\033t", "\033w", "\033h"};
    if (index < 0 || index >= static_cast<int>(std::size(menu_keys))) return false;
    screen.clear(); send(menu_keys[index]);
    return visible(marker, 3s);
  };
  const auto selectWindowFromEnd = [&](int up_count) {
    const bool menu = openTopMenu(7, "Show Open files");
    screen.clear();
    for (int index = 0; index < up_count; ++index) send("\033[A");
    send("\r");
    return menu;
  };
  const auto selectWindowMnemonic = [&](char mnemonic) {
    const bool menu = openTopMenu(7, "Show Open files");
    screen.clear(); send(std::string(1, mnemonic)); settle();
    return menu;
  };

  const bool started = visible(std::getenv("TUIIDE_LOCALE_ONLY") != nullptr
    ? "Открытые файлы" : "Open files");
  if (std::getenv("TUIIDE_LOCALE_ONLY") != nullptr) {
    settle();
    screen.clear(); send("\033f");
    const bool new_project_menu = visible("Закрыть остальные", 5s);
    screen.clear(); send("\r");
    const bool new_project_dialog = visible("Параметры нового проекта", 5s)
      && visible("Имя проекта:", 3s);
    screen.clear(); send("\033");
    std::this_thread::sleep_for(600ms); pump();
    screen.clear(); send("\033k");
    const bool palette_title = visible("Палитра команд", 5s);
    const bool palette_command = visible("Файл: Создать", 5s);
    const bool palette = palette_title && palette_command;
    screen.clear(); send("\033");
    std::this_thread::sleep_for(600ms); pump();
    screen.clear(); send("\016");
    const bool new_file = visible("1 файлов", 5s);
    screen.clear(); send("\006");
    const bool search_dialog = visible("Поиск и замена", 5s)
      && visible("Учитывать регистр", 5s);
    screen.clear(); send("\033");
    std::this_thread::sleep_for(600ms); pump();
    screen.clear(); send("\027"); settle();
    screen.clear(); send("\033l");
    const bool launch_dialog = visible("Конфигурации запуска и отладки", 5s)
      && visible("текущая цель CMake", 5s);
    screen.clear(); send("\033");
    std::this_thread::sleep_for(600ms); pump();
    screen.clear(); send("\020");
    const bool configure_presets = visible("Пресеты настройки CMake", 5s)
      && visible("Без пресета настройки", 5s);
    screen.clear(); send("\033");
    std::this_thread::sleep_for(600ms); pump();
    screen.clear(); send("\033b");
    const bool build_presets = visible("Пресеты сборки CMake", 5s)
      && visible("Без пресета сборки", 5s);
    screen.clear(); send("\033");
    std::this_thread::sleep_for(600ms); pump();
    screen.clear(); send("\033f");
    const bool project_menu = visible("Закрыть остальные", 5s);
    screen.clear(); send("\033[B"); settle(); send("\r");
    const bool project_directory = visible("Открыть проект CMake", 5s);
    screen.clear(); send("\033");
    std::this_thread::sleep_for(600ms); pump();
    screen.clear(); send("\033f");
    const bool translated_menu = visible("Закрыть остальные", 5s);
    screen.clear(); send("\033");
    (void)visible("Открытые фай", 3s);
    settle();
    screen.clear(); send("\033t");
    const bool language_menu = visible("Язык интерфейса", 5s);
    screen.clear(); send("\r");
    const bool options = visible("English", 3s);
    screen.clear(); send("\r");
    const bool switched = visible("Open files", 5s);
    std::ifstream saved_settings(settings_file);
    const std::string saved_text((std::istreambuf_iterator<char>(saved_settings)),
      std::istreambuf_iterator<char>());
    const bool persisted = saved_text.find("\"language\": \"en\"") != std::string::npos;
    screen.clear(); send("\021");
    int locale_status{};
    const auto exited = waitFor(pump, [&] {
      return ::waitpid(child, &locale_status, WNOHANG) == child;
    }, 8s);
    if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &locale_status, 0); }
    ::close(master);
    const bool success = started && new_project_menu && new_project_dialog
      && palette && new_file && search_dialog && launch_dialog
      && configure_presets && build_presets && project_menu && project_directory
      && translated_menu && language_menu && options
      && switched && persisted && exited && WIFEXITED(locale_status)
      && WEXITSTATUS(locale_status) == 0;
    if (!success) std::cerr << "Locale PTY: started=" << started
      << " new_project=" << new_project_menu << '/' << new_project_dialog
      << " palette=" << palette_title << '/' << palette_command
      << " new_file=" << new_file << " search=" << search_dialog
      << " launch=" << launch_dialog
      << " presets=" << configure_presets << '/' << build_presets
      << " project_directory=" << project_menu << '/' << project_directory
      << " menu=" << translated_menu << " language_menu=" << language_menu
      << " options=" << options << " switched=" << switched << " persisted=" << persisted
      << " exited=" << exited << " status=" << locale_status << '\n';
    return success;
  }
  if (std::getenv("TUIIDE_LAUNCH_CONFIGS_ONLY") != nullptr) {
    screen.clear(); send("\033l");
    const bool manager = visible("Run/Debug configurations", 5s)
      && visible("Second", 3s);
    screen.clear(); send("\033a");
    const bool add_prompt = visible("Add Run/Debug configuration", 3s);
    screen.clear(); send("Named\r");
    const bool add_editor = visible("Launch configuration", 3s);
    screen.clear(); send("--named"); send("\033s");
    const bool added = visible("Named", 5s) && visible("Run/Debug configurations", 3s);
    screen.clear(); send("\033c");
    const bool clone_prompt = visible("Clone Run/Debug configuration", 3s);
    screen.clear(); send("Named Copy\r");
    const bool cloned = visible("Named Copy", 5s);
    screen.clear(); send("\033d");
    const bool delete_prompt = visible("Delete Named Copy", 3s);
    screen.clear(); send("y");
    const bool deleted = visible("Run/Debug configurations", 5s);
    screen.clear(); send("\033[F"); send("\033s");
    const bool selected = waitFor(pump, [&] {
      return logged("Active Run/Debug configuration: Named");
    }, 5s);
    std::ifstream settings_input(project / ".tuiide-project.json", std::ios::binary);
    const std::string settings_text((std::istreambuf_iterator<char>(settings_input)),
      std::istreambuf_iterator<char>());
    const bool persisted = settings_text.find("\"activeLaunchConfiguration\": \"Named\"")
        != std::string::npos
      && settings_text.find("--named") != std::string::npos
      && settings_text.find("Named Copy") == std::string::npos;
    screen.clear(); send("\033L");
    const bool quick_select = visible("Select Run/Debug configuration", 5s)
      && visible("Second", 3s);
    screen.clear(); send("\033"); settle(); send("\033"); settle(); send("\021");
    int launch_status{};
    const auto exited = waitFor(pump, [&] {
      return ::waitpid(child, &launch_status, WNOHANG) == child;
    }, 8s);
    if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &launch_status, 0); }
    ::close(master);
    const bool success = started && manager && add_prompt && add_editor && added
      && clone_prompt && cloned && delete_prompt && deleted && selected && persisted
      && quick_select && exited && WIFEXITED(launch_status) && WEXITSTATUS(launch_status) == 0;
    if (!success) {
      std::cerr << "Launch configurations PTY: started=" << started << " manager=" << manager
        << " add_prompt=" << add_prompt << " add_editor=" << add_editor << " added=" << added
        << " clone_prompt=" << clone_prompt << " cloned=" << cloned
        << " delete_prompt=" << delete_prompt << " deleted=" << deleted
        << " selected=" << selected << " persisted=" << persisted
        << " quick_select=" << quick_select << " exited=" << exited
        << " status=" << launch_status << '\n';
    }
    return success;
  }
  if (std::getenv("TUIIDE_RECENT_FILES_ONLY") != nullptr) {
    screen.clear(); send("\033f");
    const bool file_menu = visible("New Project", 3s);
    screen.clear(); send("f");
    const bool recent_menu = visible("main.cpp", 3s) && visible("lear history", 3s);
    const bool unavailable_hidden = screen.find("removed.cpp") == std::string::npos;
    screen.clear(); send("\r");
    const bool opened = visible("1 file(s)", 5s);
    const auto persisted = waitFor(pump, [&] {
      std::ifstream input(settings_file, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
      return text.find("main.cpp") != std::string::npos
        && text.find("removed.cpp") == std::string::npos;
    }, 5s);
    screen.clear(); send("\033f"); (void)visible("Open Project", 3s);
    screen.clear(); send("f"); (void)visible("lear history", 3s);
    screen.clear(); send("c");
    const bool cleared = waitFor(pump, [&] { return logged("Recent Files history cleared"); }, 5s);
    screen.clear(); send("\033f"); (void)visible("Open Project", 3s);
    screen.clear(); send("f");
    const bool empty = visible("History is empty", 3s);
    screen.clear(); send("\033"); settle(); send("\033"); settle(); send("\021");
    int recent_status{};
    const auto exited = waitFor(pump, [&] {
      return ::waitpid(child, &recent_status, WNOHANG) == child;
    }, 8s);
    if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &recent_status, 0); }
    ::close(master);
    const bool success = started && file_menu && recent_menu && unavailable_hidden && opened
      && persisted && cleared && empty && exited && WIFEXITED(recent_status)
      && WEXITSTATUS(recent_status) == 0;
    if (!success) {
      std::cerr << "Recent files PTY: started=" << started << " file_menu=" << file_menu
        << " recent_menu=" << recent_menu << " unavailable_hidden=" << unavailable_hidden
        << " opened=" << opened << " persisted=" << persisted << " cleared=" << cleared
        << " empty=" << empty << " exited=" << exited << " status=" << recent_status << '\n';
    }
    return success;
  }
  if (std::getenv("TUIIDE_PANEL_REDRAW_ONLY") != nullptr) {
    constexpr std::string_view build_key{"\002"};
    screen.clear(); send(build_key);
    const bool build_streamed = waitFor(pump, [&] {
      return screen.find("Built target window_help") != std::string::npos;
    }, 45s);
    const bool build_finished = waitFor(pump, [&] {
      return screen.find("Build finished with exit code 0") != std::string::npos;
    }, 10s);
    screen.clear(); send("\033[18~");
    const bool step_into = waitFor(pump, [&] { return logged("Step into unavailable"); }, 5s);
    screen.clear(); send("\033[19~");
    const bool step_over = waitFor(pump, [&] { return logged("Next unavailable"); }, 5s);
    screen.clear(); send("\033[19;3~");
    const bool step_out = waitFor(pump, [&] { return logged("Step out unavailable"); }, 5s);
    screen.clear(); send("\033[19;5~");
    const bool next_diagnostic = waitFor(pump, [&] {
      return logged("No build, analysis, or clangd diagnostics");
    }, 5s);
    screen.clear(); send("\021");
    int panel_status{};
    const auto exited = waitFor(pump, [&] {
      return ::waitpid(child, &panel_status, WNOHANG) == child;
    }, 8s);
    if (!exited) {
      ::kill(child, SIGKILL);
      (void)::waitpid(child, &panel_status, 0);
    }
    ::close(master);
    const bool success = started && build_streamed && build_finished
      && step_into && step_over && step_out && next_diagnostic && exited
      && WIFEXITED(panel_status) && WEXITSTATUS(panel_status) == 0;
    if (!success)
      std::cerr << "Panel redraw PTY: started=" << started
        << " streamed=" << build_streamed << " finished=" << build_finished
        << " step_into=" << step_into << " step_over=" << step_over
        << " step_out=" << step_out
        << " next_diagnostic=" << next_diagnostic
        << " exited=" << exited << " status=" << panel_status << '\n';
    return success;
  }
  if (std::getenv("TUIIDE_MENU_MNEMONICS_ONLY") != nullptr) {
    const auto openMnemonicMenu = [&](std::string_view key, std::string_view marker) {
      screen.clear(); send(key);
      const bool opened = visible(marker, 3s);
      send("\033"); settle();
      return opened;
    };
    const bool mnemonic_file = openMnemonicMenu("\033f", "New Project");
    const bool mnemonic_edit = openMnemonicMenu("\033e", "Undo");
    const bool mnemonic_search = openMnemonicMenu("\033s", "Find...");
    const bool mnemonic_run = openMnemonicMenu("\033r", "Configure");
    const bool mnemonic_project = openMnemonicMenu("\033p", "Refresh tree");
    const bool mnemonic_debug = openMnemonicMenu("\033d", "Start / Continue");
    const bool mnemonic_tools = openMnemonicMenu("\033t", "Completion");
    const bool mnemonic_window = openMnemonicMenu("\033w", "Previous file");
    const bool mnemonic_help = openMnemonicMenu("\033h", "Keyboard shortcuts");
    screen.clear(); send("\033h");
    (void)visible("Keyboard shortcuts", 3s);
    send("k");
    const bool item_mnemonic = visible("F10 or Alt", 5s);
    screen.clear(); send("\r"); settle();
    screen.clear(); send("\021");
    int mnemonic_status{};
    const auto exited = waitFor(pump, [&] {
      return ::waitpid(child, &mnemonic_status, WNOHANG) == child;
    }, 8s);
    if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &mnemonic_status, 0); }
    ::close(master);
    const bool success = started && mnemonic_file && mnemonic_edit && mnemonic_search && mnemonic_run
      && mnemonic_project && mnemonic_debug && mnemonic_tools && mnemonic_window && mnemonic_help
      && item_mnemonic && exited && WIFEXITED(mnemonic_status) && WEXITSTATUS(mnemonic_status) == 0;
    if (!success) {
      std::cerr << "Menu mnemonics PTY: started=" << started << " file=" << mnemonic_file
        << " edit=" << mnemonic_edit << " search=" << mnemonic_search << " run=" << mnemonic_run
        << " project=" << mnemonic_project << " debug=" << mnemonic_debug << " tools=" << mnemonic_tools
        << " window=" << mnemonic_window << " help=" << mnemonic_help << " item=" << item_mnemonic
        << " exited=" << exited << " status=" << mnemonic_status << '\n';
    }
    return success;
  }

  const bool keyboard_menu = openTopMenu(8, "Keyboard shortcuts");
  screen.clear(); send("\r");
  const bool keyboard_dialog = visible("F10 or Alt", 5s);
  screen.clear(); send("\033");
  std::this_thread::sleep_for(600ms); pump();

  const bool about_menu = openTopMenu(8, "Keyboard shortcuts");
  screen.clear(); send("\033[A\r");
  const bool about_dialog = visible("C/C++ terminal IDE for Linux/amd64", 5s);
  screen.clear(); send("\033");
  std::this_thread::sleep_for(600ms); pump();

  screen.clear(); send("\033OP");
  const bool output_seeded = visible("Symbol information unavailable", 5s);
  const bool copy_menu = selectWindowFromEnd(7);
  settle();

  const bool filter_cancel_menu = selectWindowFromEnd(6);
  const bool filter_cancel_dialog = visible("Filter Problems", 5s);
  screen.clear(); send("\033"); settle();
  const bool filter_cancelled = waitFor(pump, [&] {
    return logged("Problems filter unchanged");
  }, 5s);

  const bool filter_normal_menu = selectWindowFromEnd(6);
  const bool filter_normal_dialog = visible("Filter Problems", 5s);
  settle();
  screen.clear(); send("zz-no-diagnostics\r");
  const bool filter_applied = waitFor(pump, [&] {
    return logged("Problems filter: zz-no-diagnostics");
  }, 5s);

  const bool clear_menu = selectWindowFromEnd(8);
  const bool cleared = waitFor(pump, [&] { return logged("Problems cleared"); }, 5s);
  const bool empty_copy_menu = selectWindowFromEnd(7);
  settle();

  bool panel_menus = true;
  for (const char mnemonic : std::string{"foudbeg"})
    panel_menus = selectWindowMnemonic(mnemonic) && panel_menus;
  const bool last_panel_protected = waitFor(pump, [&] {
    return logged("At least one sidebar panel must remain visible");
  }, 5s);
  const bool panel_restored = selectWindowMnemonic('f');

  const bool resize_menu = selectWindowFromEnd(4);
  const bool size_changed = waitFor(pump, [&] {
    const auto text = sessionText();
    const auto width = text.find("\"sidebar_width\":");
    return width != std::string::npos
      && text.find("\"sidebar_width\": 0", width) == std::string::npos;
  }, 5s);
  const bool reset_menu = selectWindowFromEnd(1);
  const bool sizes_reset = waitFor(pump, [&] {
    const auto text = sessionText();
    return logged("Panel sizes reset to automatic defaults")
      && text.find("\"sidebar_width\": 0") != std::string::npos
      && text.find("\"lower_panel_height\": 0") != std::string::npos;
  }, 5s);

  screen.clear(); send("\033f"); (void)visible("Exit", 3s); send("x");
  int status{};
  const auto exited = waitFor(pump, [&] {
    return ::waitpid(child, &status, WNOHANG) == child;
  }, 8s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);
  const bool copied = logged("Copied active lower panel");
  const bool empty_copy = logged("Copy panel unavailable: active panel is empty");

  const bool success = started && keyboard_menu && keyboard_dialog && about_menu && about_dialog
    && output_seeded && filter_cancel_menu && filter_cancel_dialog && filter_cancelled
    && filter_normal_menu && filter_normal_dialog
    && filter_applied && copy_menu && copied && clear_menu && cleared
    && empty_copy_menu && empty_copy && panel_menus && last_panel_protected
    && panel_restored && resize_menu && size_changed && reset_menu && sizes_reset
    && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::ifstream log_input(log);
    const std::string log_text((std::istreambuf_iterator<char>(log_input)),
      std::istreambuf_iterator<char>());
    std::cerr << "Window/Help PTY: started=" << started << " keyboard_menu=" << keyboard_menu
      << " keyboard_dialog=" << keyboard_dialog << " about_menu=" << about_menu
      << " about_dialog=" << about_dialog << " output_seeded=" << output_seeded
      << " filter_cancel_menu=" << filter_cancel_menu
      << " filter_cancel_dialog=" << filter_cancel_dialog << " filter_cancelled=" << filter_cancelled
      << " filter_normal_menu=" << filter_normal_menu
      << " filter_normal_dialog=" << filter_normal_dialog << " filter_applied=" << filter_applied
      << " copy_menu=" << copy_menu << " copied=" << copied << " clear_menu=" << clear_menu
      << " cleared=" << cleared << " empty_copy_menu=" << empty_copy_menu
      << " empty_copy=" << empty_copy << " panel_menus=" << panel_menus
      << " last_panel_protected=" << last_panel_protected << " panel_restored=" << panel_restored
      << " resize_menu=" << resize_menu << " size_changed=" << size_changed
      << " reset_menu=" << reset_menu << " sizes_reset=" << sizes_reset
      << " exited=" << exited << " status=" << status << "\nLog:\n" << log_text << '\n';
  }
  return success;
}

auto exerciseToolsDialogsPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace) -> bool {
  const auto project = workspace / "tools-project";
  const auto log = workspace / "tools.log";
  std::filesystem::create_directories(project);
  {
    std::ofstream file(project / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(tools_fixture LANGUAGES CXX)\n"
            "add_executable(tools_fixture main.cpp)\n";
  }
  { std::ofstream file(project / "main.cpp"); file << "int main() { return 0; }\n"; }
  {
    std::ofstream file(project / ".tuiide-project.json");
    file << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":1}\n";
  }
  const auto config = workspace / "tools-config";
  const auto settings_file = config / "tuiide/settings.json";
  std::filesystem::create_directories(settings_file.parent_path());
  {
    std::ofstream file(settings_file);
    if (std::getenv("TUIIDE_TOOLS_LOCALE_ONLY") != nullptr)
      file << "{\"version\":1,\"language\":\"ru\"}\n";
    else
      file << "{\"version\":1,\"theme\":\"Dark\",\"shortcuts\":{\"file.open\":\"Ctrl+U\","
        "\"debug.watch\":\"Ctrl+L\"}}\n";
  }
  const auto readFile = [](const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  };

  int master{-1};
  winsize window{26, 90, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--log-file", log.c_str(), project.c_str(),
      static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const auto settle = [&] {
    std::this_thread::sleep_for(200ms);
    pump();
  };
  const auto logged = [&](std::string_view value) {
    std::ifstream input(log);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find(value) != std::string::npos;
  };
  const auto openToolsFromEnd = [&](int up_count) {
    screen.clear(); send("\033[21~");
    (void)visible("File", 3s);
    send("\r");
    (void)visible("Open Project", 3s);
    for (int menu = 0; menu < 6; ++menu) {
      send("\033[C");
      std::this_thread::sleep_for(40ms);
      pump();
    }
    const bool tools = visible("Completion", 3s);
    screen.clear();
    for (int index = 0; index < up_count; ++index) send("\033[A");
    send("\r");
    return tools;
  };

  const bool started = visible(std::getenv("TUIIDE_TOOLS_LOCALE_ONLY") != nullptr
    ? "Открытые файлы" : "Open files");
  if (std::getenv("TUIIDE_TOOLS_LOCALE_ONLY") != nullptr) {
    const auto openFromEnd = [&](int up_count) {
      screen.clear(); send("\033t");
      const bool menu = visible("Язык интерфейса", 3s);
      send("\033[F");
      for (int index = 0; index < up_count; ++index) send("\033[A");
      send("\r");
      return menu;
    };
    const bool theme_menu = openFromEnd(3);
    const bool theme = visible("Тема редактора", 5s) && visible("Контрастная", 5s);
    send("\033[H"); settle(); screen.clear(); send("\033[B");
    const bool theme_selection = visible("Светлая", 5s);
    send("\r");
    const bool theme_identifier = waitFor(pump, [&] {
      return readFile(settings_file).find("\"theme\": \"Light\"") != std::string::npos;
    }, 5s);
    const bool colors_menu = openFromEnd(2);
    const bool colors = visible("Цвета редактора", 5s) && visible("основной текст", 5s);
    screen.clear(); send("\033"); settle();
    const bool shortcuts_menu = openFromEnd(5);
    const bool shortcuts = visible("Настройка сочетаний", 5s)
      && visible("Файл: Создать", 5s);
    screen.clear(); send("\033"); settle();
    screen.clear(); send("\021");
    int status{};
    const auto exited = waitFor(pump, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 8s);
    if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
    ::close(master);
    const bool success = started && theme_menu && theme && theme_selection && theme_identifier
      && colors_menu && colors
      && shortcuts_menu && shortcuts && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!success) std::cerr << "Tools locale PTY: started=" << started
      << " theme=" << theme_menu << '/' << theme << '/' << theme_selection
      << '/' << theme_identifier << " colors=" << colors_menu << '/' << colors
      << " shortcuts=" << shortcuts_menu << '/' << shortcuts
      << " exited=" << exited << " status=" << status << '\n';
    return success;
  }
  screen.clear(); send("\033[6;3~");
  const bool sidebar_shortcut = waitFor(pump, [&] { return logged("Sidebar tab: Project"); }, 5s);
  screen.clear(); send("\033[6;4~");
  const bool lower_shortcut = waitFor(pump, [&] { return logged("Lower panel tab: Problems"); }, 5s);

  const bool conflicts_menu = openToolsFromEnd(4);
  const bool conflicts_dialog = visible("Effective shortcut table", 5s);
  screen.clear(); send("\033"); settle();

  const auto initial_settings = readFile(settings_file);
  const bool legacy_shortcut_warning = logged("Ignored shortcut debug.watch: Ctrl+L is reserved by Final Cut");
  const bool shortcut_cancel_menu = openToolsFromEnd(5);
  const bool shortcut_cancel_dialog = visible("Configure shortcuts", 5s)
    && visible("Default: Ctrl+N", 5s) && visible("Capture", 5s);
  screen.clear(); send("\033"); settle();
  const bool shortcut_cancelled = readFile(settings_file) == initial_settings;

  const bool shortcut_error_menu = openToolsFromEnd(5);
  const bool shortcut_error_picker = visible("File: New", 5s);
  screen.clear(); send("\033c"); settle(); screen.clear();
  send(std::string(1, static_cast<char>(2)));
  const bool conflict_captured = visible("Ctrl+B", 5s);
  const bool shortcut_error = conflict_captured && readFile(settings_file) == initial_settings;
  screen.clear(); send("\033"); settle();
  const bool shortcut_error_safe = readFile(settings_file) == initial_settings;

  (void)openToolsFromEnd(5);
  (void)visible("Configure shortcuts", 5s);
  // ESC-prefix обрабатывается с задержкой: ждём отмены Capture до ввода текста.
  screen.clear(); send("\033c");
  const bool reserved_capture_started = visible("Press the desired key now", 5s);
  screen.clear(); send("\033");
  const bool reserved_capture_cancelled = visible("Capture cancelled.", 5s);
  screen.clear(); send(std::string(120, '\177')); send("Ctrl+L");
  const bool shortcut_reserved = visible("reserved by Final Cut", 5s);
  send("\023"); settle();
  const bool shortcut_reserved_safe = readFile(settings_file) == initial_settings
    && screen.find("reserved by Final Cut") != std::string::npos;
  send("\033"); settle();

  const bool shortcut_normal_menu = openToolsFromEnd(5);
  const bool shortcut_normal_picker = visible("File: New", 5s);
  screen.clear(); send("\033c"); settle(); screen.clear();
  send(std::string(1, static_cast<char>(18)));
  const bool shortcut_capture = visible("Ctrl+R", 5s);
  send("\r");
  const bool shortcut_saved = waitFor(pump, [&] {
    return logged("Shortcut settings updated");
  }, 5s);
  const bool shortcut_persisted = readFile(settings_file).find("\"file.new\": \"Ctrl+R\"")
    != std::string::npos;
  screen.clear(); send(std::string(1, static_cast<char>(18)));
  const bool shortcut_active = visible("1 file(s)", 5s);
  screen.clear(); send(std::string(1, static_cast<char>(23))); settle();

  if (std::getenv("TUIIDE_SHORTCUTS_ONLY") != nullptr) {
    screen.clear(); send("\021");
    int status{};
    const auto exited = waitFor(pump, [&] {
      return ::waitpid(child, &status, WNOHANG) == child;
    }, 8s);
    if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
    ::close(master);
    const bool success = started && conflicts_dialog
      && legacy_shortcut_warning && reserved_capture_started && reserved_capture_cancelled
      && shortcut_reserved && shortcut_reserved_safe
      && shortcut_cancelled && shortcut_error_picker && shortcut_error
      && shortcut_error_safe && shortcut_normal_picker && shortcut_capture
      && shortcut_saved && shortcut_persisted && shortcut_active && exited
      && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!success) {
      std::cerr << "Shortcut PTY: started=" << started << " conflicts=" << conflicts_dialog
        << " legacy=" << legacy_shortcut_warning << " reserved=" << shortcut_reserved
        << "/" << shortcut_reserved_safe
        << " capture-state=" << reserved_capture_started << "/" << reserved_capture_cancelled
        << " cancel_dialog=" << shortcut_cancel_dialog << " cancelled=" << shortcut_cancelled
        << " error_picker=" << shortcut_error_picker << " conflict=" << shortcut_error
        << " error_safe=" << shortcut_error_safe << " normal_picker=" << shortcut_normal_picker
        << " capture=" << shortcut_capture << " saved=" << shortcut_saved
        << " persisted=" << shortcut_persisted << " active=" << shortcut_active
        << " exited=" << exited << " status=" << status << '\n';
    }
    return success;
  }

  const auto before_theme = readFile(settings_file);
  const bool theme_cancel_menu = openToolsFromEnd(3);
  const bool theme_cancel_dialog = visible("High contrast", 5s);
  screen.clear(); send("\033"); settle();
  const bool theme_cancelled = readFile(settings_file) == before_theme;

  const bool theme_normal_menu = openToolsFromEnd(3);
  const bool theme_normal_dialog = visible("High contrast", 5s);
  // Сначала приводим список к известной строке и ждём отрисовки выбора Light.
  send("\033[H"); settle(); screen.clear(); send("\033[B");
  const bool theme_selection_visible = visible("Light", 5s);
  send("\r");
  const bool theme_saved = theme_selection_visible
    && waitFor(pump, [&] { return logged("Editor theme: Light"); }, 5s);
  const bool theme_persisted = readFile(settings_file).find("\"theme\": \"Light\"")
    != std::string::npos;

  const auto before_color = readFile(settings_file);
  const bool color_cancel_menu = openToolsFromEnd(2);
  const bool color_cancel_dialog = visible("foreground", 5s);
  screen.clear(); send("\033"); settle();
  const bool color_cancelled = readFile(settings_file) == before_color;

  const bool color_normal_menu = openToolsFromEnd(2);
  const bool color_role_dialog = visible("foreground", 5s);
  settle(); screen.clear(); send("\r");
  const bool color_value_dialog = visible("Theme default", 5s);
  settle(); screen.clear(); send("\033[B\r"); settle(); send(std::string(1, static_cast<char>(19)));
  const bool color_saved = waitFor(pump, [&] {
    return logged("Editor colors updated");
  }, 5s);
  const bool color_persisted = readFile(settings_file).find("\"foreground\": \"Black\"")
    != std::string::npos;

  screen.clear(); send("\033f"); (void)visible("Close Pro", 3s); send("j");
  const bool project_closed = waitFor(pump, [&] {
    return logged("Previous project state cleared");
  }, 5s);
  screen.clear(); send("\033f"); (void)visible("Exit", 3s); send("x");
  int status{};
  const auto exited = waitFor(pump, [&] {
    return ::waitpid(child, &status, WNOHANG) == child;
  }, 8s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);

  (void)conflicts_menu;
  (void)shortcut_cancel_menu;
  (void)shortcut_error_menu;
  (void)shortcut_normal_menu;
  (void)theme_cancel_menu;
  (void)theme_normal_menu;
  (void)color_cancel_menu;
  (void)color_normal_menu;
  (void)shortcut_cancel_dialog;
  (void)shortcut_error_picker;
  (void)shortcut_normal_picker;
  (void)theme_cancel_dialog;
  (void)theme_normal_dialog;
  (void)color_cancel_dialog;

  const bool success = started && sidebar_shortcut && lower_shortcut && conflicts_dialog
    && legacy_shortcut_warning && reserved_capture_started && reserved_capture_cancelled
    && shortcut_reserved && shortcut_reserved_safe
    && shortcut_cancelled
    && shortcut_error && shortcut_error_safe
    && shortcut_capture && shortcut_saved && shortcut_persisted && shortcut_active
    && theme_cancelled
    && theme_saved && theme_persisted
    && color_cancelled
    && color_role_dialog && color_value_dialog && color_saved && color_persisted
    && project_closed
    && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::cerr << "Tools PTY: started=" << started << " sidebar_shortcut=" << sidebar_shortcut
      << " lower_shortcut=" << lower_shortcut << " conflicts=" << conflicts_dialog
      << " shortcut_cancel=" << shortcut_cancelled << " shortcut_error=" << shortcut_error
      << " shortcut_error_safe=" << shortcut_error_safe << " shortcut_saved=" << shortcut_saved
      << " shortcut_capture=" << shortcut_capture << " shortcut_persisted=" << shortcut_persisted
      << " shortcut_active=" << shortcut_active
      << " reserved=" << reserved_capture_started << "/" << reserved_capture_cancelled
      << "/" << shortcut_reserved << "/" << shortcut_reserved_safe
      << " theme_selection=" << theme_selection_visible
      << " theme_cancel=" << theme_cancelled << " theme_saved=" << theme_saved
      << " theme_persisted=" << theme_persisted << " color_cancel=" << color_cancelled
      << " color_role=" << color_role_dialog << " color_value=" << color_value_dialog
      << " color_saved=" << color_saved << " color_persisted=" << color_persisted
      << " project_closed=" << project_closed
      << " exited=" << exited << " status=" << status << "\nLog:\n" << readFile(log) << '\n';
  }
  return success;
}

auto exerciseSearchDialogsPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace) -> bool {
  const auto project = workspace / "search-project";
  const auto log = workspace / "search.log";
  std::filesystem::create_directories(project);
  {
    std::ofstream file(project / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(search_fixture LANGUAGES CXX)\n"
            "add_executable(search_fixture main.cpp secondary.cpp)\n";
  }
  {
    std::ofstream file(project / "main.cpp");
    file << "int needle = 1;\nint first = needle;\n// TARGET_LINE_THREE\n"
            "int second = needle;\nint main() { return first + second; }\n";
  }
  { std::ofstream file(project / "secondary.cpp"); file << "int secondary_needle = 7;\n"; }
  {
    std::ofstream file(project / ".tuiide-project.json");
    file << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":1}\n";
  }
  const auto original_main = [&] {
    std::ifstream input(project / "main.cpp", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  }();

  int master{-1};
  winsize window{26, 90, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
    const auto config = workspace / "search-config";
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--log-file", log.c_str(), project.c_str(),
      static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const auto settle = [&] {
    std::this_thread::sleep_for(200ms);
    pump();
  };
  const auto logged = [&](std::string_view value) {
    std::ifstream input(log);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find(value) != std::string::npos;
  };
  const auto selectSearchFromStart = [&](int down_count) {
    screen.clear(); send("\033[21~");
    (void)visible("File", 3s);
    send("\r");
    (void)visible("Open Project", 3s);
    for (int menu = 0; menu < 2; ++menu) {
      send("\033[C");
      std::this_thread::sleep_for(40ms);
      pump();
    }
    const bool search_menu = visible("Find next", 3s);
    screen.clear();
    for (int index = 0; index < down_count; ++index) send("\033[B");
    send("\r");
    return search_menu;
  };
  const auto clear_field = std::string(100, '\177');

  const bool started = visible("Open files");
  screen.clear(); send(std::string(1, static_cast<char>(15)));
  const bool open_dialog = visible("main.cpp", 5s);
  screen.clear(); send("\177main.cpp\r");
  const bool opened = visible("TARGET_LINE_THREE", 5s);

  screen.clear(); send(std::string(1, static_cast<char>(6)));
  const bool find_cancel_dialog = visible("Find and replace", 5s);
  screen.clear(); send("\033c"); settle();

  screen.clear(); send(std::string(1, static_cast<char>(6)));
  const bool find_empty_dialog = visible("Find and replace", 5s);
  screen.clear(); send("\r");
  const bool find_empty_error = waitFor(pump, [&] {
    return logged("Find error: search text is empty");
  }, 5s);

  screen.clear(); send(std::string(1, static_cast<char>(6)));
  const bool find_normal_dialog = visible("Find and replace", 5s);
  settle(); screen.clear(); send("needle\r");
  const bool find_normal = waitFor(pump, [&] { return logged("Find: match 1 of 3"); }, 5s);
  const bool next_menu = selectSearchFromStart(1);
  const bool find_next = waitFor(pump, [&] { return logged("Find: match 2 of 3"); }, 5s);
  const bool previous_menu = selectSearchFromStart(2);
  const bool find_previous = waitFor(pump, [&] {
    std::ifstream input(log);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto first = text.find("Find: match 1 of 3");
    return first != std::string::npos && text.find("Find: match 1 of 3", first + 1) != std::string::npos;
  }, 5s);

  screen.clear(); send(std::string(1, static_cast<char>(7)));
  const bool goto_cancel_dialog = visible("Line number:", 5s);
  screen.clear(); send("\033c"); settle();
  screen.clear(); send(std::string(1, static_cast<char>(7)));
  const bool goto_error_dialog = visible("Line number:", 5s);
  screen.clear(); send("0\r");
  const bool goto_range_error = waitFor(pump, [&] {
    return logged("Go to line error: enter a value from 1 to");
  }, 5s);
  screen.clear(); send(std::string(1, static_cast<char>(7)));
  const bool goto_invalid_dialog = visible("Line number:", 5s);
  screen.clear(); send("abc\r");
  const bool goto_invalid = waitFor(pump, [&] { return logged("Invalid line number"); }, 5s);
  screen.clear(); send(std::string(1, static_cast<char>(7)));
  const bool goto_normal_dialog = visible("Line number:", 5s);
  screen.clear(); send("3\r");
  const bool goto_normal = waitFor(pump, [&] { return logged("Go to line: 3"); }, 5s);

  screen.clear(); send(std::string(1, static_cast<char>(6)));
  const bool return_to_match_dialog = visible("Find and replace", 5s);
  settle(); screen.clear(); send("\r"); settle();

  screen.clear(); send(std::string(1, static_cast<char>(6)));
  const bool replace_dialog = visible("Find and replace", 5s);
  settle(); send("\t"); settle(); send("token");
  screen.clear(); send("\033r");
  const bool replaced_one = waitFor(pump, [&] { return logged("Replace: changed 1 match"); }, 5s);
  screen.clear(); send(std::string(1, static_cast<char>(6)));
  const bool replace_all_dialog = visible("Find and replace", 5s);
  screen.clear(); send("\033a");
  const bool replaced_all = waitFor(pump, [&] {
    return logged("Replace all: changed 2 match(es) in the active file");
  }, 5s);

  const bool project_empty_menu = selectSearchFromStart(4);
  const bool project_empty_dialog = visible("Find and replace", 5s);
  settle(); screen.clear(); send(clear_field); send("not_in_project"); send("\033f");
  const bool project_no_match = waitFor(pump, [&] {
    return logged("Project search: no matches for not_in_project");
  }, 8s);

  const bool project_cancel_menu = selectSearchFromStart(4);
  const bool project_cancel_dialog = visible("Find and replace", 5s);
  settle(); screen.clear(); send(clear_field); send("secondary_needle"); send("\033f");
  const bool project_results = visible("Project search", 8s)
    && screen.find("secondary.cpp") != std::string::npos;
  screen.clear(); send("\033c"); settle();

  screen.clear(); send(std::string(1, static_cast<char>(11)));  // Ctrl+K
  const bool no_problems = waitFor(pump, [&] {
    return logged("No build, analysis, or clangd diagnostics");
  }, 5s);

  screen.clear(); send(std::string(1, static_cast<char>(23)));
  const bool unsaved_prompt = visible("Unsaved changes", 5s);
  screen.clear(); send("n"); settle();
  const bool disk_unchanged = [&] {
    std::ifstream input(project / "main.cpp", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text == original_main;
  }();

  screen.clear(); send("\033f"); (void)visible("Exit", 3s); send("x");
  int status{};
  const auto exited = waitFor(pump, [&] {
    return ::waitpid(child, &status, WNOHANG) == child;
  }, 8s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);

  (void)next_menu;
  (void)previous_menu;
  (void)project_empty_menu;
  (void)project_cancel_menu;
  const bool success = started && open_dialog && opened
    && find_cancel_dialog && find_empty_dialog && find_empty_error
    && find_normal_dialog && find_normal && find_next && find_previous
    && goto_cancel_dialog && goto_error_dialog && goto_range_error
    && goto_invalid_dialog && goto_invalid && goto_normal_dialog && goto_normal
    && return_to_match_dialog && replace_dialog && replaced_one && replace_all_dialog && replaced_all
    && project_empty_dialog && project_no_match && project_cancel_dialog && project_results
    && no_problems && unsaved_prompt && disk_unchanged
    && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::cerr << "Search PTY: started=" << started << " open=" << opened
      << " find_cancel=" << find_cancel_dialog << " find_empty=" << find_empty_error
      << " find_normal=" << find_normal << " next=" << find_next << " previous=" << find_previous
      << " goto_cancel=" << goto_cancel_dialog << " goto_range=" << goto_range_error
      << " goto_invalid=" << goto_invalid << " goto_normal=" << goto_normal
      << " project_empty=" << project_no_match << " project_results=" << project_results
      << " replace_one=" << replaced_one << " replace_all=" << replaced_all
      << " no_problems=" << no_problems << " unsaved=" << unsaved_prompt
      << " disk_unchanged=" << disk_unchanged << " exited=" << exited << " status=" << status << '\n';
  }
  return success;
}

auto exerciseLspDialogsPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& fake_server, const std::filesystem::path& workspace) -> bool {
  const auto project = workspace / "lsp-dialogs-project";
  const auto fake_bin = workspace / "lsp-dialogs-bin";
  const auto log = workspace / "lsp-dialogs.log";
  std::filesystem::create_directories(project);
  std::filesystem::create_directories(fake_bin);
  std::error_code link_error;
  std::filesystem::create_symlink(fake_server, fake_bin / "clangd", link_error);
  if (link_error) return false;
  {
    std::ofstream file(project / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(lsp_dialogs LANGUAGES CXX)\n"
            "add_executable(lsp_dialogs main.cpp)\n";
  }
  { std::ofstream file(project / "main.cpp"); file << "matrix\nempty\nfailure\n"; }
  {
    std::ofstream file(project / ".tuiide-project.json");
    file << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":1}\n";
  }
  const auto original_source = [&] {
    std::ifstream input(project / "main.cpp", std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  }();

  int master{-1};
  winsize window{26, 90, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
    const auto config = workspace / "lsp-dialogs-config";
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    const auto old_path = std::getenv("PATH");
    const auto path = fake_bin.string() + (old_path ? ":" + std::string(old_path) : std::string{});
    ::setenv("PATH", path.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--log-file", log.c_str(), project.c_str(),
      static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const auto logged = [&](std::string_view value) {
    std::ifstream input(log);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find(value) != std::string::npos;
  };
  const auto settle = [&] { std::this_thread::sleep_for(180ms); pump(); };
  const auto goToLine = [&](int line) {
    screen.clear(); send(std::string(1, static_cast<char>(7)));
    const bool dialog = visible("Line number:", 5s);
    screen.clear(); send(std::to_string(line)); send("\r");
    settle();
    return dialog;
  };
  const auto requestFeedback = [&](int line, std::string_view key, std::string_view expected) {
    const bool moved = goToLine(line);
    screen.clear(); send(key);
    return moved && waitFor(pump, [&] { return logged(expected); }, 5s);
  };
  const auto selectToolsMnemonic = [&](char mnemonic) {
    screen.clear(); send("\033t");
    const bool tools = visible("Completion", 3s);
    screen.clear(); send(std::string(1, mnemonic));
    return tools;
  };
  const auto requestToolFeedback = [&](int line, char mnemonic, std::string_view expected) {
    const bool moved = goToLine(line);
    (void)selectToolsMnemonic(mnemonic);
    return moved && waitFor(pump, [&] { return logged(expected); }, 5s);
  };

  const bool started = visible("Open files");
  screen.clear(); send(std::string(1, static_cast<char>(15)));
  const bool open_dialog = visible("main.cpp", 5s);
  screen.clear(); send("\177main.cpp\r");
  const bool opened = visible("1 file(s)", 5s);
  settle();

  const bool hover_position = goToLine(1);
  screen.clear(); send("\033OP");
  const bool hover_dialog = visible("matrix hover", 5s)
    && screen.find("Symbol information") != std::string::npos;
  screen.clear(); send("\033");
  std::this_thread::sleep_for(800ms);
  pump();
  const bool hover_empty = requestFeedback(2, "\033OP", "Symbol information: no symbol information");
  const bool hover_error = requestFeedback(3, "\033OP", "Symbol information error: forced operation failure");

  const bool definition_position = goToLine(1);
  screen.clear(); send("\033[F"); settle(); screen.clear(); send("\033OR");
  settle();
  const bool definition_normal = definition_position;
  const bool definition_empty = requestFeedback(2, "\033OR", "Go to definition: definition not found");
  const bool definition_error = requestFeedback(3, "\033OR", "Go to definition error: forced operation failure");

  const bool references_position = goToLine(1);
  screen.clear(); send("\033OS");
  const bool references_dialog = visible("References", 5s)
    && screen.find("main.cpp:1:1") != std::string::npos;
  screen.clear(); send("\033c"); settle();
  const bool references_empty = requestFeedback(2, "\033OS", "Find references: no references found");
  const bool references_error = requestFeedback(3, "\033OS", "Find references error: forced operation failure");

  const bool rename_position = goToLine(1);
  screen.clear(); send("\033OQ");
  const bool rename_prompt = visible("New name:", 5s);
  screen.clear(); send("renamed\r");
  const bool rename_preview = visible("Rename preview", 5s)
    && screen.find("`matrix` -> `renamed`") != std::string::npos;
  screen.clear(); send("\033c");
  const bool rename_cancelled = waitFor(pump, [&] { return logged("Rename cancelled; no files changed"); }, 5s);
  const bool rename_empty_position = goToLine(2);
  screen.clear(); send("\033OQ"); (void)visible("New name:", 5s);
  screen.clear(); send("renamed\r");
  const bool rename_empty = waitFor(pump, [&] { return logged("Rename: rename produced no edits"); }, 5s);
  const bool rename_error_position = goToLine(3);
  screen.clear(); send("\033OQ"); (void)visible("New name:", 5s);
  screen.clear(); send("renamed\r");
  const bool rename_error = waitFor(pump, [&] { return logged("Rename error: forced operation failure"); }, 5s);

  const bool action_position = goToLine(1);
  screen.clear(); send("\033a");
  const bool action_picker = visible("Matrix quick fix", 5s);
  screen.clear(); send("\r");
  const bool action_preview = visible("Matrix quick fix preview", 5s)
    && screen.find("`matrix` -> `fixed`") != std::string::npos;
  screen.clear(); send("\033c");
  const bool action_cancelled = waitFor(pump, [&] { return logged("Matrix quick fix cancelled; no files changed"); }, 5s);
  const bool action_empty = requestFeedback(2, "\033a", "Code Actions: no applicable code actions");
  const bool action_error = requestFeedback(3, "\033a", "Code Actions error: forced operation failure");

  const bool signature_position = goToLine(1);
  (void)selectToolsMnemonic('h');
  const bool signature_dialog = visible("Signature help", 5s)
    && screen.find("int matrix(int value)") != std::string::npos
    && screen.find("matrix signature") != std::string::npos;
  screen.clear(); send("\033"); std::this_thread::sleep_for(800ms); pump();
  const bool signature_empty = requestToolFeedback(2, 'h',
    "Signature help: no signature information");
  const bool signature_error = requestToolFeedback(3, 'h',
    "Signature help error: forced operation failure");

  const auto requestWorkspaceSymbols = [&](std::string_view query, std::string_view expected) {
    (void)selectToolsMnemonic('w');
    const bool prompt = visible("Name or substring:", 5s);
    screen.clear(); send(query); send("\r");
    return prompt && (expected.empty() || waitFor(pump, [&] { return logged(expected); }, 5s));
  };
  const bool workspace_prompt = requestWorkspaceSymbols("matrix", {});
  const bool workspace_dialog = visible("Workspace Symbols", 5s)
    && screen.find("matrixSymbol") != std::string::npos;
  screen.clear(); send("\033c"); settle();
  const bool workspace_empty = requestWorkspaceSymbols("empty",
    "Workspace Symbols: no matching workspace symbols");
  const bool workspace_error = requestWorkspaceSymbols("failure",
    "Workspace Symbols error: forced operation failure");

  const bool call_position = goToLine(1);
  (void)selectToolsMnemonic('l');
  const bool call_dialog = visible("Call Hierarchy", 5s)
    && screen.find("matrixRoot") != std::string::npos
    && screen.find("matrixCaller") != std::string::npos
    && screen.find("matrixCallee") != std::string::npos;
  screen.clear(); send("\033c"); settle();
  const bool call_empty = requestToolFeedback(2, 'l',
    "Call Hierarchy: call hierarchy is unavailable at the cursor");
  const bool call_error = requestToolFeedback(3, 'l',
    "Call Hierarchy error: forced operation failure");

  const bool type_position = goToLine(1);
  (void)selectToolsMnemonic('y');
  const bool type_dialog = visible("Type Hierarchy", 5s)
    && screen.find("MatrixType") != std::string::npos
    && screen.find("MatrixBase") != std::string::npos
    && screen.find("MatrixDerived") != std::string::npos;
  screen.clear(); send("\033c"); settle();
  const bool type_empty = requestToolFeedback(2, 'y',
    "Type Hierarchy: type hierarchy is unavailable at the cursor");
  const bool type_error = requestToolFeedback(3, 'y',
    "Type Hierarchy error: forced operation failure");

  const bool source_unchanged = [&] {
    std::ifstream input(project / "main.cpp", std::ios::binary);
    const std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return value == original_source;
  }();
  screen.clear(); send(std::string(1, static_cast<char>(4)));
  int status{};
  const auto exited = waitFor(pump, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 8s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);

  const bool success = started && open_dialog && opened
    && hover_position && hover_dialog && hover_empty && hover_error
    && definition_position && definition_normal && definition_empty && definition_error
    && references_position && references_dialog && references_empty && references_error
    && rename_position && rename_prompt && rename_preview && rename_cancelled
    && rename_empty_position && rename_empty && rename_error_position && rename_error
    && action_position && action_picker && action_preview && action_cancelled
    && action_empty && action_error
    && signature_position && signature_dialog && signature_empty && signature_error
    && workspace_prompt && workspace_dialog && workspace_empty && workspace_error
    && call_position && call_dialog && call_empty && call_error
    && type_position && type_dialog && type_empty && type_error
    && source_unchanged
    && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::ifstream log_input(log);
    const std::string log_text((std::istreambuf_iterator<char>(log_input)), std::istreambuf_iterator<char>());
    std::cerr << "LSP dialogs PTY: started=" << started << " open=" << opened
      << " hover=" << hover_dialog << "/" << hover_empty << "/" << hover_error
      << " definition=" << definition_normal << "/" << definition_empty << "/" << definition_error
      << " references=" << references_dialog << "/" << references_empty << "/" << references_error
      << " rename=" << rename_prompt << "/" << rename_preview << "/" << rename_cancelled
      << "/" << rename_empty << "/" << rename_error
      << " action=" << action_picker << "/" << action_preview << "/" << action_cancelled
      << "/" << action_empty << "/" << action_error
      << " signature=" << signature_dialog << "/" << signature_empty << "/" << signature_error
      << " workspace=" << workspace_dialog << "/" << workspace_empty << "/" << workspace_error
      << " call=" << call_dialog << "/" << call_empty << "/" << call_error
      << " type=" << type_dialog << "/" << type_empty << "/" << type_error
      << " unchanged=" << source_unchanged
      << " exited=" << exited << " status=" << status << "\nLog:\n" << log_text << '\n';
  }
  return success;
}

auto exerciseDebugDialogsPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace) -> bool {
  const auto project = workspace / "debug-dialogs-project";
  const auto build = project / "build";
  const auto log = workspace / "debug-dialogs.log";
  const auto session = build / ".tuiide-session.json";
  std::filesystem::create_directories(project);
  {
    std::ofstream file(project / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(debug_dialogs LANGUAGES CXX)\n"
            "add_executable(debug_fixture main.cpp)\n";
  }
  {
    std::ofstream file(project / "main.cpp");
    file << "int main() {\n  int value = 41;\n  value += 1;\n  return value == 42 ? 0 : 1;\n}\n";
  }
  {
    std::ofstream file(project / ".tuiide-project.json");
    file << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":1,"
            "\"launch\":{\"executable\":\"build/debug_fixture\","
            "\"workingDirectory\":\".\",\"preLaunchBuild\":false}}\n";
  }
  std::string build_output;
  if (!runProcess({"cmake", "-S", project.string(), "-B", build.string(),
      "-DCMAKE_BUILD_TYPE=Debug"}, project, build_output)
      || !runProcess({"cmake", "--build", build.string(), "--parallel", "1"},
        project, build_output)) {
    std::cerr << "Debug dialogs fixture build failed:\n" << build_output << '\n';
    return false;
  }

  int master{-1};
  winsize window{28, 100, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
    const auto config = workspace / "debug-dialogs-config";
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--log-file", log.c_str(), project.c_str(),
      static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const auto logged = [&](std::string_view value) {
    std::ifstream input(log);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find(value) != std::string::npos;
  };
  const auto readSession = [&] {
    std::ifstream input(session);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  };
  const auto settle = [&] { std::this_thread::sleep_for(250ms); pump(); };
  const auto closeTextDialog = [&] {
    screen.clear(); send("\033"); std::this_thread::sleep_for(800ms); pump();
  };
  const auto goToLine = [&](int line) {
    screen.clear(); send(std::string(1, static_cast<char>(7)));
    const bool dialog = visible("Line number:", 5s);
    screen.clear(); send(std::to_string(line)); send("\r"); settle();
    return dialog;
  };
  const auto openTopMenu = [&](int index, std::string_view marker) {
    if (index != 5 && index != 7) return false;
    screen.clear(); send(index == 5 ? "\033d" : "\033w");
    return visible(marker, 3s);
  };
  const auto debugCommand = [&](char mnemonic) {
    const bool menu = openTopMenu(5, "Start / Continue");
    screen.clear(); send(std::string(1, mnemonic));
    return menu;
  };
  const auto toggleBreakpointsPanel = [&] {
    const bool menu = openTopMenu(7, "Show Open files");
    screen.clear(); send("b"); settle();
    return menu;
  };
  const auto clear_field = std::string(120, '\177');

  const bool started = visible("Open files");
  screen.clear(); send(std::string(1, static_cast<char>(15)));
  const bool open_dialog = visible("main.cpp", 5s);
  screen.clear(); send("\177main.cpp\r");
  const bool opened = visible("1 file(s)", 5s);

  // Ctrl+L должен только перерисовывать экран, новая клавиша открывает watch.
  screen.clear(); send("\014"); settle();
  const bool redraw_only = screen.find("Add watch") == std::string::npos;
  screen.clear(); send("\033U");
  const bool watch_cancel_prompt = visible("Add watch", 5s) && screen.find("Expression:") != std::string::npos;
  closeTextDialog();
  const auto before_watch = readSession();
  screen.clear(); send("\033U"); (void)visible("Expression:", 5s);
  screen.clear(); send("value\r");
  const bool watch_added = waitFor(pump, [&] {
    return readSession().find("\"value\"") != std::string::npos;
  }, 5s);
  screen.clear(); (void)debugCommand('w'); (void)visible("Expression:", 5s);
  screen.clear(); send("value\r");
  const bool watch_duplicate = waitFor(pump, [&] {
    return logged("Watch already exists or is empty: value");
  }, 5s);
  const bool watch_cancelled = before_watch.empty() || before_watch.find("\"value\"") == std::string::npos;

  const bool breakpoint_line = goToLine(3);
  screen.clear(); send("\033[20~");
  const bool breakpoint_set = waitFor(pump, [&] { return logged("Breakpoint set:"); }, 5s);
  const bool panel_hidden = toggleBreakpointsPanel();
  const bool panel_focused = toggleBreakpointsPanel() && visible("main.cpp:3", 5s);

  const auto before_properties = readSession();
  screen.clear(); send("\033OQ");
  const bool properties_cancel_dialog = visible("Breakpoint properties", 5s);
  closeTextDialog();
  const bool properties_cancelled = readSession() == before_properties;

  screen.clear(); send("\033OQ");
  const bool properties_error_dialog = visible("Ignore first hits:", 5s);
  screen.clear(); send("\t"); send(clear_field); send("bad"); send("\033s");
  const bool properties_error = visible("Ignore hit count must be an integer", 5s);
  screen.clear(); send("\r"); settle();
  const bool properties_error_safe = readSession() == before_properties;

  screen.clear(); send("\033OQ");
  const bool properties_normal_dialog = visible("Breakpoint properties", 5s);
  screen.clear(); send("value == 41"); send("\033s");
  const bool properties_saved = waitFor(pump, [&] {
    return readSession().find("\"condition\": \"value == 41\"") != std::string::npos;
  }, 5s);

  screen.clear(); send("\033[15~");
  const bool debug_started = waitFor(pump, [&] { return logged("GDB:"); }, 8s);
  const bool debug_stopped = waitFor(pump, [&] { return logged("Breakpoint 1, main"); }, 20s);
  const bool execution_marker = visible("▶", 5s);

  const bool evaluate_cancel_menu = debugCommand('e');
  const bool evaluate_cancel_prompt = visible("Evaluate expression", 5s);
  closeTextDialog();
  const bool evaluate_normal_menu = debugCommand('e');
  const bool evaluate_normal_prompt = visible("Expression:", 5s);
  screen.clear(); send("value + 1\r");
  const bool evaluation_result = visible("value + 1 = 42", 8s);
  closeTextDialog();
  const bool evaluate_error_menu = debugCommand('e');
  (void)visible("Expression:", 5s); screen.clear(); send("missing_name\r");
  const bool evaluation_error = visible("Evaluation error", 8s)
    && screen.find("missing_name") != std::string::npos;
  closeTextDialog();

  const bool assignment_menu = debugCommand('v');
  const bool assignment_expression = visible("Expression:", 5s);
  screen.clear(); send("value\r");
  const bool assignment_value = visible("New value", 5s);
  screen.clear(); send("100\r");
  const bool assignment_result = visible("value = 100", 8s);
  closeTextDialog();

  const bool disassembly_menu = debugCommand('y');
  const bool disassembly_prompt = visible("Address/expression ($pc):", 5s);
  screen.clear(); send("$pc\r");
  const bool disassembly_result = visible("Disassembly", 8s) && screen.find("0x") != std::string::npos;
  closeTextDialog();
  const bool disassembly_error_menu = debugCommand('y');
  (void)visible("Address/expression ($pc):", 5s);
  screen.clear(); send("missing_address\r");
  const bool disassembly_error = visible("Disassembly error", 8s);
  closeTextDialog();

  const bool memory_cancel_menu = debugCommand('m');
  const bool memory_cancel_prompt = visible("Memory view", 5s);
  closeTextDialog();
  const bool memory_invalid_menu = debugCommand('m');
  (void)visible("Address/expression ($sp):", 5s); screen.clear(); send("$sp\r");
  const bool memory_invalid_count = visible("Bytes (1..4096):", 5s);
  screen.clear(); send("5000\r");
  const bool memory_invalid = waitFor(pump, [&] {
    return logged("Memory view: byte count must be between 1 and 4096");
  }, 5s);
  const bool memory_normal_menu = debugCommand('m');
  (void)visible("Address/expression ($sp):", 5s); screen.clear(); send("$sp\r");
  (void)visible("Bytes (1..4096):", 5s); screen.clear(); send("16\r");
  const bool memory_result = visible("Memory", 8s) && screen.find("0x") != std::string::npos;
  closeTextDialog();
  const bool memory_error_menu = debugCommand('m');
  (void)visible("Address/expression ($sp):", 5s); screen.clear(); send("missing_address\r");
  (void)visible("Bytes (1..4096):", 5s); screen.clear(); send("16\r");
  const bool memory_error = visible("Memory error", 8s);
  closeTextDialog();

  (void)debugCommand('h');
  const bool signals_dialog = visible("Inspect policies", 5s);
  closeTextDialog();
  (void)debugCommand('h');
  (void)visible("Inspect policies", 5s); screen.clear(); send("\r");
  const bool signals_inspected = waitFor(pump, [&] { return logged("SIGUSR1"); }, 5s);
  (void)debugCommand('h');
  (void)visible("Inspect policies", 5s); screen.clear(); send("\033[B\r");
  const bool signals_picker = visible("Select signal", 5s);
  screen.clear(); send("\r");
  const bool signal_policy_dialog = visible("Handling of SIGHUP", 5s);
  screen.clear(); send("\r");
  const bool signal_policy_requested = waitFor(pump, [&] {
    return logged("Changing SIGHUP policy");
  }, 5s);

  const bool stop_menu = debugCommand('t');
  const bool debug_stopped_by_user = waitFor(pump, [&] { return logged("Debug session stopped"); }, 8s);
  const bool core_menu = debugCommand('c');
  const bool core_dialog = visible("Executable:", 5s)
    && screen.find("Core dump:") != std::string::npos
    && screen.find("read-only") != std::string::npos;
  closeTextDialog();
  screen.clear(); send("\033f"); (void)visible("Exit", 3s); send("x");
  int status{};
  const auto exited = waitFor(pump, [&] { return ::waitpid(child, &status, WNOHANG) == child; }, 10s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);

  (void)evaluate_cancel_menu;
  (void)evaluate_normal_menu;
  (void)evaluate_error_menu;
  (void)assignment_menu;
  (void)disassembly_menu;
  (void)disassembly_error_menu;
  (void)memory_cancel_menu;
  (void)memory_invalid_menu;
  (void)memory_normal_menu;
  (void)memory_error_menu;
  (void)stop_menu;
  (void)core_menu;
  (void)open_dialog;
  (void)breakpoint_line;
  (void)panel_hidden;
  (void)properties_error_dialog;
  (void)properties_normal_dialog;
  (void)evaluate_normal_prompt;
  (void)assignment_expression;
  (void)assignment_value;
  (void)disassembly_prompt;
  (void)memory_invalid_count;
  const bool success = started && opened
    && redraw_only && watch_cancel_prompt && watch_cancelled && watch_added && watch_duplicate
    && breakpoint_set && panel_focused
    && properties_cancel_dialog && properties_cancelled
    && properties_error && properties_error_safe && properties_saved
    && debug_started && debug_stopped && execution_marker
    && evaluate_cancel_prompt && evaluation_result && evaluation_error
    && assignment_result && disassembly_result && disassembly_error
    && memory_cancel_prompt && memory_invalid
    && memory_result && memory_error && signals_dialog && signals_inspected
    && signals_picker && signal_policy_dialog && signal_policy_requested && debug_stopped_by_user
    && core_dialog
    && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::ifstream log_input(log);
    const std::string log_text((std::istreambuf_iterator<char>(log_input)), std::istreambuf_iterator<char>());
    std::cerr << "Debug dialogs PTY: started=" << started << " opened=" << opened
      << " watch=" << watch_cancelled << "/" << watch_added << "/" << watch_duplicate
      << " breakpoint=" << breakpoint_set << "/" << panel_focused
      << " properties=" << properties_cancelled << "/" << properties_error
      << "/" << properties_error_safe << "/" << properties_saved
      << " gdb=" << debug_started << "/" << debug_stopped
      << " marker=" << execution_marker
      << " cancel-prompts=" << watch_cancel_prompt << "/" << properties_cancel_dialog
      << "/" << evaluate_cancel_prompt << "/" << memory_cancel_prompt
      << " evaluate=" << evaluation_result << "/" << evaluation_error
      << " assignment=" << assignment_expression << "/" << assignment_value << "/" << assignment_result
      << " disassembly=" << disassembly_result << "/" << disassembly_error
      << " memory=" << memory_invalid << "/" << memory_result << "/" << memory_error
      << " signals=" << signals_dialog << "/" << signals_inspected << "/" << signals_picker
      << "/" << signal_policy_dialog << "/" << signal_policy_requested
      << " stop=" << debug_stopped_by_user << " core-dialog=" << core_dialog
      << " exited=" << exited << " status=" << status
      << "\nSession:\n" << readSession() << "\nLog:\n" << log_text << '\n';
  }
  return success;
}

auto exerciseExternalChangesPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& project) -> bool {
  const auto source = project / "main.cpp";
  std::ifstream original_input(source, std::ios::binary);
  const std::string original((std::istreambuf_iterator<char>(original_input)),
    std::istreambuf_iterator<char>());
  int master{-1};
  winsize window{24, 80, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    const auto config = project / ".external-test-config";
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), project.c_str(), static_cast<char*>(nullptr));
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
    return screen.find("Open files") != std::string::npos;
  }, 5s);
  const char open = 15;
  screen.clear();
  (void)::write(master, &open, 1);
  const auto chooser = waitFor(pump, [&] { return screen.find("main.cpp") != std::string::npos; }, 5s);
  const std::string open_main{"\177main.cpp\r"};
  screen.clear();
  (void)::write(master, open_main.data(), open_main.size());
  const auto opened = waitFor(pump, [&] { return screen.find("1 file(s)") != std::string::npos; }, 5s);

  const std::string external_text = "// EXTERNAL_RELOAD_MARKER\nint main() { return 0; }\n";
  { std::ofstream output(source, std::ios::binary | std::ios::trunc); output << external_text; }
  screen.clear();
  const auto changed_dialog = waitFor(pump, [&] {
    return screen.find("File changed outside TUI IDE") != std::string::npos
      && screen.find("Reload from disk") != std::string::npos;
  }, 6s);
  const char enter = '\r';
  (void)::write(master, &enter, 1);
  const auto reloaded = waitFor(pump, [&] {
    return screen.find("Reloaded external changes") != std::string::npos;
  }, 5s);

  std::error_code remove_error;
  std::filesystem::remove(source, remove_error);
  screen.clear();
  const auto deleted_dialog = waitFor(pump, [&] {
    return screen.find("File deleted outside TUI IDE") != std::string::npos
      && screen.find("Keep editor contents") != std::string::npos;
  }, 6s);
  (void)::write(master, &enter, 1);
  const auto kept = waitFor(pump, [&] {
    return screen.find("Kept editor contents after external deletion") != std::string::npos;
  }, 5s);
  const char quit = 4;
  (void)::write(master, &quit, 1);
  int status{};
  const auto exited = waitFor(pump, [&] {
    return ::waitpid(child, &status, WNOHANG) == child;
  }, 5s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);
  { std::ofstream output(source, std::ios::binary | std::ios::trunc); output << original; }
  return ready && chooser && opened && changed_dialog && reloaded && deleted_dialog && kept
    && !remove_error && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

auto exerciseRecoveryPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& project) -> bool {
  const auto source = project / "main.cpp";
  const auto recovery_file = project / "build/.tuiide-recovery.json";
  tuiide::clearRecovery(recovery_file);
  std::ifstream original_input(source, std::ios::binary);
  const std::string original((std::istreambuf_iterator<char>(original_input)),
    std::istreambuf_iterator<char>());
  const auto start = [&](std::string_view config_name, int& master) -> pid_t {
    winsize window{24, 80, 0, 0};
    const auto child = ::forkpty(&master, nullptr, nullptr, &window);
    if (child == 0) {
      ::setenv("TERM", "xterm-256color", 1);
      const auto config = project / std::string(config_name);
      ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
      ::execl(tuiide.c_str(), tuiide.c_str(), project.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    if (child > 0) {
      const auto flags = ::fcntl(master, F_GETFL, 0);
      if (flags >= 0) (void)::fcntl(master, F_SETFL, flags | O_NONBLOCK);
    }
    return child;
  };
  const auto pumpInto = [](int master, std::string& screen) {
    char buffer[4096];
    const auto count = ::read(master, buffer, sizeof(buffer));
    if (count > 0) screen.append(buffer, static_cast<std::size_t>(count));
  };

  int first_master{-1};
  const auto first = start(".recovery-test-config", first_master);
  if (first < 0) return false;
  std::string first_screen;
  const auto first_pump = [&] { pumpInto(first_master, first_screen); };
  const auto ready = waitFor(first_pump, [&] {
    return first_screen.find("Open files") != std::string::npos;
  }, 5s);
  const char open = 15;
  first_screen.clear();
  (void)::write(first_master, &open, 1);
  const auto chooser = waitFor(first_pump, [&] {
    return first_screen.find("main.cpp") != std::string::npos;
  }, 5s);
  const std::string open_main{"\177main.cpp\r"};
  first_screen.clear();
  (void)::write(first_master, open_main.data(), open_main.size());
  const auto opened = waitFor(first_pump, [&] {
    return first_screen.find("1 file(s)") != std::string::npos;
  }, 5s);
  constexpr std::string_view home{"\033[H"};
  constexpr std::string_view marker{"/*RECOVERY_TUI_MARKER*/"};
  (void)::write(first_master, home.data(), home.size());
  (void)::write(first_master, marker.data(), marker.size());
  const auto edited = waitFor(first_pump, [&] {
    return first_screen.find("RECOVERY_TUI_MARKER") != std::string::npos;
  }, 5s);
  std::vector<tuiide::RecoveryDocument> autosaved;
  std::string recovery_error;
  const auto autosaved_ready = waitFor(first_pump, [&] {
    if (!std::filesystem::exists(recovery_file)) return false;
    return tuiide::loadRecovery(recovery_file, autosaved, recovery_error)
      && autosaved.size() == 1
      && autosaved.front().text.find("RECOVERY_TUI_MARKER") != std::string::npos;
  }, 8s);
  ::kill(first, SIGKILL);
  (void)::waitpid(first, nullptr, 0);
  ::close(first_master);

  int second_master{-1};
  const auto second = start(".recovery-test-config", second_master);
  if (second < 0) return false;
  std::string second_screen;
  const auto second_pump = [&] { pumpInto(second_master, second_screen); };
  const auto recovery_dialog = waitFor(second_pump, [&] {
    return second_screen.find("Crash recovery") != std::string::npos
      && second_screen.find("Restore 1 autosaved document") != std::string::npos;
  }, 8s);
  const char enter = '\r';
  (void)::write(second_master, &enter, 1);
  const auto restored = waitFor(second_pump, [&] {
    return second_screen.find("Crash recovery restored 1 document") != std::string::npos
      && second_screen.find("RECOVERY_TUI_MARKER") != std::string::npos;
  }, 6s);
  const bool recovery_consumed = !std::filesystem::exists(recovery_file);
  const char quit = 4;
  second_screen.clear();
  (void)::write(second_master, &quit, 1);
  const auto unsaved_prompt = waitFor(second_pump, [&] {
    return second_screen.find("Save all changed documents before closing") != std::string::npos;
  }, 5s);
  const char no = 'n';
  (void)::write(second_master, &no, 1);
  int status{};
  const auto exited = waitFor(second_pump, [&] {
    return ::waitpid(second, &status, WNOHANG) == second;
  }, 5s);
  if (!exited) { ::kill(second, SIGKILL); (void)::waitpid(second, &status, 0); }
  ::close(second_master);
  std::ifstream unchanged_input(source, std::ios::binary);
  const std::string unchanged((std::istreambuf_iterator<char>(unchanged_input)),
    std::istreambuf_iterator<char>());
  tuiide::clearRecovery(recovery_file);
  return ready && chooser && opened && edited && autosaved_ready && recovery_error.empty()
    && recovery_dialog && restored && recovery_consumed && unsaved_prompt && exited
    && WIFEXITED(status) && WEXITSTATUS(status) == 0 && unchanged == original;
}

void exerciseCmakeGeneratorMatrix(std::string_view unique) {
  TemporaryProject project{std::filesystem::temp_directory_path()
    / ("tuiide C матрица " + std::string(unique))};
  const auto make_build = project.path / "сборка Make Debug";
  const auto ninja_build = project.path / "сборка Ninja Release";
  std::filesystem::create_directories(project.path / "исходники");
  {
    std::ofstream file(project.path / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\n"
            "project(c_matrix LANGUAGES C)\n"
            "set(CMAKE_C_STANDARD 17)\n"
            "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
            "add_library(c_shared STATIC \"исходники/shared.c\")\n"
            "add_executable(c_alpha \"исходники/alpha.c\")\n"
            "add_executable(c_beta \"исходники/beta.c\")\n"
            "target_link_libraries(c_alpha PRIVATE c_shared)\n"
            "target_link_libraries(c_beta PRIVATE c_shared)\n";
  }
  { std::ofstream file(project.path / "исходники/shared.c"); file << "int answer(void) { return 42; }\n"; }
  { std::ofstream file(project.path / "исходники/alpha.c");
    file << "int answer(void); int main(void) { return answer() == 42 ? 0 : 1; }\n"; }
  { std::ofstream file(project.path / "исходники/beta.c");
    file << "int answer(void); int main(void) { return answer() - 42; }\n"; }

  std::string error;
  std::string output;
  expect(tuiide::createCMakeFileApiQuery(make_build, error),
    "Makefiles fixture creates a CMake File API query");
  expect(runProcess({"cmake", "-S", project.path.string(), "-B", make_build.string(),
      "-G", "Unix Makefiles", "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},
      project.path, output),
    "C fixture with Unicode and spaces configures with Unix Makefiles");
  expect(runProcess({"cmake", "--build", make_build.string(), "--parallel", "1"},
      project.path, output),
    "Debug C fixture builds with Unix Makefiles in one thread");
  auto make_targets = tuiide::loadCMakeExecutableTargets(make_build, error);
  expect(error.empty() && make_targets.size() == 2
      && std::all_of(make_targets.begin(), make_targets.end(), [](const auto& target) {
        return target.configuration == "Debug" && std::filesystem::exists(target.artifact);
      }),
    "CMake File API reports both Debug executable targets from Unix Makefiles");
  const auto alpha = std::find_if(make_targets.begin(), make_targets.end(), [](const auto& target) {
    return target.name == "c_alpha";
  });
  expect(alpha != make_targets.end() && runProcess({alpha->artifact.string()}, project.path, output),
    "Debug C target discovered through CMake File API runs successfully");
  std::ifstream make_database(make_build / "compile_commands.json");
  const std::string make_commands((std::istreambuf_iterator<char>(make_database)),
    std::istreambuf_iterator<char>());
  expect(make_commands.find("alpha.c") != std::string::npos
      && make_commands.find("shared.c") != std::string::npos,
    "Makefiles exports C compilation commands from the Unicode source directory");

  output.clear();
  expect(tuiide::createCMakeFileApiQuery(ninja_build, error),
    "Ninja fixture creates a CMake File API query");
  expect(runProcess({"cmake", "-S", project.path.string(), "-B", ninja_build.string(),
      "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},
      project.path, output),
    "the same C fixture configures with Ninja Release");
  expect(runProcess({"cmake", "--build", ninja_build.string(), "--parallel", "1",
      "--target", "c_beta"}, project.path, output),
    "selected Release C target builds with Ninja in one thread");
  const auto ninja_targets = tuiide::loadCMakeExecutableTargets(ninja_build, error);
  const auto beta = std::find_if(ninja_targets.begin(), ninja_targets.end(), [](const auto& target) {
    return target.name == "c_beta" && target.configuration == "Release";
  });
  expect(error.empty() && ninja_targets.size() == 2 && beta != ninja_targets.end()
      && std::filesystem::exists(beta->artifact)
      && runProcess({beta->artifact.string()}, project.path, output),
    "Ninja codemodel exposes multiple Release targets and the selected artifact runs");
}

auto exercisePresetDialogsPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace) -> bool {
  const auto project = workspace / "preset-dialog-project";
  std::filesystem::create_directories(project);
  { std::ofstream file(project / "CMakeLists.txt"); file
      << "cmake_minimum_required(VERSION 3.20)\nproject(preset_dialog LANGUAGES CXX)\n"
         "add_executable(preset_dialog main.cpp)\n"; }
  { std::ofstream file(project / "main.cpp"); file << "int main() { return 0; }\n"; }
  { std::ofstream file(project / "CMakePresets.json"); file << R"({
    "version": 3,
    "configurePresets": [{"name":"dev","displayName":"Development","generator":"Ninja",
      "binaryDir":"${sourceDir}/build/${presetName}"}]
  })"; }
  { std::ofstream file(project / ".tuiide-project.json"); file
      << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":1}\n"; }

  int master{-1};
  winsize window{28, 110, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
    const auto config = workspace / "preset-dialog-config";
    const auto log = workspace / "preset-dialog.log";
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), "--log-file", log.c_str(), project.c_str(),
      static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    screen.clear();
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const auto fileContains = [&](const std::filesystem::path& path, std::string_view value) {
    std::ifstream input(path);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return text.find(value) != std::string::npos;
  };

  const bool started = visible("Open files");
  const bool configure_key = send(std::string(1, static_cast<char>(16)));  // Ctrl+P
  const bool configure_manager = visible("CMake configure presets")
    && visible("CMakePresets.json");
  const bool add_key = send("\033a");
  const bool add_form = visible("Binary directory:");
  const bool name_entered = send("ui-debug");
  const bool save_key = send("\033s");
  const bool saved = waitFor(pump, [&] {
    return std::filesystem::is_regular_file(project / "CMakeUserPresets.json")
      && fileContains(project / "CMakeUserPresets.json", "ui-debug");
  });
  const bool manager_returned = visible("ui-debug");
  const bool selected = send("\r") && waitFor(pump, [&] {
    return fileContains(project / "build/.tuiide-session.json", "ui-debug")
      && fileContains(workspace / "preset-dialog.log", "Selected configure preset: ui-debug");
  });
  const bool quit = send("\021");
  int status{};
  const bool exited = waitFor(pump, [&] {
    return ::waitpid(child, &status, WNOHANG) == child;
  }, 10s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);
  const bool success = started && configure_key && configure_manager && add_key
    && add_form && name_entered && save_key && saved && manager_returned && selected
    && quit && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::cerr << "Preset dialog PTY: started=" << started
      << " configure_key=" << configure_key << " manager=" << configure_manager
      << " add_key=" << add_key << " add_form=" << add_form
      << " name=" << name_entered << " save=" << save_key << " saved=" << saved
      << " returned=" << manager_returned << " selected=" << selected
      << " quit=" << quit << " exited=" << exited << " status=" << status << '\n';
  }
  return success;
}

auto exerciseClassTemplateDialogPty(const std::filesystem::path& tuiide,
    const std::filesystem::path& workspace) -> bool {
  const bool russian = std::getenv("TUIIDE_CLASS_TEMPLATE_LOCALE_ONLY") != nullptr;
  const auto project = workspace / "class-template-project";
  std::filesystem::create_directories(project);
  { std::ofstream file(project / "CMakeLists.txt"); file
      << "cmake_minimum_required(VERSION 3.20)\nproject(class_dialog LANGUAGES CXX)\n"
         "add_executable(class_dialog main.cpp)\n"; }
  { std::ofstream file(project / "main.cpp"); file << "int main() { return 0; }\n"; }
  { std::ofstream file(project / ".tuiide-project.json"); file
      << "{\"version\":1,\"buildDirectory\":\"build\",\"buildJobs\":1,"
         "\"cppHeaderExtension\":\"h\"}\n"; }
  if (russian) {
    const auto settings = workspace / "class-template-config/tuiide/settings.json";
    std::filesystem::create_directories(settings.parent_path());
    std::ofstream file(settings);
    file << "{\"version\":1,\"language\":\"ru\"}\n";
  }

  int master{-1};
  winsize window{30, 110, 0, 0};
  const auto child = ::forkpty(&master, nullptr, nullptr, &window);
  if (child < 0) return false;
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
    ::setenv("TUIIDE_OSC52", "0", 1);
    const auto config = workspace / "class-template-config";
    ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    ::execl(tuiide.c_str(), tuiide.c_str(), project.c_str(), static_cast<char*>(nullptr));
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
  const auto send = [&](std::string_view value) {
    screen.clear();
    return ::write(master, value.data(), value.size()) == static_cast<ssize_t>(value.size());
  };
  const auto visible = [&](std::string_view value, std::chrono::milliseconds timeout = 5s) {
    return waitFor(pump, [&] { return screen.find(value) != std::string::npos; }, timeout);
  };
  const bool started = visible(russian ? "Открытые файлы" : "Open files");
  const bool focused = send("\005") && visible("main.cpp");
  const bool picker_key = send("\033[2~");
  const bool picker = visible("C++ class");
  const bool class_selected = send("\033[F") && send("\r");
  const bool options = visible(russian ? "Параметры класса C++" : "C++ class options")
    && visible(russian ? "Имя класса:" : "Class name:")
    && visible("NewClass.h") && visible("NewClass.cpp");
  const bool class_entered = send("Widget");
  const std::string erase_default(32, '\177');
  const bool header_focus = send("\r");
  const bool header_entered = send(erase_default) && send("widget_api.hpp")
    && visible("widget_api.hpp");
  const bool source_focus = send("\r");
  const bool source_entered = send(erase_default) && send("widget_impl.cpp")
    && visible("widget_impl.cpp");
  const bool cancelled = send("\033") && visible("TUI IDE");
  const bool quit = send("\021");
  int status{};
  const bool exited = waitFor(pump, [&] {
    return ::waitpid(child, &status, WNOHANG) == child;
  }, 10s);
  if (!exited) { ::kill(child, SIGKILL); (void)::waitpid(child, &status, 0); }
  ::close(master);
  const bool success = started && focused && picker_key && picker && class_selected
    && options && class_entered && header_focus && header_entered && source_focus
    && source_entered && cancelled && quit && exited
    && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  if (!success) {
    std::cerr << "Class template PTY: started=" << started << " focused=" << focused
      << " picker_key=" << picker_key << " picker=" << picker
      << " selected=" << class_selected << " options=" << options
      << " class=" << class_entered << " header_focus=" << header_focus
      << " header=" << header_entered << " source_focus=" << source_focus
      << " source=" << source_entered << " cancelled=" << cancelled
      << " quit=" << quit << " exited=" << exited
      << " status=" << status << '\n';
  }
  return success;
}
}  // namespace

int main(int argc, char** argv) {
  expect(argc == 3, "paths to tuiide and the deterministic LSP endpoint are provided");
  const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  TemporaryProject project{std::filesystem::temp_directory_path()
    / ("tuiide C++ интеграция " + unique)};
  const auto build = project.path / "build";
  std::filesystem::create_directories(project.path);
  if (std::getenv("TUIIDE_GIT_ONLY") != nullptr) {
    expect(exerciseGitSession(project.path), "Git status, stage, diff, unstage, and path guard work");
    std::cout << "Git integration test passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_PROJECT_DIALOGS_ONLY") != nullptr) {
    expect(exerciseProjectDialogsPty(std::filesystem::absolute(argv[1]), project.path),
      "New Project and Project Settings dialogs cover normal, cancel, and error outcomes");
    std::cout << "Project dialog PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_WINDOW_HELP_ONLY") != nullptr) {
    expect(exerciseWindowHelpPty(std::filesystem::absolute(argv[1]), project.path),
      "Window and Help menus cover modal, panel, clipboard, and boundary outcomes");
    std::cout << "Window/Help PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_TOOLS_DIALOGS_ONLY") != nullptr) {
    expect(exerciseToolsDialogsPty(std::filesystem::absolute(argv[1]), project.path),
      "Tools dialogs cover normal, cancel, validation, persistence, and unavailable outcomes");
    std::cout << "Tools dialog PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_SHORTCUTS_ONLY") != nullptr) {
    expect(exerciseToolsDialogsPty(std::filesystem::absolute(argv[1]), project.path),
      "shortcut editor captures, validates, saves, cancels, and applies bindings");
    std::cout << "Shortcut editor PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_SEARCH_DIALOGS_ONLY") != nullptr) {
    expect(exerciseSearchDialogsPty(std::filesystem::absolute(argv[1]), project.path),
      "Search dialogs cover navigation, project results, replacement, cancel, and error outcomes");
    std::cout << "Search dialog PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_LSP_DIALOGS_ONLY") != nullptr) {
    expect(exerciseLspDialogsPty(std::filesystem::absolute(argv[1]),
      std::filesystem::absolute(argv[2]), project.path),
      "LSP dialogs cover normal, empty, error, and cancelled workspace-edit outcomes");
    std::cout << "LSP dialog PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_DEBUG_DIALOGS_ONLY") != nullptr) {
    expect(exerciseDebugDialogsPty(std::filesystem::absolute(argv[1]), project.path),
      "Debug dialogs cover breakpoint, watch, evaluation, assignment, disassembly, and memory outcomes");
    std::cout << "Debug dialog PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_PRESET_DIALOGS_ONLY") != nullptr) {
    expect(exercisePresetDialogsPty(std::filesystem::absolute(argv[1]), project.path),
      "CMake preset managers create, select, cancel, and persist user presets");
    std::cout << "CMake preset dialog PTY tests passed\n";
    return 0;
  }
  if (std::getenv("TUIIDE_CLASS_TEMPLATE_ONLY") != nullptr
      || std::getenv("TUIIDE_CLASS_TEMPLATE_LOCALE_ONLY") != nullptr) {
    expect(exerciseClassTemplateDialogPty(std::filesystem::absolute(argv[1]), project.path),
      "C++ class dialog exposes editable header and implementation file names");
    std::cout << "C++ class template dialog PTY tests passed\n";
    return 0;
  }
  std::string cli_output;
  expect(runProcess({std::filesystem::absolute(argv[1]).string(), "--help"}, project.path, cli_output)
      && cli_output.find("--project PATH") != std::string::npos,
    "CLI help works without starting the terminal UI");
  cli_output.clear();
  expect(runProcess({std::filesystem::absolute(argv[1]).string(), "--version"}, project.path, cli_output)
      && cli_output.find("tuiide 0.1.0") != std::string::npos,
    "CLI version works without starting the terminal UI");
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
  TemporaryProject secondary_project{std::filesystem::temp_directory_path()
    / ("tuiide secondary " + unique)};
  std::filesystem::create_directories(secondary_project.path);
  {
    std::ofstream file(secondary_project.path / "CMakeLists.txt");
    file << "cmake_minimum_required(VERSION 3.20)\nproject(secondary LANGUAGES CXX)\n"
      "add_executable(secondary secondary.cpp)\n";
  }
  { std::ofstream file(secondary_project.path / "secondary.cpp"); file << "int main() { return 0; }\n"; }
  {
    std::ofstream file(secondary_project.path / ".tuiide-project.json");
    file << "{\"version\":1,\"buildDirectory\":\"build\","
      "\"launch\":{\"arguments\":[\"SECOND_PROJECT_ARGUMENT\"]}}\n";
  }

  std::string process_output;
  expect(runProcess({"cmake", "-S", project.path.string(), "-B", build.string(),
    "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"}, project.path, process_output),
    "fixture configures with CMake");
  expect(runProcess({"cmake", "--build", build.string(), "--parallel", "1"}, project.path, process_output),
    "fixture builds in one thread");
  std::filesystem::copy_file(build / "compile_commands.json", project.path / "compile_commands.json",
    std::filesystem::copy_options::overwrite_existing);

  expect(argc >= 3, "integration test receives the deterministic LSP endpoint");
  exerciseLspOutcomeMatrix(std::filesystem::absolute(argv[2]), project.path);
  if (std::getenv("TUIIDE_LSP_MATRIX_ONLY") != nullptr) {
    std::cout << "LSP response matrix passed\n";
    return 0;
  }

  if (std::getenv("TUIIDE_PTY_ONLY") != nullptr) {
    expect(exercisePty(std::filesystem::absolute(argv[1]), project.path, true),
      "Final Cut TUI survives its pseudo-terminal interaction and resize scenario");
    std::cout << "PTY integration test passed\n";
    return 0;
  }

  exerciseCmakeGeneratorMatrix(unique);

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
  expect(!lsp.running() && !lsp.ready() && lsp.diagnostics().empty()
      && lsp.semanticTokens().empty() && lsp.takeFeedback().empty()
      && lsp.takeCompletions().empty() && lsp.takeReferences().empty()
      && !lsp.takeRenameEdit().has_value(),
    "stopping clangd removes diagnostics and pending project response state");

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
    const auto first_level_ready = waitFor([&] {
      gdb.poll();
      auto lines = gdb.takeOutput();
      gdb_log.insert(gdb_log.end(), std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
    }, [&] {
      return std::any_of(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
        return variable.depth > 0;
      });
    });
    const auto left_ready = [&] {
      return std::any_of(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
        return variable.depth > 0 && variable.name == "left"
          && variable.value.find("42") != std::string::npos;
      });
    };
    bool children_ready = first_level_ready && left_ready();
    if (first_level_ready && !children_ready) {
      const auto access_group = std::find_if(gdb.variables().begin(), gdb.variables().end(), [](const auto& variable) {
        return variable.depth == 1
          && (variable.name == "public" || variable.name == "private" || variable.name == "protected")
          && variable.expandable;
      });
      if (access_group != gdb.variables().end()) {
        const auto access_index = static_cast<std::size_t>(
          std::distance(gdb.variables().begin(), access_group));
        if (gdb.toggleVariable(access_index)) {
          children_ready = waitFor([&] {
            gdb.poll();
            auto lines = gdb.takeOutput();
            gdb_log.insert(gdb_log.end(), std::make_move_iterator(lines.begin()),
              std::make_move_iterator(lines.end()));
          }, left_ready);
        }
      }
    }
    if (!children_ready) {
      std::cerr << "GDB variable tree after expansion:\n";
      for (const auto& variable : gdb.variables())
        std::cerr << "  depth=" << variable.depth << " name='" << variable.name
                  << "' expression='" << variable.expression << "' object='" << variable.object
                  << "' value='" << variable.value << "' expandable=" << variable.expandable
                  << " expanded=" << variable.expanded << '\n';
      for (const auto& line : gdb_log) std::cerr << "MI: " << line << '\n';
    }
    expect(children_ready, "GDB variable tree exposes a structured child value");
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
    const auto evaluation_ready = waitFor([&] {
      gdb.poll();
      auto results = gdb.takeResults();
      debug_results.insert(debug_results.end(), std::make_move_iterator(results.begin()),
        std::make_move_iterator(results.end()));
    }, [&] {
      return std::any_of(debug_results.begin(), debug_results.end(), [](const auto& result) {
        return result.kind == tuiide::DebugResultKind::Evaluation && result.error.empty()
          && result.expression == "result + pair.right" && result.value.find("107") != std::string::npos;
      });
    });
    if (!evaluation_ready) {
      std::cerr << "GDB expression results:\n";
      for (const auto& result : debug_results)
        std::cerr << "  kind=" << static_cast<int>(result.kind)
                  << " expression='" << result.expression << "' value='" << result.value
                  << "' error='" << result.error << "'\n";
      for (const auto& line : gdb.takeOutput()) std::cerr << "MI: " << line << '\n';
    }
    expect(evaluation_ready, "GDB evaluates an expression in the selected stack frame");
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
    expect(gdb.setSignalPolicy("SIGUSR1", false, false, false),
      "GDB accepts a signal suppression policy while stopped");
    expect(gdb.inspectSignals(), "GDB accepts inspection of current signal policies");
    expect(waitFor([&] {
      gdb.poll();
      auto lines = gdb.takeOutput();
      gdb_log.insert(gdb_log.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
    }, [&] {
      return std::any_of(gdb_log.begin(), gdb_log.end(), [](const auto& line) {
        return line.find("SIGUSR1") != std::string::npos && line.find("No") != std::string::npos;
      });
    }), "GDB reports the applied SIGUSR1 policy in its console output");
    expect(!gdb.sendSignal("not-a-signal") && gdb.stopped(),
      "invalid signal delivery preserves the stopped debugger state");
    expect(gdb.sendSignal("0"), "GDB resumes with the pending signal suppressed");
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
    gdb.stop();
    gdb.clearBreakpoints();
    expect(gdb.addBreakpoint(project.path / "main.cpp", 10)
        && gdb.start(build / "smoke_app"),
      "signal delivery regression starts a fresh stopped inferior");
    gdb.run();
    expect(waitFor([&] { gdb.poll(); }, [&] { return gdb.stopped(); }),
      "signal delivery regression reaches its breakpoint");
    expect(gdb.sendSignal("SIGTERM"), "GDB sends a named signal to the inferior");
    expect(waitFor([&] { gdb.poll(); }, [&] { return gdb.exited() && !gdb.active(); }),
      "fatal signal delivery reports inferior exit without hanging the debug session");
  }
  gdb.stop();

  // CLI generate-core-file не интерпретирует кавычки вокруг имени с пробелами,
  // поэтому тестовая фикстура использует безопасное уникальное имя в /tmp.
  const auto generated_core_file = std::filesystem::temp_directory_path()
    / ("tuiide-core-" + unique + ".core");
  const auto core_file = project.path / "smoke app.core";
  std::string core_generation_output;
  expect(runProcess({"gdb", "--quiet", "--batch", "-ex", "set confirm off",
      "-ex", "break main.cpp:10", "-ex", "run",
      "-ex", "generate-core-file " + generated_core_file.string(), "-ex", "kill",
      "--args", (build / "smoke_app").string()}, project.path, core_generation_output)
      && std::filesystem::is_regular_file(generated_core_file),
    "GDB fixture generates a core dump at a source breakpoint");
  std::error_code core_move_error;
  std::filesystem::rename(generated_core_file, core_file, core_move_error);
  expect(!core_move_error && std::filesystem::is_regular_file(core_file),
    "core fixture moves to a path with spaces for MI quoting regression");
  tuiide::GdbClient core_gdb;
  expect(core_gdb.addWatch("result"), "core session queues a read-only watch");
  core_gdb.setRegistersEnabled(true);
  expect(core_gdb.openCore(build / "smoke_app", core_file, project.path)
      && core_gdb.mode() == tuiide::DebugSessionMode::Core,
    "GDB starts a dedicated core dump session");
  std::vector<std::string> core_log;
  const auto core_ready = waitFor([&] {
    core_gdb.poll();
    auto lines = core_gdb.takeOutput();
    core_log.insert(core_log.end(), std::make_move_iterator(lines.begin()),
      std::make_move_iterator(lines.end()));
  }, [&] {
    return core_gdb.stopped() && !core_gdb.frames().empty()
      && !core_gdb.variables().empty() && !core_gdb.registers().empty()
      && !core_gdb.watches().empty() && core_gdb.watches()[0].error.empty();
  });
  if (!core_ready) {
    std::cerr << "Core state: stopped=" << core_gdb.stopped()
              << " frames=" << core_gdb.frames().size()
              << " variables=" << core_gdb.variables().size()
              << " registers=" << core_gdb.registers().size() << '\n';
    for (const auto& line : core_log) std::cerr << "Core MI: " << line << '\n';
  }
  expect(core_ready && core_gdb.active()
      && std::none_of(core_log.begin(), core_log.end(), [](const auto& line) {
        return line.find("GDB error:") != std::string::npos;
      }),
    "core dump exposes frames, locals, watches, and registers");
  core_gdb.continueExecution(); core_gdb.next(); core_gdb.step(); core_gdb.finish(); core_gdb.interrupt();
  expect(core_gdb.stopped() && !core_gdb.assign("result", "0")
      && !core_gdb.sendSignal("SIGTERM")
      && !core_gdb.setSignalPolicy("SIGUSR1", true, true, true),
    "core dump rejects execution, assignment, and signal mutations");
  std::vector<tuiide::DebugResult> core_results;
  expect(core_gdb.evaluate("result = 0"),
    "core dump forwards an assignment-shaped expression to GDB's memory-write guard");
  expect(waitFor([&] {
    core_gdb.poll();
    auto results = core_gdb.takeResults();
    core_results.insert(core_results.end(), std::make_move_iterator(results.begin()),
      std::make_move_iterator(results.end()));
  }, [&] {
    return std::any_of(core_results.begin(), core_results.end(), [](const auto& result) {
      return result.expression == "result = 0" && !result.error.empty();
    });
  }), "GDB memory-write guard rejects assignment through expression evaluation");
  expect(core_gdb.evaluate("result"), "core dump accepts read-only expression evaluation");
  expect(waitFor([&] {
    core_gdb.poll();
    auto results = core_gdb.takeResults();
    core_results.insert(core_results.end(), std::make_move_iterator(results.begin()),
      std::make_move_iterator(results.end()));
  }, [&] {
    return std::any_of(core_results.begin(), core_results.end(), [](const auto& result) {
      return result.kind == tuiide::DebugResultKind::Evaluation
        && result.error.empty() && result.value.find("42") != std::string::npos;
    });
  }), "core dump evaluates an expression without modifying captured state");
  expect(core_gdb.stop() && core_gdb.mode() == tuiide::DebugSessionMode::None,
    "closing a core dump resets the debugger lifecycle");
  std::error_code core_cleanup_error;
  std::filesystem::remove(core_file, core_cleanup_error);
  if (std::getenv("TUIIDE_CORE_DUMP_ONLY") != nullptr) {
    std::cout << "GDB core dump tests passed\n";
    return 0;
  }

  int attach_ready[2]{-1, -1};
  expect(::pipe(attach_ready) == 0, "attach regression creates a readiness pipe");
  const auto attach_target = ::fork();
  expect(attach_target >= 0, "attach regression starts a live target process");
  if (attach_target == 0) {
    ::close(attach_ready[0]);
    const bool allowed = ::prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY) == 0;
    const char ready = allowed ? '1' : '0';
    (void)::write(attach_ready[1], &ready, 1);
    ::close(attach_ready[1]);
    if (!allowed) _exit(77);
    for (;;) ::pause();
  }
  ::close(attach_ready[1]);
  char attach_allowed{};
  const auto readiness = ::read(attach_ready[0], &attach_allowed, 1);
  ::close(attach_ready[0]);
  if (readiness == 1 && attach_allowed == '1') {
    tuiide::GdbClient attached_gdb;
    expect(attached_gdb.attach(static_cast<int>(attach_target), project.path)
        && attached_gdb.mode() == tuiide::DebugSessionMode::Attach,
      "GDB starts an attach session for a live process");
    std::vector<std::string> attach_log;
    bool attach_restricted{};
    const auto attached = waitFor([&] {
      attached_gdb.poll();
      auto lines = attached_gdb.takeOutput();
      for (const auto& line : lines) {
        if (line.find("Operation not permitted") != std::string::npos
            || line.find("ptrace") != std::string::npos) attach_restricted = true;
      }
      attach_log.insert(attach_log.end(), std::make_move_iterator(lines.begin()),
        std::make_move_iterator(lines.end()));
    }, [&] { return attached_gdb.stopped() || attach_restricted; });
    expect(attached || attach_restricted,
      "GDB attach reaches a stopped target or reports an explicit ptrace restriction");
    if (!attach_restricted) {
      expect(attached_gdb.active() && attached_gdb.stopped(),
        "attached process is represented as an active stopped inferior");
      const bool detached_cleanly = attached_gdb.stop();
      const auto detach_log = attached_gdb.takeOutput();
      expect(detached_cleanly && attached_gdb.mode() == tuiide::DebugSessionMode::None
          && ::kill(attach_target, 0) == 0
          && std::none_of(detach_log.begin(), detach_log.end(), [](const auto& line) {
            return line.find("detach was not acknowledged") != std::string::npos;
          }),
        "Debug Stop confirms detach and leaves the external target process alive");
    } else {
      std::cout << "GDB attach skipped: ptrace is restricted\n";
      attached_gdb.stop();
    }
  }
  (void)::kill(attach_target, SIGTERM);
  int attach_status{};
  (void)::waitpid(attach_target, &attach_status, 0);

  std::error_code configure_cleanup_error;
  std::filesystem::remove(build / "CMakeCache.txt", configure_cleanup_error);
  std::filesystem::remove_all(build / "CMakeFiles", configure_cleanup_error);

  TemporaryProject ui_lifecycle{std::filesystem::temp_directory_path()
    / ("tuiide UI lifecycle " + unique)};
  const auto ui_source = ui_lifecycle.path / "ui_import";
  const auto ui_build = ui_lifecycle.path / "ui_import_build";
  std::filesystem::create_directories(ui_source);
  {
    std::ofstream file(ui_source / "main.cpp");
    file << "#include <iostream>\n#include <string>\n"
      "int main(int argc, char** argv) {\n"
      "  std::cout << \"ARG=\" << (argc > 1 ? argv[1] : \"missing\") << '\\n';\n"
      "  std::string input; std::getline(std::cin, input);\n"
      "  std::cout << \"INPUT=\" << input << '\\n';\n"
      "  return argc > 1 && std::string(argv[1]) == \"ui-argument\""
      " && (input == \"run-input\" || input == \"debug-input\") ? 0 : 2;\n}\n";
  }
  expect(exerciseImportLifecyclePty(std::filesystem::absolute(argv[1]),
      ui_lifecycle.path, ui_source, ui_build, ptrace_denied),
    "UI import wizard configures, builds, runs, and debugs with arguments and interactive stdin");

  expect(exercisePty(std::filesystem::absolute(argv[1]), project.path, false, secondary_project.path),
    "Final Cut TUI switches projects without retaining documents, settings, or breakpoints");
  expect(exerciseExternalChangesPty(std::filesystem::absolute(argv[1]), project.path),
    "open document reloads an external modification and can retain contents after external deletion");
  expect(exerciseRecoveryPty(std::filesystem::absolute(argv[1]), project.path),
    "unsaved editor contents survive a simulated crash and restore through the recovery dialog");
  expect(exerciseEmptyStartup(std::filesystem::absolute(argv[1]), project.path / "empty-config"),
    "starting without a path shows a no-project screen and does not start clangd");
  std::cout << "Integration smoke test passed\n";
}
