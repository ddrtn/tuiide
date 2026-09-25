#include "tuiide/ide_window.hpp"

#include "tuiide/build_command.hpp"
#include "tuiide/debug_dialogs.hpp"
#include "tuiide/document_labels.hpp"
#include "tuiide/project_dialogs.hpp"
#include "tuiide/run_dialogs.hpp"
#include "tuiide/search_dialogs.hpp"
#include "tuiide/text_display.hpp"
#include "tuiide/toolchain_kit.hpp"
#include "tuiide/ui_dialogs.hpp"
#include "tuiide/ui_localization.hpp"
#include "tuiide/workspace_file_transaction.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <unistd.h>

namespace tuiide {
namespace {
struct IdeCommand {
  std::string_view id;
  std::string_view title;
  finalcut::FKey default_key;
  std::string_view default_shortcut;
};

auto ideCommands() -> const std::vector<IdeCommand>& {
  static const std::vector<IdeCommand> commands{
    {"file.new", "File: New", finalcut::FKey::Ctrl_n, "Ctrl+N"},
    {"file.open", "File: Open", finalcut::FKey::Ctrl_o, "Ctrl+O"},
    {"file.save", "File: Save", finalcut::FKey::Ctrl_s, "Ctrl+S"},
    {"file.close", "File: Close", finalcut::FKey::Ctrl_w, "Ctrl+W"},
    {"file.closeAll", "File: Close All", finalcut::FKey::Meta_W, "Alt+Shift+W"},
    {"file.reopen", "File: Reopen Closed", finalcut::FKey::Meta_u, "Alt+U"},
    {"search.find", "Search: Find and Replace", finalcut::FKey::Ctrl_f, "Ctrl+F"},
    {"search.goToLine", "Search: Go to Line", finalcut::FKey::Ctrl_g, "Ctrl+G"},
    {"search.definition", "Search: Go to Definition", finalcut::FKey::F3, "F3"},
    {"search.references", "Search: Find References", finalcut::FKey::F4, "F4"},
    {"search.problems", "Search: Problems", finalcut::FKey::Ctrl_k, "Ctrl+K"},
    {"search.nextDiagnostic", "Search: Next Diagnostic", finalcut::FKey::F32, "Ctrl+F8"},
    {"run.debug", "Debug: Start / Continue", finalcut::FKey::F5, "F5"},
    {"run.run", "Run: Run", finalcut::FKey::F6, "F6"},
    {"run.build", "Run: Build", finalcut::FKey::Ctrl_b, "Ctrl+B"},
    {"run.selectLaunch", "Run: Select Configuration", finalcut::FKey::Meta_L, "Alt+Shift+L"},
    {"run.launchSettings", "Run: Launch Configuration", finalcut::FKey::Meta_l, "Alt+L"},
    {"debug.breakpoint", "Debug: Toggle Breakpoint", finalcut::FKey::F9, "F9"},
    {"debug.stepInto", "Debug: Step Into", finalcut::FKey::F7, "F7"},
    {"debug.stepOver", "Debug: Step Over / Next", finalcut::FKey::F8, "F8"},
    {"debug.stepOut", "Debug: Step Out", finalcut::FKey::F56, "Alt+F8"},
    {"debug.watch", "Debug: Add Watch", finalcut::FKey::Meta_U, "Alt+Shift+U"},
    {"tools.completion", "Tools: Completion", finalcut::FKey::Ctrl_space, "Ctrl+Space"},
    {"tools.hover", "Tools: Symbol Information", finalcut::FKey::F1, "F1"},
    {"tools.rename", "Tools: Rename Symbol", finalcut::FKey::F2, "F2"},
    {"tools.codeActions", "Tools: Code Actions", finalcut::FKey::Meta_a, "Alt+A"},
    {"window.previous", "Window: Previous File", finalcut::FKey::Ctrl_page_up, "Ctrl+PageUp"},
    {"window.next", "Window: Next File", finalcut::FKey::Ctrl_page_down, "Ctrl+PageDown"},
    {"window.previousSidebar", "Window: Previous Sidebar Tab", finalcut::FKey::Meta_page_up, "Alt+PageUp"},
    {"window.nextSidebar", "Window: Next Sidebar Tab", finalcut::FKey::Meta_page_down, "Alt+PageDown"},
    {"window.previousLower", "Window: Previous Lower Tab", finalcut::FKey::Shift_Meta_page_up, "Alt+Shift+PageUp"},
    {"window.nextLower", "Window: Next Lower Tab", finalcut::FKey::Shift_Meta_page_down, "Alt+Shift+PageDown"},
  };
  return commands;
}

auto shortcutKey(std::string value) -> std::optional<finalcut::FKey> {
  if (!reservedShortcutReason(value).empty()) return std::nullopt;
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  static const std::map<std::string, finalcut::FKey> keys{
    {"CTRL+A", finalcut::FKey::Ctrl_a}, {"CTRL+B", finalcut::FKey::Ctrl_b},
    {"CTRL+B", finalcut::FKey::Ctrl_b},
    {"CTRL+D", finalcut::FKey::Ctrl_d}, {"CTRL+E", finalcut::FKey::Ctrl_e},
    {"CTRL+F", finalcut::FKey::Ctrl_f}, {"CTRL+G", finalcut::FKey::Ctrl_g},
    {"CTRL+K", finalcut::FKey::Ctrl_k}, {"CTRL+L", finalcut::FKey::Ctrl_l},
    {"CTRL+N", finalcut::FKey::Ctrl_n}, {"CTRL+O", finalcut::FKey::Ctrl_o},
    {"CTRL+P", finalcut::FKey::Ctrl_p}, {"CTRL+Q", finalcut::FKey::Ctrl_q},
    {"CTRL+R", finalcut::FKey::Ctrl_r}, {"CTRL+S", finalcut::FKey::Ctrl_s},
    {"CTRL+T", finalcut::FKey::Ctrl_t}, {"CTRL+U", finalcut::FKey::Ctrl_u},
    {"CTRL+W", finalcut::FKey::Ctrl_w}, {"CTRL+Y", finalcut::FKey::Ctrl_y},
    {"CTRL+SPACE", finalcut::FKey::Ctrl_space},
    {"CTRL+PAGEUP", finalcut::FKey::Ctrl_page_up}, {"CTRL+PAGEDOWN", finalcut::FKey::Ctrl_page_down},
    {"ALT+PAGEUP", finalcut::FKey::Meta_page_up}, {"ALT+PAGEDOWN", finalcut::FKey::Meta_page_down},
    {"ALT+SHIFT+PAGEUP", finalcut::FKey::Shift_Meta_page_up},
    {"ALT+SHIFT+PAGEDOWN", finalcut::FKey::Shift_Meta_page_down},
    {"ALT+A", finalcut::FKey::Meta_a}, {"ALT+B", finalcut::FKey::Meta_b},
    {"ALT+D", finalcut::FKey::Meta_d}, {"ALT+E", finalcut::FKey::Meta_e},
    {"ALT+F", finalcut::FKey::Meta_f}, {"ALT+H", finalcut::FKey::Meta_h},
    {"ALT+K", finalcut::FKey::Meta_k}, {"ALT+L", finalcut::FKey::Meta_l},
    {"ALT+SHIFT+L", finalcut::FKey::Meta_L},
    {"ALT+F8", finalcut::FKey::F56},
    {"ALT+P", finalcut::FKey::Meta_p},
    {"ALT+R", finalcut::FKey::Meta_r}, {"ALT+S", finalcut::FKey::Meta_s},
    {"ALT+T", finalcut::FKey::Meta_t},
    {"ALT+U", finalcut::FKey::Meta_u}, {"ALT+W", finalcut::FKey::Meta_w},
    {"ALT+SHIFT+U", finalcut::FKey::Meta_U},
    {"ALT+SHIFT+W", finalcut::FKey::Meta_W},
    {"F1", finalcut::FKey::F1}, {"F2", finalcut::FKey::F2}, {"F3", finalcut::FKey::F3},
    {"F4", finalcut::FKey::F4}, {"F5", finalcut::FKey::F5}, {"F6", finalcut::FKey::F6},
    {"F7", finalcut::FKey::F7}, {"F8", finalcut::FKey::F8}, {"F9", finalcut::FKey::F9},
    {"F10", finalcut::FKey::F10}, {"F11", finalcut::FKey::F11}, {"F12", finalcut::FKey::F12},
    {"CTRL+F8", finalcut::FKey::F32},
  };
  const auto found = keys.find(value);
  return found == keys.end() ? std::nullopt : std::optional<finalcut::FKey>{found->second};
}

auto escapeMenuLabel(const std::filesystem::path& path) -> std::string {
  auto label = path.string();
  for (std::size_t offset{}; (offset = label.find('&', offset)) != std::string::npos; offset += 2)
    label.insert(offset, 1, '&');
  return label;
}

auto completionKindName(int kind) -> std::string_view {
  switch (kind) {
    case 2: return "method";
    case 3: return "function";
    case 4: return "constructor";
    case 5: return "field";
    case 6: return "variable";
    case 7: return "class";
    case 8: return "interface";
    case 9: return "module";
    case 10: return "property";
    case 13: return "enum";
    case 14: return "keyword";
    case 20: return "enum member";
    case 21: return "constant";
    case 22: return "struct";
    case 24: return "operator";
    case 25: return "type parameter";
    default: return "item";
  }
}

auto isCppSource(const std::filesystem::path& path) -> bool {
  static const std::vector<std::string> extensions{".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ipp"};
  return std::find(extensions.begin(), extensions.end(), path.extension().string()) != extensions.end();
}

constexpr auto fileDialogFilter = "*";

struct ProjectNode {
  std::filesystem::path path;
  bool file{};
  std::map<std::string, ProjectNode> children;
};

template <typename Menu>
auto menuMnemonicError(std::string_view menu_name, const Menu& menu) -> std::string {
  std::map<int, std::string> owners;
  for (const auto* item : menu.getItemList()) {
    if (item == nullptr || item->isSeparator() || !item->hasHotkey()) continue;
    const auto mnemonic = std::tolower(static_cast<unsigned char>(static_cast<int>(item->getHotkey())));
    const auto [found, inserted] = owners.emplace(mnemonic, item->getText().toString());
    if (!inserted) {
      return "Duplicate mnemonic '" + std::string(1, static_cast<char>(mnemonic)) + "' in "
        + std::string(menu_name) + ": " + found->second + " and " + item->getText().toString();
    }
  }
  return {};
}
}  // namespace

IdeWindow::IdeWindow(std::filesystem::path initial_root, std::filesystem::path log_file,
    bool diagnostic, finalcut::FWidget* parent)
    : FDialog(parent), root_(project_session_.root()), build_dir_(cmake_session_.buildDirectory()),
      session_file_(project_session_.sessionFile()), recovery_file_(project_session_.recoveryFile()),
      project_history_file_(defaultProjectHistoryPath()), user_settings_file_(defaultUserSettingsPath()),
      project_settings_(project_session_.settings()),
      documents_(document_session_.documents()), closed_documents_(document_session_.closedDocuments()),
      document_(document_session_.activeDocument()), active_document_(document_session_.activeIndex()),
      external_tools_(discoverExternalTools()), diagnostic_mode_(diagnostic) {
  std::string user_settings_error;
  std::vector<std::string> shortcut_warnings;
  if (!loadUserSettings(user_settings_file_, user_settings_, user_settings_error, &shortcut_warnings)) user_settings_ = {};
  setText("TUI IDE — C/C++");
  unsetBorder();
  unsetTitleBarButtonVisibility();
  menu_bar_.setForegroundColor(finalcut::FColor::Black);
  menu_bar_.setBackgroundColor(finalcut::FColor::LightGray);
  menu_bar_.delAccelerator();
  menu_bar_.addAccelerator(finalcut::FKey::F10, &menu_bar_);
  menu_bar_.addAccelerator(finalcut::FKey::Menu, &menu_bar_);
  setupMenus();
  refreshRecentFilesMenu();
  const std::array<std::pair<std::string_view, const finalcut::FMenu*>, 10> menus{{
    {"File", &file_menu_.menu}, {"Edit", &edit_menu_.menu}, {"Search", &search_menu_.menu},
    {"Run", &run_menu_.menu}, {"Project", &project_menu_.menu}, {"Debug", &debug_menu_.menu},
    {"Tests", &run_menu_.tests}, {"Tools", &tools_menu_.menu},
    {"Window", &window_menu_.menu}, {"Help", &help_menu_.menu},
  }};
  if (const auto top_error = menuMnemonicError("top-level menu", menu_bar_); !top_error.empty())
    throw std::logic_error(top_error);
  for (const auto& [name, menu] : menus) {
    if (const auto error = menuMnemonicError(name, *menu); !error.empty()) throw std::logic_error(error);
  }
  // Верхнеуровневые Meta-клавиши проходят через IdeWindow::onKeyPress, чтобы
  // работать при фокусе дочернего виджета и в терминалах, кодирующих Alt как ESC-prefix.
  menu_bar_.delAccelerator();
  menu_bar_.addAccelerator(finalcut::FKey::F10, &menu_bar_);
  menu_bar_.addAccelerator(finalcut::FKey::Menu, &menu_bar_);
  applyShortcutAccelerators();
  editor_.setTheme(effectiveEditorTheme(user_settings_), user_settings_.colors);
  sidebar_tabs_.addTab("Open files", tabs_);
  sidebar_tabs_.addTab("Project", files_);
  sidebar_tabs_.addTab("Outline", outline_);
  sidebar_tabs_.addTab("Debug", debug_);
  sidebar_tabs_.addTab("Breakpoints", breakpoints_);
  sidebar_tabs_.addTab("Tests", tests_);
  sidebar_tabs_.addTab("Git", git_files_);
  lower_tabs_.addTab("Output", output_);
  lower_tabs_.addTab("Problems", problems_);
  lower_tabs_.addTab("Build", build_output_);
  lower_tabs_.addTab("Terminal", console_);
  lower_tabs_.addTab("Analysis", analysis_output_);
  notification_.hide();
  output_.setText("Output");
  build_output_.setText("Build output");
  analysis_output_.setText("Analysis output");
  problems_.insert(localizedUiText(user_settings_.language, "No problems"));
  applyUiLanguage();
  problems_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return) { openSelectedProblem(); return true; }
    return handleCommand(key);
  });
  problems_.addCallback("clicked", [this] { openSelectedProblem(); });
  console_.setInputHandler([this](std::string line) {
    if (!run_session_.consoleRunning()) { publishEvent(EventSource::Run, EventSeverity::Warning, "Console input unavailable: no PTY program is running\n"); return; }
    line.push_back('\n');
    if (!run_session_.writeConsole(line)) publishEvent(EventSource::Run, EventSeverity::Error, "Console input failed\n");
  });
  console_.setControlHandler([this](char control) {
    if (control == 'c') {
      if (gdb_.running() && gdb_.active()) gdb_.interrupt();
      else if (!run_session_.signalConsole(SIGINT)) publishEvent(EventSource::Run, EventSeverity::Warning, "Console interrupt unavailable\n");
    } else if (control == 'd' && run_session_.consoleRunning()) {
      (void)run_session_.writeConsole("\x04");
    }
  });
  editor_.setChangedHandler([this] {
    if (document_ && isCppSource(document_->path())) lsp_.change(*document_);
    refreshTabs();
    updateStatus();
  });
  editor_.setFeedbackHandler([this](std::string message, bool warning) {
    publishEvent(EventSource::Editor,
      warning ? EventSeverity::Warning : EventSeverity::Success, std::move(message));
  });
  editor_.setCommandHandler([this](finalcut::FKey key) { return handleCommand(key); });
  editor_.setBreakpointProvider([this](std::size_t line) {
    return document_ && !document_->path().empty() && gdb_.hasBreakpoint(document_->path(), line + 1);
  });
  files_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return) { openSelected(); return true; }
    if (key == finalcut::FKey::Insert) {
      deferred_command_ = [this] { createProjectFile(); };
      return true;
    }
    if (key == finalcut::FKey::Del_char) {
      deferred_command_ = [this] {
        const auto* item = files_.getCurrentItem();
        const auto entry = item ? project_item_paths_.find(item) : project_item_paths_.end();
        std::error_code error;
        if (entry != project_item_paths_.end() && std::filesystem::is_directory(entry->second, error))
          deleteSelectedProjectDirectory();
        else removeSelectedProjectFile();
      };
      return true;
    }
    if (key == finalcut::FKey::Meta_n) { deferred_command_ = [this] { createProjectDirectory(); }; return true; }
    if (key == finalcut::FKey::F2) { deferred_command_ = [this] { renameSelectedProjectEntry(); }; return true; }
    return handleCommand(key);
  });
  tabs_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return) {
      const auto index = tabs_.currentItem();
      if (index > 0 && index <= documents_.size()) activateDocument(index - 1);
      return true;
    }
    if (key == finalcut::FKey::Del_char) { closeActiveDocument(); return true; }
    return handleCommand(key);
  });
  debug_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return) { openSelectedFrame(); return true; }
    if (key == finalcut::FKey::Insert) { deferred_command_ = [this] { addWatch(); }; return true; }
    return handleCommand(key);
  });
  breakpoints_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return || key == finalcut::FKey::F2) {
      deferred_command_ = [this] { editSelectedBreakpoint(); }; return true;
    }
    if (key == finalcut::FKey::Space) { toggleSelectedBreakpoint(); return true; }
    if (key == finalcut::FKey::Del_char) { removeSelectedBreakpoint(); return true; }
    if (key == finalcut::FKey::Meta_D) { deferred_command_ = [this] { clearBreakpoints(); }; return true; }
    return handleCommand(key);
  });
  tests_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return) { openSelectedTestFailure(); return true; }
    if (key == finalcut::FKey::Space) { deferred_command_ = [this] { runSelectedTest(); }; return true; }
    return handleCommand(key);
  });
  git_files_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return) { openSelectedGitFile(); return true; }
    if (key == finalcut::FKey::Space) { stageSelectedGitFile(); return true; }
    if (key == finalcut::FKey::Ctrl_d) {
      deferred_command_ = [this] { diffSelectedGitFile(); }; return true;
    }
    if (key == finalcut::FKey::Ctrl_y) {
      deferred_command_ = [this] { historySelectedGitFile(); }; return true;
    }
    if (key == finalcut::FKey::Ctrl_r) {
      if (!git_session_.running()) (void)git_session_.startStatus();
      return true;
    }
    return handleCommand(key);
  });
  tabs_.setContextHandler([this](finalcut::FPoint position) { showOpenFilesContextMenu(position); });
  files_.setContextHandler([this](finalcut::FPoint position) { showProjectContextMenu(position); });
  debug_.setContextHandler([this](finalcut::FPoint position) { showDebugContextMenu(position, false); });
  breakpoints_.setContextHandler([this](finalcut::FPoint position) { showDebugContextMenu(position, true); });
  files_.addCallback("row-selected", [this] { openSelected(); });
  files_.addCallback("clicked", [this] { openSelected(); });
  const auto activate_tab = [this] {
    const auto index = tabs_.currentItem();
    if (index > 0 && index <= documents_.size()) activateDocument(index - 1);
  };
  tabs_.addCallback("row-selected", activate_tab);
  tabs_.addCallback("clicked", activate_tab);
  debug_.addCallback("row-selected", [this] { openSelectedFrame(); });
  debug_.addCallback("clicked", [this] { openSelectedFrame(); });
  breakpoints_.addCallback("row-selected", [this] { openSelectedBreakpoint(); });
  breakpoints_.addCallback("clicked", [this] { openSelectedBreakpoint(); });
  tests_.addCallback("clicked", [this] { openSelectedTestFailure(); });
  outline_.addCallback("row-selected", [this] { openSelectedOutlineSymbol(); });
  outline_.addCallback("clicked", [this] { openSelectedOutlineSymbol(); });

  timer_id_ = addTimer(100);
  ui_ready_ = true;
  std::string log_error;
  if (!event_log_.openFile(log_file, log_error)) {
    publishEvent(EventSource::System, EventSeverity::Error, "Log file: " + log_error + "\n");
  } else if (!log_file.empty()) {
    publishEvent(EventSource::System, EventSeverity::Information,
      "Event log: " + event_log_.filePath().string() + "\n");
  }
  if (!user_settings_error.empty())
    publishEvent(EventSource::System, EventSeverity::Error,
      "User settings: " + user_settings_error + "; defaults are used\n");
  for (const auto& warning : shortcut_warnings)
    publishEvent(EventSource::System, EventSeverity::Warning, warning + "\n");
  if (diagnostic_mode_) {
    publishEvent(EventSource::System, EventSeverity::Information,
      "Diagnostic mode enabled (hardware threads: "
        + std::to_string(std::max(1U, std::thread::hardware_concurrency())) + ")\n");
  }
  for (const auto& message : externalToolMessages(external_tools_, diagnostic_mode_)) {
    const bool missing = message.find("unavailable:") != std::string::npos;
    publishEvent(EventSource::System,
      missing ? EventSeverity::Warning : EventSeverity::Information, message + "\n");
  }
  std::string history_error;
  recent_projects_ = loadRecentProjects(project_history_file_, history_error);
  if (!history_error.empty()) publishEvent(EventSource::System, EventSeverity::Error, "Recent projects: " + history_error + "\n");
  if (initial_root.empty()) {
    setText("TUI IDE — No project");
    publishEvent(EventSource::System, EventSeverity::Information, "Welcome to TUI IDE. Use File > Open Project or File > New Project to begin.\n");
    refreshFiles(); refreshDebugPanel(); refreshBreakpointsPanel(); refreshTestsPanel(); updateStatus();
  } else if (!loadProject(std::move(initial_root))) {
    setText("TUI IDE — No project");
    refreshFiles(); refreshDebugPanel(); refreshBreakpointsPanel(); refreshTestsPanel(); updateStatus();
  }
  layout();
}

IdeWindow::~IdeWindow() {
  if (debug_state_dirty_) saveDebugState();
  lsp_.stop(); gdb_.stop(); build_session_.reset(); ctest_session_.stop(); analysis_session_.stop();
  run_session_.stop(); console_.setControlEnabled(false);
  gdb_.clearSessionState();
  execution_file_.clear(); execution_line_ = 0;
}

void IdeWindow::adjustSize() {
  FDialog::adjustSize();
  const auto desktop_height = getDesktopHeight();
  setGeometry({1, 1}, {getDesktopWidth(), desktop_height > 1 ? desktop_height - 1 : desktop_height}, false);
  if (ui_ready_) layout();
}

void IdeWindow::setupMenus() {
  file_menu_.project_separator.setSeparator();
  file_menu_.separator.setSeparator();
  edit_menu_.separator1.setSeparator();
  edit_menu_.separator2.setSeparator();
  edit_menu_.separator3.setSeparator();
  search_menu_.separator.setSeparator();
  project_menu_.separator1.setSeparator();
  run_menu_.separator1.setSeparator(); run_menu_.separator2.setSeparator();
  run_menu_.test_separator.setSeparator();
  debug_menu_.separator1.setSeparator(); debug_menu_.separator2.setSeparator();
  tools_menu_.separator.setSeparator(); tools_menu_.separator2.setSeparator(); tools_menu_.separator3.setSeparator();
  tools_menu_.separator4.setSeparator(); tools_menu_.separator_analysis.setSeparator();
  tools_menu_.separator_language.setSeparator();
  window_menu_.separator.setSeparator();
  help_menu_.separator.setSeparator();

  const auto status = [this](finalcut::FMenuItem& item, std::string message) {
    english_status_messages_[&item] = message;
    item.setStatusBarMessage(finalcut::FString(localizedUiText(user_settings_.language, message)));
  };
  const auto bind = [this, &status](finalcut::FMenuItem& item, finalcut::FKey key, std::string message) {
    status(item, std::move(message));
    item.addCallback("clicked", [this, key] { queueMenuCommand(key); });
  };
  file_menu_.new_project.addCallback("clicked", [this] { deferred_command_ = [this] { newProject(); }; });
  status(file_menu_.open_project, "Open a directory containing CMakeLists.txt");
  file_menu_.open_project.addCallback("clicked", [this] { deferred_command_ = [this] { openProject(); }; });
  status(file_menu_.recent_projects, "Open a recently used CMake project");
  file_menu_.recent_projects.addCallback("clicked", [this] { deferred_command_ = [this] { openRecentProject(); }; });
  status(file_menu_.close_project, "Close the current project");
  file_menu_.close_project.addCallback("clicked", [this] { deferred_command_ = [this] { closeProject(); }; });
  bind(file_menu_.new_file, finalcut::FKey::Ctrl_n, "Create a new source file");
  status(file_menu_.new_project_file, "Create a C/C++ file or class and add it to CMake");
  file_menu_.new_project_file.addCallback("clicked", [this] {
    deferred_command_ = [this] { createProjectFile(); };
  });
  bind(file_menu_.open, finalcut::FKey::Ctrl_o, "Open a C or C++ source file");
  status(*file_menu_.recent_files.getItem(), "Open or clear the persistent file history");
  bind(file_menu_.save, finalcut::FKey::Ctrl_s, "Save the active source file");
  status(file_menu_.save_all, "Save every modified document");
  file_menu_.save_all.addCallback("clicked", [this] {
    deferred_command_ = [this] { (void)saveAllDocuments(); };
  });
  status(file_menu_.save_as, "Save the active file under another name");
  file_menu_.save_as.addCallback("clicked", [this] { deferred_command_ = [this] { (void)saveAs(); }; });
  bind(file_menu_.close, finalcut::FKey::Ctrl_w, "Close the active file");
  status(file_menu_.close_others, "Close every file except the active one");
  file_menu_.close_others.addCallback("clicked", [this] {
    deferred_command_ = [this] { closeOtherDocuments(); };
  });
  bind(file_menu_.close_all, finalcut::FKey::Meta_W, "Close every open file");
  bind(file_menu_.reopen_closed, finalcut::FKey::Meta_u, "Reopen the most recently closed saved file");
  status(file_menu_.quit, "Exit TUI IDE");
  file_menu_.quit.addCallback("clicked", [this] { exitIde(); });

  bind(edit_menu_.undo, finalcut::FKey::Ctrl_z, "Undo the last edit");
  bind(edit_menu_.redo, finalcut::FKey::Ctrl_y, "Redo the last edit");
  bind(edit_menu_.cut, finalcut::FKey::Ctrl_x, "Cut selection to the system clipboard");
  bind(edit_menu_.copy, finalcut::FKey::Ctrl_c, "Copy selection to the system clipboard");
  bind(edit_menu_.paste, finalcut::FKey::Ctrl_v, "Paste from the system clipboard");
  bind(edit_menu_.select_all, finalcut::FKey::Ctrl_a, "Select the entire document");
  status(edit_menu_.toggle_comment, "Comment or uncomment the selected lines");
  edit_menu_.toggle_comment.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.toggleComment(); };
  });
  status(edit_menu_.duplicate_line, "Duplicate the current line or selected lines");
  edit_menu_.duplicate_line.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.duplicateLine(); };
  });
  status(edit_menu_.move_line_up, "Move the current line or selected lines up");
  edit_menu_.move_line_up.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.moveLine(false); };
  });
  status(edit_menu_.move_line_down, "Move the current line or selected lines down");
  edit_menu_.move_line_down.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.moveLine(true); };
  });
  status(edit_menu_.delete_line, "Delete the current line or selected lines");
  edit_menu_.delete_line.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.deleteLine(); };
  });

  bind(search_menu_.find, finalcut::FKey::Ctrl_f, "Find text in the active file");
  status(search_menu_.find_next, "Find the next match using the current options");
  search_menu_.find_next.addCallback("clicked", [this] { deferred_command_ = [this] { findNext(false); }; });
  status(search_menu_.find_previous, "Find the previous match using the current options");
  search_menu_.find_previous.addCallback("clicked", [this] { deferred_command_ = [this] { findNext(true); }; });
  status(search_menu_.replace, "Find and replace text in a file or project");
  search_menu_.replace.addCallback("clicked", [this] { deferred_command_ = [this] { find(); }; });
  status(search_menu_.project_search, "Search across editable project files");
  search_menu_.project_search.addCallback("clicked", [this] {
    deferred_command_ = [this] { search_project_ = true; find(); };
  });
  bind(search_menu_.go_to_line, finalcut::FKey::Ctrl_g, "Move to a line number");
  bind(search_menu_.definition, finalcut::FKey::F3, "Open the symbol definition");
  bind(search_menu_.references, finalcut::FKey::F4, "List symbol references");
  bind(search_menu_.problems, finalcut::FKey::Ctrl_k, "List build and clangd problems");
  bind(search_menu_.next_diagnostic, finalcut::FKey::F32,
    "Open the next build or clangd diagnostic");

  project_menu_.refresh.addCallback("clicked", [this] { deferred_command_ = [this] {
    refreshFiles();
    publishEvent(EventSource::Project, EventSeverity::Information,
      "Project tree refreshed: " + std::to_string(project_item_paths_.size()) + " entries\n");
  }; });
  project_menu_.filter.addCallback("clicked", [this] { deferred_command_ = [this] { filterProjectTree(); }; });
  project_menu_.clear_filter.addCallback("clicked", [this] {
    deferred_command_ = [this] {
      project_filter_.clear(); refreshFiles();
      publishEvent(EventSource::Project, EventSeverity::Information, "Project tree filter cleared\n");
    };
  });
  project_menu_.new_directory.addCallback("clicked", [this] { deferred_command_ = [this] { createProjectDirectory(); }; });
  project_menu_.rename_move.addCallback("clicked", [this] { deferred_command_ = [this] { renameSelectedProjectEntry(); }; });
  project_menu_.delete_directory.addCallback("clicked", [this] {
    deferred_command_ = [this] { deleteSelectedProjectDirectory(); };
  });

  status(run_menu_.configure, "Configure the project with CMake");
  run_menu_.configure.addCallback("clicked", [this] { deferred_command_ = [this] { configure(); }; });
  bind(run_menu_.build, finalcut::FKey::Ctrl_b,
    "Build the project; configure first when required");
  status(run_menu_.rebuild, "Clean and build the project");
  run_menu_.rebuild.addCallback("clicked", [this] { deferred_command_ = [this] { rebuild(); }; });
  status(run_menu_.clean, "Build the CMake clean target");
  run_menu_.clean.addCallback("clicked", [this] { deferred_command_ = [this] { clean(); }; });
  status(run_menu_.cancel_build, "Stop the active CMake operation");
  run_menu_.cancel_build.addCallback("clicked", [this] { deferred_command_ = [this] { cancelBuild(); }; });
  bind(run_menu_.run, finalcut::FKey::F6, "Run the selected executable");
  status(run_menu_.stop_run, "Terminate the running program and its process group");
  run_menu_.stop_run.addCallback("clicked", [this] { deferred_command_ = [this] { stopRun(); }; });
  bind(run_menu_.launch_select, finalcut::FKey::Meta_L,
    "Quickly select the active Run/Debug configuration");
  bind(run_menu_.launch_settings, finalcut::FKey::Meta_l,
    "Create, clone, edit, delete, or select Run/Debug configurations");
  bind(run_menu_.configure_preset, finalcut::FKey::Ctrl_p, "Select a CMake configure preset");
  bind(run_menu_.build_preset, finalcut::FKey::Meta_b, "Select a CMake build preset");
  bind(run_menu_.target, finalcut::FKey::Ctrl_t, "Select an executable CMake target");
  status(run_menu_.discover_tests, "Discover tests using CTest JSON output");
  run_menu_.discover_tests.addCallback("clicked", [this] {
    deferred_command_ = [this] { discoverTests(); };
  });
  status(run_menu_.run_all_tests, "Run all discovered CTest tests");
  run_menu_.run_all_tests.addCallback("clicked", [this] {
    deferred_command_ = [this] { runAllTests(); };
  });
  status(run_menu_.run_selected_test, "Run the test selected in the Tests panel");
  run_menu_.run_selected_test.addCallback("clicked", [this] {
    deferred_command_ = [this] { runSelectedTest(); };
  });
  status(run_menu_.rerun_failed_tests, "Rerun tests from CTest's failed-test log");
  run_menu_.rerun_failed_tests.addCallback("clicked", [this] {
    deferred_command_ = [this] { rerunFailedTests(); };
  });
  status(run_menu_.test_preset, "Select and run a CMake test preset");
  run_menu_.test_preset.addCallback("clicked", [this] {
    deferred_command_ = [this] { selectCTestPreset(); };
  });
  status(run_menu_.stop_tests, "Stop the active CTest process");
  run_menu_.stop_tests.addCallback("clicked", [this] {
    deferred_command_ = [this] { stopTests(); };
  });

  bind(debug_menu_.start, finalcut::FKey::F5, "Start or continue debugging");
  bind(debug_menu_.pause, finalcut::FKey::F17, "Pause the debuggee");
  status(debug_menu_.stop, "Stop the debug session and terminate GDB");
  debug_menu_.stop.addCallback("clicked", [this] { deferred_command_ = [this] { debugStop(); }; });
  status(debug_menu_.restart, "Restart the selected executable under GDB");
  debug_menu_.restart.addCallback("clicked", [this] { deferred_command_ = [this] { debugRestart(); }; });
  status(debug_menu_.attach, "Attach GDB to a running Linux process");
  debug_menu_.attach.addCallback("clicked", [this] { deferred_command_ = [this] { attachToProcess(); }; });
  status(debug_menu_.core_dump, "Open an executable and core dump for read-only inspection");
  debug_menu_.core_dump.addCallback("clicked", [this] { deferred_command_ = [this] { openCoreDump(); }; });
  bind(debug_menu_.breakpoint, finalcut::FKey::F9, "Toggle breakpoint on the current line");
  status(debug_menu_.breakpoint_properties, "Edit condition, ignored hits, or logpoint message");
  debug_menu_.breakpoint_properties.addCallback("clicked", [this] { deferred_command_ = [this] { editSelectedBreakpoint(); }; });
  status(debug_menu_.breakpoint_enable, "Enable or disable the selected breakpoint");
  debug_menu_.breakpoint_enable.addCallback("clicked", [this] { deferred_command_ = [this] { toggleSelectedBreakpoint(); }; });
  status(debug_menu_.breakpoint_remove, "Remove the selected breakpoint");
  debug_menu_.breakpoint_remove.addCallback("clicked", [this] { deferred_command_ = [this] { removeSelectedBreakpoint(); }; });
  status(debug_menu_.breakpoint_clear, "Remove every project breakpoint");
  debug_menu_.breakpoint_clear.addCallback("clicked", [this] { deferred_command_ = [this] { clearBreakpoints(); }; });
  bind(debug_menu_.next, finalcut::FKey::F8, "Step over the current source line");
  bind(debug_menu_.step, finalcut::FKey::F7, "Step into the current call");
  bind(debug_menu_.finish, finalcut::FKey::F56, "Finish the current stack frame");
  bind(debug_menu_.watch, finalcut::FKey::Meta_U, "Add a GDB watch expression");
  status(debug_menu_.evaluate, "Evaluate a C/C++ expression in the selected stack frame");
  debug_menu_.evaluate.addCallback("clicked", [this] { deferred_command_ = [this] { evaluateExpression(); }; });
  status(debug_menu_.set_variable, "Change a variable in the selected stack frame");
  debug_menu_.set_variable.addCallback("clicked", [this] { deferred_command_ = [this] { editVariableValue(); }; });
  status(debug_menu_.disassembly, "Disassemble machine instructions near an address or $pc");
  debug_menu_.disassembly.addCallback("clicked", [this] { deferred_command_ = [this] { showDisassembly(); }; });
  status(debug_menu_.memory, "Read a bounded memory range as hex and ASCII");
  debug_menu_.memory.addCallback("clicked", [this] { deferred_command_ = [this] { showMemory(); }; });
  bind(debug_menu_.registers, finalcut::FKey::Ctrl_r, "Show or hide amd64 registers");
  status(debug_menu_.signals, "Inspect signal policies, configure handling, or send a signal and continue");
  debug_menu_.signals.addCallback("clicked", [this] { deferred_command_ = [this] { manageSignals(); }; });

  bind(tools_menu_.completion, finalcut::FKey::Ctrl_space, "Request clangd completion");
  status(tools_menu_.signature, "Show clangd function signature help");
  tools_menu_.signature.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestSignatureHelp(); };
  });
  bind(tools_menu_.hover, finalcut::FKey::F1, "Show clangd symbol information");
  bind(tools_menu_.rename, finalcut::FKey::F2, "Rename a symbol across the workspace");
  bind(tools_menu_.code_actions, finalcut::FKey::Meta_a, "Show clangd quick fixes and refactorings");
  status(tools_menu_.organize_includes, "Sort and remove unused includes with clangd");
  tools_menu_.organize_includes.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestCodeActions(true); };
  });
  status(tools_menu_.switch_source_header, "Switch between matching C/C++ header and source files");
  tools_menu_.switch_source_header.addCallback("clicked", [this] {
    deferred_command_ = [this] { switchSourceHeader(); };
  });
  status(tools_menu_.workspace_symbols, "Search symbols across the clangd workspace index");
  tools_menu_.workspace_symbols.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestWorkspaceSymbols(); };
  });
  status(tools_menu_.call_hierarchy, "Show incoming and outgoing calls for the symbol at the cursor");
  tools_menu_.call_hierarchy.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestHierarchy(false); };
  });
  status(tools_menu_.type_hierarchy, "Show supertypes and subtypes for the type at the cursor");
  tools_menu_.type_hierarchy.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestHierarchy(true); };
  });
  tools_menu_.separator_lsp.setSeparator();
  status(tools_menu_.format_document, "Format the active C/C++ document with clang-format");
  tools_menu_.format_document.addCallback("clicked", [this] {
    deferred_command_ = [this] { formatDocument(false); };
  });
  status(tools_menu_.format_selection, "Format selected C/C++ lines with clang-format");
  tools_menu_.format_selection.addCallback("clicked", [this] {
    deferred_command_ = [this] { formatDocument(true); };
  });
  status(tools_menu_.toolchain_kits, "Detect and select a C/C++ toolchain kit");
  tools_menu_.toolchain_kits.addCallback("clicked", [this] {
    deferred_command_ = [this] { manageToolchainKits(); };
  });
  status(tools_menu_.language_insights, "Request clangd inlay hints, folding, code lens, and include hierarchy");
  tools_menu_.language_insights.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestLanguageInsights(); };
  });
  status(tools_menu_.run_analysis, "Run static checks, sanitizers, coverage, Valgrind, or perf");
  tools_menu_.run_analysis.addCallback("clicked", [this] {
    deferred_command_ = [this] { runAnalysis(); };
  });
  status(tools_menu_.stop_analysis, "Stop the active static-analysis process");
  tools_menu_.stop_analysis.addCallback("clicked", [this] {
    deferred_command_ = [this] { stopAnalysis(); };
  });
  bind(tools_menu_.command_palette, finalcut::FKey::Meta_k, "Search and execute an IDE command");
  status(tools_menu_.configure_shortcut, "Configure a user-wide command shortcut");
  tools_menu_.configure_shortcut.addCallback("clicked", [this] {
    deferred_command_ = [this] { configureShortcut(); };
  });
  status(tools_menu_.shortcut_conflicts, "Show effective shortcuts and conflicts");
  tools_menu_.shortcut_conflicts.addCallback("clicked", [this] {
    deferred_command_ = [this] { showShortcutConflicts(); };
  });
  status(tools_menu_.theme, "Select an accessible editor color theme");
  tools_menu_.theme.addCallback("clicked", [this] { deferred_command_ = [this] { selectTheme(); }; });
  status(tools_menu_.colors, "Override a syntax or diagnostic color role");
  tools_menu_.colors.addCallback("clicked", [this] { deferred_command_ = [this] { configureEditorColor(); }; });
  tools_menu_.language_english.addCallback("clicked", [this] {
    deferred_command_ = [this] { setUiLanguage("en"); };
  });
  tools_menu_.language_russian.addCallback("clicked", [this] {
    deferred_command_ = [this] { setUiLanguage("ru"); };
  });
  status(tools_menu_.project_settings, "Configure CMake, compilers, environment, and clangd");
  tools_menu_.project_settings.addCallback("clicked", [this] {
    deferred_command_ = [this] { projectSettings(); };
  });
  bind(window_menu_.previous, finalcut::FKey::Ctrl_page_up, "Activate the previous open file");
  bind(window_menu_.next, finalcut::FKey::Ctrl_page_down, "Activate the next open file");
  window_menu_.open_files.setChecked();
  window_menu_.project.setChecked();
  window_menu_.outline.setChecked();
  window_menu_.debug.setChecked();
  window_menu_.breakpoints.setChecked();
  window_menu_.tests.setChecked();
  window_menu_.git.setChecked();
  const auto togglePanel = [this](finalcut::FCheckMenuItem& item, std::size_t index) {
    auto* menu_item = &item;
    item.addCallback("clicked", [this, menu_item, index] {
      const bool show = menu_item->isChecked();
      deferred_command_ = [this, menu_item, index, show] {
        if (!sidebar_tabs_.setTabVisible(index, show, true)) {
          menu_item->setChecked();
          publishEvent(EventSource::System, EventSeverity::Information, "At least one sidebar panel must remain visible.\n");
        }
      };
    });
  };
  togglePanel(window_menu_.open_files, 0);
  togglePanel(window_menu_.project, 1);
  togglePanel(window_menu_.outline, 2);
  togglePanel(window_menu_.debug, 3);
  togglePanel(window_menu_.breakpoints, 4);
  togglePanel(window_menu_.tests, 5);
  togglePanel(window_menu_.git, 6);
  status(window_menu_.clear_lower, "Clear Output, Problems, Build, Terminal, or Analysis content");
  window_menu_.clear_lower.addCallback("clicked", [this] { deferred_command_ = [this] { clearLowerPanel(); }; });
  status(window_menu_.copy_lower, "Copy all text from the active lower panel");
  window_menu_.copy_lower.addCallback("clicked", [this] { deferred_command_ = [this] { copyLowerPanel(); }; });
  status(window_menu_.filter_problems, "Filter diagnostics by file, severity, or message");
  window_menu_.filter_problems.addCallback("clicked", [this] { deferred_command_ = [this] { filterProblems(); }; });
  window_menu_.separator3.setSeparator();
  window_menu_.sidebar_narrower.addCallback("clicked", [this] { deferred_command_ = [this] { resizeSidebar(-2); }; });
  window_menu_.sidebar_wider.addCallback("clicked", [this] { deferred_command_ = [this] { resizeSidebar(2); }; });
  window_menu_.lower_shorter.addCallback("clicked", [this] { deferred_command_ = [this] { resizeLowerPanel(-1); }; });
  window_menu_.lower_taller.addCallback("clicked", [this] { deferred_command_ = [this] { resizeLowerPanel(1); }; });
  window_menu_.reset_panels.addCallback("clicked", [this] { deferred_command_ = [this] { resetPanelSizes(); }; });

  status(help_menu_.keyboard, "Show the keyboard reference");
  help_menu_.keyboard.addCallback("clicked", [this] { deferred_command_ = [this] { showKeyboardHelp(); }; });
  status(help_menu_.about, "About TUI IDE");
  help_menu_.about.addCallback("clicked", [this] { deferred_command_ = [this] { showAbout(); }; });
}

void IdeWindow::applyUiLanguage() {
  const auto translate = [this](finalcut::FMenuItem* item) {
    if (item == nullptr || item->isSeparator()) return;
    const auto [entry, inserted] = english_menu_labels_.try_emplace(item, item->getText().toString());
    (void)inserted;
    item->setText(finalcut::FString(localizedUiText(user_settings_.language, entry->second)));
  };
  for (auto* item : menu_bar_.getItemList()) translate(item);
  const std::array<finalcut::FMenu*, 11> menus{
    &file_menu_.menu, &edit_menu_.menu, &search_menu_.menu, &run_menu_.menu,
    &project_menu_.menu, &debug_menu_.menu, &tools_menu_.menu, &window_menu_.menu,
    &help_menu_.menu, &run_menu_.tests, &tools_menu_.language,
  };
  for (auto* menu : menus)
    for (auto* item : menu->getItemList()) translate(item);
  for (auto& [item, message] : english_status_messages_) {
    if (item != nullptr)
      item->setStatusBarMessage(finalcut::FString(localizedUiText(user_settings_.language, message)));
  }

  constexpr std::array<std::string_view, 7> sidebar_titles{
    "Open files", "Project", "Outline", "Debug", "Breakpoints", "Tests", "Git"};
  constexpr std::array<std::string_view, 5> lower_titles{
    "Output", "Problems", "Build", "Terminal", "Analysis"};
  for (std::size_t index{}; index < sidebar_titles.size(); ++index)
    sidebar_tabs_.setTabTitle(index, localizedUiText(user_settings_.language, sidebar_titles[index]));
  for (std::size_t index{}; index < lower_titles.size(); ++index)
    lower_tabs_.setTabTitle(index, localizedUiText(user_settings_.language, lower_titles[index]));
  output_.setText(localizedUiText(user_settings_.language, "Output"));
  build_output_.setText(localizedUiText(user_settings_.language, "Build output"));
  analysis_output_.setText(localizedUiText(user_settings_.language, "Analysis output"));
  tools_menu_.language_english.unsetChecked();
  tools_menu_.language_russian.unsetChecked();
  if (user_settings_.language == "ru") tools_menu_.language_russian.setChecked();
  else tools_menu_.language_english.setChecked();
  refreshRecentFilesMenu();
  menu_bar_.redraw();
}

void IdeWindow::setUiLanguage(std::string language) {
  if (language == user_settings_.language) { applyUiLanguage(); return; }
  auto updated = user_settings_;
  updated.language = std::move(language);
  std::string error;
  if (!saveUserSettings(user_settings_file_, updated, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    applyUiLanguage();
    return;
  }
  user_settings_ = std::move(updated);
  applyUiLanguage();
  problems_signature_.clear();
  refreshProblemsPanel();
  updateStatus();
}

void IdeWindow::applyShortcutAccelerators(bool enabled) {
  const std::map<std::string_view, finalcut::FMenuItem*> items{
    {"file.new", &file_menu_.new_file}, {"file.open", &file_menu_.open},
    {"file.save", &file_menu_.save}, {"file.close", &file_menu_.close},
    {"file.closeAll", &file_menu_.close_all}, {"file.reopen", &file_menu_.reopen_closed},
    {"search.find", &search_menu_.find}, {"search.goToLine", &search_menu_.go_to_line},
    {"search.definition", &search_menu_.definition}, {"search.references", &search_menu_.references},
    {"search.problems", &search_menu_.problems},
    {"search.nextDiagnostic", &search_menu_.next_diagnostic},
    {"run.debug", &debug_menu_.start},
    {"run.run", &run_menu_.run}, {"run.build", &run_menu_.build},
    {"run.selectLaunch", &run_menu_.launch_select},
    {"run.launchSettings", &run_menu_.launch_settings},
    {"debug.breakpoint", &debug_menu_.breakpoint}, {"debug.stepInto", &debug_menu_.step},
    {"debug.stepOver", &debug_menu_.next}, {"debug.stepOut", &debug_menu_.finish},
    {"debug.watch", &debug_menu_.watch},
    {"tools.completion", &tools_menu_.completion}, {"tools.hover", &tools_menu_.hover},
    {"tools.rename", &tools_menu_.rename}, {"tools.codeActions", &tools_menu_.code_actions},
    {"window.previous", &window_menu_.previous},
    {"window.next", &window_menu_.next},
  };
  for (const auto& command : ideCommands()) {
    const auto item = items.find(command.id);
    if (item == items.end()) continue;
    auto key = std::optional<finalcut::FKey>{command.default_key};
    const auto custom = user_settings_.shortcuts.find(std::string(command.id));
    if (custom != user_settings_.shortcuts.end()) key = shortcutKey(custom->second);
    item->second->delAccelerator();
    if (enabled && key) item->second->addAccelerator(*key);
  }
  tools_menu_.command_palette.delAccelerator();
  if (enabled) tools_menu_.command_palette.addAccelerator(finalcut::FKey::Meta_k);
}

void IdeWindow::queueMenuCommand(finalcut::FKey key) {
  deferred_command_ = [this, key] {
    auto effective_key = key;
    const auto command = std::find_if(ideCommands().begin(), ideCommands().end(), [key](const auto& item) {
      return item.default_key == key;
    });
    if (command != ideCommands().end()) {
      const auto custom = user_settings_.shortcuts.find(std::string(command->id));
      if (custom != user_settings_.shortcuts.end()) {
        const auto configured = shortcutKey(custom->second);
        if (configured) effective_key = *configured;
      }
    }
    if (!handleCommand(effective_key)) {
      finalcut::FKeyEvent event(finalcut::Event::KeyPress, effective_key);
      finalcut::FApplication::sendEvent(&editor_, &event);
      editor_.setFocus();
    }
  };
}

void IdeWindow::showAbout() {
  finalcut::FMessageBox::info(this,
    finalcut::FString(localizedUiText(user_settings_.language, "About TUI IDE")),
    finalcut::FString(localizedUiText(user_settings_.language,
      "TUI IDE 0.1\nC/C++ terminal IDE for Linux/amd64\nFinal Cut + clangd + CMake + GDB/MI")));
}

void IdeWindow::showKeyboardHelp() {
  // Прокрутка и адаптивный размер сохраняют начало справки на низком терминале.
  showTextDialog("Keyboard shortcuts", std::string(keyboardHelpText(user_settings_.language)));
}

void IdeWindow::showCommandPalette() {
  std::vector<std::string> items;
  for (const auto& command : ideCommands()) {
    const auto custom = user_settings_.shortcuts.find(std::string(command.id));
    items.push_back(localizedUiText(user_settings_.language, command.title) + "  ["
      + (custom == user_settings_.shortcuts.end() ? std::string(command.default_shortcut) : custom->second) + "]");
  }
  delTimer(timer_id_);
  CommandPaletteDialog dialog(std::move(items), this, user_settings_.language);
  const auto selected = dialog.exec() == finalcut::FDialog::ResultCode::Accept ? dialog.selected() : 0;
  timer_id_ = addTimer(100);
  if (selected == 0 || selected > ideCommands().size()) return;
  const auto& command = ideCommands()[selected - 1];
  const auto custom = user_settings_.shortcuts.find(std::string(command.id));
  const auto key = custom == user_settings_.shortcuts.end()
    ? std::optional<finalcut::FKey>{command.default_key} : shortcutKey(custom->second);
  if (key) (void)handleCommand(*key);
}

void IdeWindow::configureShortcut() {
  std::vector<ShortcutEditorCommand> commands;
  commands.reserve(ideCommands().size());
  for (const auto& command : ideCommands()) {
    commands.push_back({std::string(command.id), std::string(command.title),
      std::string(command.default_shortcut)});
  }
  delTimer(timer_id_);
  applyShortcutAccelerators(false);
  ShortcutEditorDialog dialog(std::move(commands), user_settings_.shortcuts, this,
    user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  applyShortcutAccelerators();
  timer_id_ = addTimer(100);
  if (!accepted) return;
  auto updated = user_settings_;
  updated.shortcuts = dialog.overrides();
  std::string error;
  if (!saveUserSettings(user_settings_file_, updated, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  user_settings_ = std::move(updated);
  applyShortcutAccelerators();
  publishEvent(EventSource::Project, EventSeverity::Success, "Shortcut settings updated\n");
}

void IdeWindow::showShortcutConflicts() {
  std::map<finalcut::FKey, std::vector<std::string>> owners;
  std::ostringstream table;
  table << "Effective shortcut table\n\n";
  owners[finalcut::FKey::Meta_k].push_back("Tools: Command Palette (reserved)");
  table << "Alt+K\tTools: Command Palette (reserved)\n";
  owners[finalcut::FKey::Ctrl_l].push_back("Final Cut: Redraw screen (reserved)");
  table << "Ctrl+L\tFinal Cut: Redraw screen (reserved)\n";
  for (const auto& command : ideCommands()) {
    const auto custom = user_settings_.shortcuts.find(std::string(command.id));
    const auto label = custom == user_settings_.shortcuts.end()
      ? std::string(command.default_shortcut) : custom->second;
    const auto key = shortcutKey(label);
    if (key) owners[*key].push_back(std::string(command.title));
    table << label << "\t" << command.title << '\n';
  }
  table << "\nConflicts\n";
  bool conflict{};
  for (const auto& [key, commands] : owners) {
    (void)key;
    if (commands.size() < 2) continue;
    conflict = true;
    for (const auto& command : commands) table << (command == commands.front() ? "- " : "  ") << command << '\n';
  }
  if (!conflict) table << "No conflicts detected.\n";
  showTextDialog("Keyboard shortcuts", table.str());
}

void IdeWindow::selectTheme() {
  const auto original_theme = effectiveEditorTheme(user_settings_);
  const auto original_colors = user_settings_.colors;
  delTimer(timer_id_);
  ThemeEditorDialog dialog(user_settings_.theme, user_settings_.custom_themes,
    [this](const std::string& theme) { editor_.setTheme(theme, user_settings_.colors); }, this,
    user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) {
    editor_.setTheme(original_theme, original_colors);
    return;
  }
  auto updated = user_settings_;
  updated.theme = dialog.selectedTheme();
  updated.custom_themes = dialog.customThemes();
  std::string error;
  if (!saveUserSettings(user_settings_file_, updated, error)) {
    editor_.setTheme(original_theme, original_colors);
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  user_settings_ = std::move(updated);
  editor_.setTheme(effectiveEditorTheme(user_settings_), user_settings_.colors);
  publishEvent(EventSource::Editor, EventSeverity::Information, "Editor theme: " + user_settings_.theme + "\n");
}

void IdeWindow::configureEditorColor() {
  const auto original_colors = user_settings_.colors;
  const auto theme = effectiveEditorTheme(user_settings_);
  delTimer(timer_id_);
  applyShortcutAccelerators(false);
  ColorEditorDialog dialog(theme, original_colors,
    [this, &theme](const auto& colors) { editor_.setTheme(theme, colors); }, this,
    user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  applyShortcutAccelerators();
  timer_id_ = addTimer(100);
  if (!accepted) {
    editor_.setTheme(theme, original_colors);
    return;
  }
  auto updated = user_settings_;
  updated.colors = dialog.overrides();
  std::string error;
  if (!saveUserSettings(user_settings_file_, updated, error)) {
    editor_.setTheme(theme, original_colors);
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  user_settings_ = std::move(updated);
  editor_.setTheme(effectiveEditorTheme(user_settings_), user_settings_.colors);
  publishEvent(EventSource::Editor, EventSeverity::Success, "Editor colors updated\n");
}

void IdeWindow::showTextDialog(std::string title, std::string text) {
  delTimer(timer_id_);
  TextDialog dialog(std::move(title), std::move(text), this, user_settings_.language);
  (void)dialog.exec();
  timer_id_ = addTimer(100);
}

void IdeWindow::refreshGitPanel() {
  const auto selected = git_files_.currentItem();
  git_files_.clear();
  if (root_.empty()) git_files_.insert("No project");
  else if (git_files_state_.empty())
    git_files_.insert(finalcut::FString(git_panel_message_.empty() ? "Working tree clean" : git_panel_message_));
  else for (const auto& file : git_files_state_) {
    const auto relative = file.path.lexically_relative(root_);
    git_files_.insert(finalcut::FString(file.code + " " + relative.string()));
  }
  if (selected > 0 && selected <= git_files_state_.size()) git_files_.setCurrentItem(selected);
  if (sidebar_tabs_.currentIndex() == 6) sidebar_tabs_.redrawCurrentPage();
}

void IdeWindow::openSelectedGitFile() {
  const auto selected = git_files_.currentItem();
  if (selected > 0 && selected <= git_files_state_.size())
    openFile(git_files_state_[selected - 1].path);
}

void IdeWindow::diffSelectedGitFile() {
  const auto selected = git_files_.currentItem();
  if (selected == 0 || selected > git_files_state_.size()) return;
  const auto& file = git_files_state_[selected - 1];
  if (file.untracked()) {
    showNotification("Stage untracked file to view its diff", NotificationKind::Information);
    return;
  }
  if (!git_session_.startDiff(file.path, file.staged()))
    showNotification("Git is busy", NotificationKind::Warning);
}

void IdeWindow::stageSelectedGitFile() {
  const auto selected = git_files_.currentItem();
  if (selected == 0 || selected > git_files_state_.size()) return;
  const auto& file = git_files_state_[selected - 1];
  for (const auto& open : documents_) {
    if (open->path() == file.path && open->modified()) {
      showNotification("Save the modified file before staging", NotificationKind::Warning);
      return;
    }
  }
  const bool started = file.staged() ? git_session_.startUnstage(file.path, file.code[0] == 'A')
                                     : git_session_.startStage(file.path);
  if (!started) showNotification("Git is busy", NotificationKind::Warning);
}

void IdeWindow::historySelectedGitFile() {
  const auto selected = git_files_.currentItem();
  const auto path = selected > 0 && selected <= git_files_state_.size()
    ? git_files_state_[selected - 1].path : document_ ? document_->path() : std::filesystem::path{};
  if (path.empty()) return;
  if (!git_session_.startHistory(path))
    showNotification("Git is busy", NotificationKind::Warning);
}

void IdeWindow::showContextMenu(finalcut::FMenu& menu, finalcut::FPoint position) {
  if (auto* open = getOpenMenu(); open && open != &menu) open->hide();
  const auto desktop_width = static_cast<int>(getDesktopWidth());
  const auto desktop_height = static_cast<int>(getDesktopHeight());
  const auto x = std::clamp(position.getX(), 1, std::max(1, desktop_width - static_cast<int>(menu.getWidth()) + 1));
  const auto y = std::clamp(position.getY(), 1, std::max(1, desktop_height - static_cast<int>(menu.getHeight()) + 1));
  menu.setPos({x, y});
  context_focus_ = getFocusWidget();
  active_context_menu_ = &menu;
  setOpenMenu(&menu);
  menu.selectFirstItem();
  menu.show(); menu.setFocus(); menu.redraw();
}

void IdeWindow::showOpenFilesContextMenu(finalcut::FPoint position) {
  if (!open_files_context_) {
    open_files_context_ = std::make_unique<OpenFilesContextMenu>(this);
    open_files_context_->activate.addCallback("clicked", [this] {
      deferred_command_ = [this] {
        const auto selected_index = tabs_.currentItem();
        if (selected_index > 0 && selected_index <= documents_.size()) activateDocument(selected_index - 1);
      };
    });
    open_files_context_->save.addCallback("clicked", [this] { deferred_command_ = [this] { (void)save(); }; });
    open_files_context_->close.addCallback("clicked", [this] { deferred_command_ = [this] { closeActiveDocument(); }; });
    open_files_context_->close_others.addCallback("clicked", [this] { deferred_command_ = [this] { closeOtherDocuments(); }; });
    open_files_context_->close_all.addCallback("clicked", [this] {
      deferred_command_ = [this] {
        const auto count = documents_.size();
        if (closeAllDocuments()) publishEvent(EventSource::Editor, EventSeverity::Information, "Close All: closed " + std::to_string(count) + " document(s)\n");
      };
    });
    open_files_context_->reopen.addCallback("clicked", [this] { deferred_command_ = [this] { reopenClosedDocument(); }; });
  }
  const auto index = tabs_.currentItem();
  const bool selected = index > 0 && index <= documents_.size();
  if (selected) activateDocument(index - 1);
  open_files_context_->activate.setEnable(selected);
  open_files_context_->save.setEnable(selected);
  open_files_context_->close.setEnable(selected);
  open_files_context_->close_others.setEnable(selected && documents_.size() > 1);
  open_files_context_->close_all.setEnable(!documents_.empty());
  open_files_context_->reopen.setEnable(!closed_documents_.empty());
  showContextMenu(open_files_context_->menu, position);
}

void IdeWindow::showProjectContextMenu(finalcut::FPoint position) {
  if (!project_context_) {
    project_context_ = std::make_unique<ProjectContextMenu>(this);
    project_context_->open.addCallback("clicked", [this] { deferred_command_ = [this] { openSelected(); }; });
    project_context_->new_file.addCallback("clicked", [this] { deferred_command_ = [this] { createProjectFile(); }; });
    project_context_->new_directory.addCallback("clicked", [this] { deferred_command_ = [this] { createProjectDirectory(); }; });
    project_context_->rename.addCallback("clicked", [this] { deferred_command_ = [this] { renameSelectedProjectEntry(); }; });
    project_context_->remove.addCallback("clicked", [this] {
      deferred_command_ = [this] {
        const auto* selected_item = files_.getCurrentItem();
        const auto selected_entry = selected_item ? project_item_paths_.find(selected_item) : project_item_paths_.end();
        std::error_code type_error;
        if (selected_entry != project_item_paths_.end() && std::filesystem::is_directory(selected_entry->second, type_error))
          deleteSelectedProjectDirectory();
        else removeSelectedProjectFile();
      };
    });
    project_context_->refresh.addCallback("clicked", [this] { deferred_command_ = [this] {
      refreshFiles();
      publishEvent(EventSource::Project, EventSeverity::Information,
        "Project tree refreshed: " + std::to_string(project_item_paths_.size()) + " entries\n");
    }; });
  }
  const auto* item = files_.getCurrentItem();
  const auto entry = item ? project_item_paths_.find(item) : project_item_paths_.end();
  const bool selected = entry != project_item_paths_.end();
  std::error_code error;
  const bool file = selected && std::filesystem::is_regular_file(entry->second, error);
  const bool root = selected && normalizePath(entry->second) == root_;
  project_context_->open.setEnable(file);
  project_context_->new_file.setEnable(!root_.empty());
  project_context_->new_directory.setEnable(!root_.empty());
  project_context_->rename.setEnable(selected && !root);
  project_context_->remove.setEnable(selected && !root);
  project_context_->refresh.setEnable(!root_.empty());
  showContextMenu(project_context_->menu, position);
}

void IdeWindow::showDebugContextMenu(finalcut::FPoint position, bool breakpoints) {
  if (!debug_context_) {
    debug_context_ = std::make_unique<DebugContextMenu>(this);
    debug_context_->open.addCallback("clicked", [this] { deferred_command_ = [this] { openSelectedFrame(); }; });
    debug_context_->open_breakpoint.addCallback("clicked", [this] { deferred_command_ = [this] { openSelectedBreakpoint(); }; });
    debug_context_->add_watch.addCallback("clicked", [this] { deferred_command_ = [this] { addWatch(); }; });
    debug_context_->remove_watch.addCallback("clicked", [this] { deferred_command_ = [this] { removeSelectedWatch(); }; });
    debug_context_->registers.addCallback("clicked", [this] {
      deferred_command_ = [this] { (void)handleCommand(finalcut::FKey::Ctrl_r); };
    });
    debug_context_->properties.addCallback("clicked", [this] { deferred_command_ = [this] { editSelectedBreakpoint(); }; });
    debug_context_->toggle.addCallback("clicked", [this] { deferred_command_ = [this] { toggleSelectedBreakpoint(); }; });
    debug_context_->remove_breakpoint.addCallback("clicked", [this] { deferred_command_ = [this] { removeSelectedBreakpoint(); }; });
    debug_context_->clear_breakpoints.addCallback("clicked", [this] { deferred_command_ = [this] { clearBreakpoints(); }; });
  }
  const auto debug_index = debug_.currentItem();
  const auto* debug_row = debug_index > 0 ? debug_ui_.debugRow(debug_index - 1) : nullptr;
  const bool debug_selected = debug_row != nullptr;
  const bool watch_selected = debug_row && debug_row->watch_index.has_value();
  const auto breakpoint_index = breakpoints_.currentItem();
  const bool breakpoint_selected = breakpoint_index > 0
    && debug_ui_.breakpointRow(breakpoint_index - 1) != nullptr;
  debug_context_->open.setEnable(debug_selected);
  debug_context_->add_watch.setEnable(!root_.empty());
  debug_context_->remove_watch.setEnable(watch_selected);
  debug_context_->registers.setEnable(!root_.empty());
  debug_context_->open_breakpoint.setEnable(breakpoint_selected);
  debug_context_->properties.setEnable(breakpoint_selected);
  debug_context_->toggle.setEnable(breakpoint_selected);
  debug_context_->remove_breakpoint.setEnable(breakpoint_selected);
  debug_context_->clear_breakpoints.setEnable(!debug_ui_.breakpointRows().empty());
  showContextMenu(breakpoints ? debug_context_->breakpoint_menu : debug_context_->debug_menu, position);
}

void IdeWindow::layout() {
  const auto width = std::max<std::size_t>(1, getClientWidth());
  const auto height = std::max<std::size_t>(1, getClientHeight());
  const auto default_sidebar = std::clamp<std::size_t>(width / 3, 20, 38);
  const auto default_output = std::clamp<std::size_t>(height / 3, 5, 12);
  const std::size_t sidebar = std::clamp(sidebar_width_ == 0 ? default_sidebar : sidebar_width_,
    std::size_t{18}, width > 28 ? width - 28 : std::size_t{18});
  const std::size_t output_height = std::clamp(lower_panel_height_ == 0 ? default_output : lower_panel_height_,
    std::size_t{4}, height > 9 ? height - 9 : std::size_t{4});
  sidebar_tabs_.setGeometry({1, 2}, {sidebar, height - 2});
  sidebar_tabs_.layoutPages();
  editor_.setGeometry({static_cast<int>(sidebar + 1), 2}, {width - sidebar, height - output_height - 2});
  lower_tabs_.setGeometry({static_cast<int>(sidebar + 1), static_cast<int>(height - output_height)}, {width - sidebar, output_height});
  lower_tabs_.layoutPages();
  if (run_session_.consoleRunning())
    (void)run_session_.resizeConsole(console_.columns(), console_.rows());
  status_.setGeometry({1, static_cast<int>(height)}, {width, 1});
  const auto workspace_width = width > sidebar ? width - sidebar : std::size_t{1};
  const auto notification_width = std::min<std::size_t>(48, workspace_width > 2 ? workspace_width - 2 : workspace_width);
  notification_.setGeometry({static_cast<int>(width - notification_width + 1), 2}, {notification_width, 1});
}

void IdeWindow::resizeSidebar(int delta) {
  const auto width = std::max<std::size_t>(60, getClientWidth());
  const auto current = sidebar_width_ == 0 ? std::clamp<std::size_t>(width / 3, 20, 38) : sidebar_width_;
  const auto changed = static_cast<long long>(current) + delta;
  const auto resized = std::clamp<std::size_t>(changed < 0 ? 0 : static_cast<std::size_t>(changed), 18, width - 28);
  if (resized == current) {
    showNotification(delta < 0 ? "Sidebar is already at minimum width"
                               : "Sidebar is already at maximum width", NotificationKind::Information);
    return;
  }
  sidebar_width_ = resized;
  debug_state_dirty_ = true; saveDebugState(); layout(); redraw();
}

void IdeWindow::resizeLowerPanel(int delta) {
  const auto height = std::max<std::size_t>(18, getClientHeight());
  const auto current = lower_panel_height_ == 0 ? std::clamp<std::size_t>(height / 3, 5, 12) : lower_panel_height_;
  const auto changed = static_cast<long long>(current) + delta;
  const auto resized = std::clamp<std::size_t>(changed < 0 ? 0 : static_cast<std::size_t>(changed), 4, height - 9);
  if (resized == current) {
    showNotification(delta < 0 ? "Lower panel is already at minimum height"
                               : "Lower panel is already at maximum height", NotificationKind::Information);
    return;
  }
  lower_panel_height_ = resized;
  debug_state_dirty_ = true; saveDebugState(); layout(); redraw();
}

void IdeWindow::resetPanelSizes() {
  sidebar_width_ = 0; lower_panel_height_ = 0;
  debug_state_dirty_ = true; saveDebugState(); layout(); redraw();
  publishEvent(EventSource::System, EventSeverity::Information, "Panel sizes reset to automatic defaults\n");
}

void IdeWindow::refreshFiles() {
  files_.clear(); file_paths_.clear(); project_item_paths_.clear();
  if (root_.empty()) { sidebar_tabs_.redrawCurrentPage(); return; }
  ProjectTreeSnapshot snapshot;
  std::string scan_error;
  if (!scanProjectTree(root_, build_dir_, project_filter_, snapshot, scan_error)) {
    publishEvent(EventSource::Project, EventSeverity::Error, "Project tree: " + scan_error + "\n");
    sidebar_tabs_.redrawCurrentPage(); return;
  }
  file_paths_ = std::move(snapshot.editable_files);
  ProjectNode tree{root_, false, {}};
  for (const auto& entry : snapshot.entries) {
    const auto& path = entry.path;
    std::error_code relative_error;
    const auto relative = std::filesystem::relative(path, root_, relative_error);
    if (relative_error || relative.empty()) continue;
    auto* node = &tree;
    auto current_path = root_;
    for (auto component = relative.begin(); component != relative.end(); ++component) {
      current_path /= *component;
      const bool file = std::next(component) == relative.end() ? !entry.directory : false;
      auto [child, inserted] = node->children.try_emplace(component->string(), ProjectNode{current_path, file, {}});
      if (!inserted && file) child->second.file = true;
      node = &child->second;
    }
  }

  auto root_name = root_.filename().empty() ? root_.string() : root_.filename().string();
  if (!project_filter_.empty()) root_name += " [" + project_filter_ + "]";
  const finalcut::FStringList root_columns{finalcut::FString(root_name)};
  auto root_item = files_.insert(root_columns);
  project_item_paths_.emplace(static_cast<finalcut::FListViewItem*>(*root_item), root_);
  const auto insert_children = [&](const auto& self, const ProjectNode& parent,
                                   finalcut::FObject::iterator parent_item) -> void {
    for (const auto& [name, node] : parent.children) {
      const finalcut::FStringList columns{finalcut::FString(name)};
      auto item = files_.insert(columns, parent_item);
      project_item_paths_.emplace(static_cast<finalcut::FListViewItem*>(*item), node.path);
      if (!node.file) self(self, node, item);
    }
  };
  insert_children(insert_children, tree, root_item);
  auto* root_view_item = static_cast<finalcut::FListViewItem*>(*root_item);
  root_view_item->expand();
  project_menu_.clear_filter.setEnable(!project_filter_.empty());
  if (snapshot.skipped_errors != 0)
    publishEvent(EventSource::Project, EventSeverity::Warning, "Project tree: skipped " + std::to_string(snapshot.skipped_errors) + " inaccessible entries\n");
  sidebar_tabs_.redrawCurrentPage();
}

void IdeWindow::openSelected() {
  const auto* item = files_.getCurrentItem();
  if (!item) return;
  const auto entry = project_item_paths_.find(item);
  if (entry == project_item_paths_.end()) return;
  const auto& path = entry->second;
  std::error_code error;
  if (std::filesystem::is_regular_file(path, error)) openFile(path);
}

void IdeWindow::filterProjectTree() {
  if (root_.empty()) return;
  const auto value = prompt("Project tree filter", "Path substring (empty shows all):");
  project_filter_ = value; refreshFiles();
  publishEvent(EventSource::Project, EventSeverity::Information, project_filter_.empty() ? "Project tree filter cleared\n"
    : "Project tree filter: " + project_filter_ + "\n");
}

void IdeWindow::createProjectDirectory() {
  if (root_.empty()) return;
  auto base = root_;
  if (const auto* item = files_.getCurrentItem()) {
    const auto entry = project_item_paths_.find(item);
    if (entry != project_item_paths_.end()) {
      std::error_code error;
      base = std::filesystem::is_directory(entry->second, error) ? entry->second : entry->second.parent_path();
    }
  }
  const auto name = prompt("New project directory", "Name or relative path:");
  if (name.empty()) return;
  std::string error;
  if (!tuiide::createProjectDirectory(root_, base / name, error)) {
    finalcut::FMessageBox::error(this,
      finalcut::FString(localizedUiText(user_settings_.language, error)));
    return;
  }
  refreshFiles(); publishEvent(EventSource::Project, EventSeverity::Success, "Project directory created: " + (base / name).string() + "\n");
}

void IdeWindow::deleteSelectedProjectDirectory() {
  const auto* item = files_.getCurrentItem();
  const auto entry = item ? project_item_paths_.find(item) : project_item_paths_.end();
  if (entry == project_item_paths_.end() || normalizePath(entry->second) == root_) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(user_settings_.language, "Project")),
      finalcut::FString(localizedUiText(user_settings_.language,
        "Select an empty project subdirectory.")));
    return;
  }
  std::error_code relative_error;
  const auto relative = std::filesystem::relative(entry->second, root_, relative_error);
  const auto answer = finalcut::FMessageBox::info(this,
    finalcut::FString(localizedUiText(user_settings_.language, "Delete directory")),
    finalcut::FString(localizedUiText(user_settings_.language,
      "Delete empty directory from disk?\n") + relative.generic_string()),
    finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No,
    finalcut::FMessageBox::ButtonType::Reject);
  if (answer != finalcut::FMessageBox::ButtonType::Yes) return;
  std::string error;
  if (!tuiide::deleteEmptyProjectDirectory(root_, entry->second, error)) {
    finalcut::FMessageBox::error(this,
      finalcut::FString(localizedUiText(user_settings_.language, error)));
    return;
  }
  refreshFiles(); publishEvent(EventSource::Project, EventSeverity::Success, "Deleted empty project directory: " + relative.generic_string() + "\n");
}

void IdeWindow::renameSelectedProjectEntry() {
  const auto* item = files_.getCurrentItem();
  const auto entry = item ? project_item_paths_.find(item) : project_item_paths_.end();
  if (entry == project_item_paths_.end() || normalizePath(entry->second) == root_) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(user_settings_.language, "Project")),
      finalcut::FString(localizedUiText(user_settings_.language,
        "Select a project file or subdirectory to rename.")));
    return;
  }
  const auto source = normalizePath(entry->second);
  const auto modified_cmake = std::find_if(documents_.begin(), documents_.end(), [](const auto& document) {
    return isCMakePath(document->path()) && document->modified();
  });
  if (modified_cmake != documents_.end()) {
    finalcut::FMessageBox::error(this, finalcut::FString(localizedUiText(user_settings_.language,
      "Save modified CMake files before Rename / Move.")));
    return;
  }
  std::error_code relative_error;
  const auto old_relative = std::filesystem::relative(source, root_, relative_error);
  const auto value = prompt("Rename / Move project entry", "New project-relative path:");
  if (value.empty()) return;
  const std::filesystem::path requested(value);
  const auto destination = normalizePath(requested.is_absolute() ? requested : root_ / requested);
  std::vector<Document*> affected;
  for (const auto& open : documents_) {
    std::error_code document_error;
    const auto relative = std::filesystem::relative(open->path(), source, document_error);
    if (open->path() == source || (!document_error && relative != ".." && *relative.begin() != ".."))
      affected.push_back(open.get());
  }
  for (auto* open : affected) if (isCppSource(open->path())) lsp_.close(*open);
  std::string error;
  CMakeSourceRename cmake_result;
  if (!moveProjectEntryWithCMake(root_, source, destination, cmake_result, error)) {
    for (auto* open : affected) if (isCppSource(open->path())) lsp_.open(*open);
    finalcut::FMessageBox::error(this,
      finalcut::FString(localizedUiText(user_settings_.language, error)));
    return;
  }
  for (auto* open : affected) {
    std::error_code document_error;
    const auto relative = open->path() == source ? std::filesystem::path{}
      : std::filesystem::relative(open->path(), source, document_error);
    open->relocate(relative.empty() ? destination : destination / relative);
    if (isCppSource(open->path())) lsp_.open(*open);
  }
  for (const auto& changed_cmake : cmake_result.changed_files) {
    const auto open = std::find_if(documents_.begin(), documents_.end(), [&](const auto& document) {
      return document->path() == normalizePath(changed_cmake);
    });
    if (open != documents_.end()) {
      std::string reload_error;
      if (!(*open)->load(changed_cmake, reload_error)) publishEvent(EventSource::Project, EventSeverity::Error, "CMake reload: " + reload_error + "\n");
    }
  }
  refreshFiles(); refreshTabs(); editor_.invalidateSyntax(); updateStatus();
  publishEvent(EventSource::Project, EventSeverity::Success, "Project entry moved: " + old_relative.generic_string() + " -> "
    + std::filesystem::relative(destination, root_).generic_string()
    + "; updated " + std::to_string(cmake_result.references_changed) + " CMake reference(s)\n");
}

void IdeWindow::removeSelectedProjectFile() {
  const auto* item = files_.getCurrentItem();
  if (!item) return;
  const auto entry = project_item_paths_.find(item);
  if (entry == project_item_paths_.end()) return;
  const auto path = normalizePath(entry->second);
  std::error_code status_error;
  if (!std::filesystem::is_regular_file(path, status_error)) {
    finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(user_settings_.language, "Project")),
      finalcut::FString(localizedUiText(user_settings_.language,
        "Select a file to remove. Directories are not removed.")));
    return;
  }
  std::error_code relative_error;
  const auto relative = std::filesystem::relative(path, root_, relative_error);
  if (relative_error || relative.empty() || *relative.begin() == "..") {
    finalcut::FMessageBox::error(this, finalcut::FString(localizedUiText(user_settings_.language,
      "The selected file is outside the project root.")));
    return;
  }

  const auto selection = choose("Remove " + relative.generic_string(), {
    "Remove from CMake project only",
    "Remove from CMake project and delete from disk"
  });
  if (selection == 0) return;
  const bool delete_from_disk = selection == 2;
  const auto open_document = std::find_if(documents_.begin(), documents_.end(), [&path](const auto& document) {
    return document->path() == path;
  });
  if (delete_from_disk && open_document != documents_.end() && (*open_document)->modified()) {
    finalcut::FMessageBox::error(this, finalcut::FString(localizedUiText(user_settings_.language,
      "Save or close the modified file before deleting it.")));
    return;
  }
  if (delete_from_disk) {
    const auto answer = finalcut::FMessageBox::info(this,
      finalcut::FString(localizedUiText(user_settings_.language, "Delete file")),
      finalcut::FString(localizedUiText(user_settings_.language,
        "Permanently delete from disk?\n") + relative.generic_string()),
      finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No,
      finalcut::FMessageBox::ButtonType::Reject);
    if (answer != finalcut::FMessageBox::ButtonType::Yes) return;
  }

  CMakeSourceRemoval removal;
  std::string error;
  if (!removeCMakeSourceReferences(root_, path, removal, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  if (delete_from_disk) {
    if (!std::filesystem::remove(path, status_error) || status_error) {
      finalcut::FMessageBox::error(this, finalcut::FString("Cannot delete " + path.string()
        + ": " + status_error.message()));
      return;
    }
    if (open_document != documents_.end()) {
      const auto index = static_cast<std::size_t>(std::distance(documents_.begin(), open_document));
      activateDocument(index);
      closeActiveDocument(false);
    }
    refreshFiles();
  }
  const auto cmake_message = removal.references_removed == 0
    ? std::string("No exact CMake source reference was found")
    : std::to_string(removal.references_removed) + " CMake source reference(s) removed";
  publishEvent(EventSource::Project, EventSeverity::Success, "Project: " + cmake_message + (delete_from_disk ? "; file deleted: " : ": ")
    + relative.generic_string() + "\n");
}

void IdeWindow::refreshTabs() {
  tabs_.clear();
  std::vector<std::filesystem::path> paths;
  paths.reserve(documents_.size());
  for (const auto& document : documents_) paths.push_back(document->path());
  const auto labels = distinguishDocumentLabels(paths);
  for (std::size_t i = 0; i < documents_.size(); ++i) {
    const auto& document = *documents_[i];
    auto label = labels[i];
    if (document.modified()) label += " *";
    tabs_.insert(finalcut::FString(label));
  }
  if (!documents_.empty() && active_document_ < documents_.size()) tabs_.setCurrentItem(active_document_ + 1);
  sidebar_tabs_.redrawCurrentPage();
}

void IdeWindow::refreshDebugPanel() {
  if (!debug_ui_.updateDebug(DebugUiController::capture(gdb_))) return;
  debug_.clear();
  for (const auto& row : debug_ui_.debugRows()) debug_.insert(finalcut::FString(row.label));
  sidebar_tabs_.redrawCurrentPage();
}

void IdeWindow::refreshExecutionLocation() {
  std::filesystem::path file;
  std::size_t line{};
  if (gdb_.stopped()) {
    const auto& frames = gdb_.frames();
    const auto frame = std::find_if(frames.begin(), frames.end(), [](const auto& item) {
      return item.level == 0 && !item.file.empty() && item.line != 0;
    });
    if (frame != frames.end()) {
      file = frame->file.is_absolute() ? frame->file : root_ / frame->file;
      file = normalizePath(file);
      line = frame->line;
    }
  }

  const bool changed = file != execution_file_ || line != execution_line_;
  execution_file_ = std::move(file);
  execution_line_ = line;
  if (changed && execution_line_ != 0) {
    std::error_code error;
    if (std::filesystem::is_regular_file(execution_file_, error)) {
      if (!document_ || document_->path() != execution_file_) openFile(execution_file_);
      if (document_ && document_->path() == execution_file_)
        editor_.reveal({execution_line_ - 1, 0});
    }
  }
  const bool visible = execution_line_ != 0 && document_
    && document_->path() == execution_file_ && execution_line_ <= document_->lines().size();
  editor_.setExecutionLine(visible
    ? std::optional<std::size_t>{execution_line_ - 1} : std::nullopt);
}

void IdeWindow::refreshBreakpointsPanel() {
  const auto current = breakpoints_.currentItem();
  if (!debug_ui_.updateBreakpoints(gdb_.breakpoints(), gdb_.running(), root_)) return;
  breakpoints_.clear();
  if (debug_ui_.breakpointRows().empty()) breakpoints_.insert("No breakpoints");
  for (const auto& row : debug_ui_.breakpointRows()) breakpoints_.insert(finalcut::FString(row.label));
  if (!debug_ui_.breakpointRows().empty())
    breakpoints_.setCurrentItem(std::clamp<std::size_t>(current, 1, debug_ui_.breakpointRows().size()));
  sidebar_tabs_.redrawCurrentPage();
}

void IdeWindow::openSelectedBreakpoint() {
  const auto index = breakpoints_.currentItem();
  const auto* row = index > 0 ? debug_ui_.breakpointRow(index - 1) : nullptr;
  if (!row) return;
  const auto breakpoint = row->breakpoint;
  openFile(breakpoint.file);
  if (document_ && document_->path() == normalizePath(breakpoint.file))
    editor_.reveal({breakpoint.line - 1, 0});
}

void IdeWindow::editSelectedBreakpoint() {
  const auto index = breakpoints_.currentItem();
  const auto* row = index > 0 ? debug_ui_.breakpointRow(index - 1) : nullptr;
  if (!row) {
    publishEvent(EventSource::Debug, EventSeverity::Warning, "Breakpoint properties unavailable: select a breakpoint in the Breakpoints panel\n"); return;
  }
  auto breakpoint = row->breakpoint;
  delTimer(timer_id_); BreakpointSettingsDialog dialog(breakpoint, this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept; timer_id_ = addTimer(100);
  if (!accepted) return;
  std::string error;
  if (!dialog.apply(breakpoint, error)) { finalcut::FMessageBox::error(this, finalcut::FString(error)); return; }
  if (!gdb_.updateBreakpoint(breakpoint)) { publishEvent(EventSource::Debug, EventSeverity::Error, "Breakpoint update failed\n"); return; }
  debug_state_dirty_ = true; saveDebugState(); debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
}

void IdeWindow::toggleSelectedBreakpoint() {
  const auto index = breakpoints_.currentItem();
  const auto* row = index > 0 ? debug_ui_.breakpointRow(index - 1) : nullptr;
  if (!row) {
    publishEvent(EventSource::Debug, EventSeverity::Warning, "Enable breakpoint unavailable: select a breakpoint in the Breakpoints panel\n"); return;
  }
  auto breakpoint = row->breakpoint; breakpoint.enabled = !breakpoint.enabled;
  if (!gdb_.updateBreakpoint(breakpoint)) return;
  debug_state_dirty_ = true; saveDebugState(); debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
}

void IdeWindow::removeSelectedBreakpoint() {
  const auto index = breakpoints_.currentItem();
  const auto* row = index > 0 ? debug_ui_.breakpointRow(index - 1) : nullptr;
  if (!row) {
    publishEvent(EventSource::Debug, EventSeverity::Warning, "Remove breakpoint unavailable: select a breakpoint in the Breakpoints panel\n"); return;
  }
  const auto breakpoint = row->breakpoint;
  if (!gdb_.removeBreakpoint(breakpoint.file, breakpoint.line)) return;
  debug_state_dirty_ = true; saveDebugState(); debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
}

void IdeWindow::clearBreakpoints() {
  if (gdb_.breakpoints().empty()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Remove all breakpoints unavailable: no breakpoints exist\n"); return; }
  const auto answer = finalcut::FMessageBox::info(this, "Remove all breakpoints",
    "Remove every breakpoint in this project?", finalcut::FMessageBox::ButtonType::Yes,
    finalcut::FMessageBox::ButtonType::No, finalcut::FMessageBox::ButtonType::Reject);
  if (answer != finalcut::FMessageBox::ButtonType::Yes) return;
  gdb_.clearBreakpoints(); debug_state_dirty_ = true; saveDebugState();
  debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
}

void IdeWindow::refreshOutline(std::optional<LspDocumentSymbols> response) {
  const auto active = document_ && isCppSource(document_->path())
    ? std::optional<LspDocumentIdentity>{{document_->path(), document_->version()}}
    : std::nullopt;
  if (response) {
    if (lsp_ui_.acceptOutline({response->path, response->version}, active)) {
      outline_.clear(); outline_positions_.clear();
      const auto kindName = [](int kind) -> std::string_view {
        switch (kind) {
          case 2: return "module"; case 3: return "namespace"; case 5: return "class";
          case 6: return "method"; case 7: return "property"; case 8: return "field";
          case 9: return "constructor"; case 10: return "enum"; case 11: return "interface";
          case 12: return "function"; case 13: return "variable"; case 14: return "constant";
          case 22: return "enum member"; case 23: return "struct"; case 26: return "type parameter";
          default: return "symbol";
        }
      };
      for (const auto& symbol : response->symbols) {
        auto label = std::string(symbol.depth * 2, ' ') + symbol.name + "  [" + std::string(kindName(symbol.kind)) + "]";
        if (!symbol.detail.empty() && symbol.detail != symbol.name) label += " — " + symbol.detail;
        outline_.insert(finalcut::FString(label)); outline_positions_.push_back(symbol.position);
      }
      if (outline_positions_.empty()) outline_.insert("No symbols in this document");
      sidebar_tabs_.redrawCurrentPage();
    }
  }
  const auto decision = lsp_ui_.updateOutline(active, lsp_.ready(), maintenance_ticks_);
  if (decision == OutlineDecision::Clear) {
    outline_.clear();
    outline_.insert(active ? "Waiting for outline..." : "Open a C/C++ file for outline");
    outline_positions_.clear();
    sidebar_tabs_.redrawCurrentPage();
  } else if (decision == OutlineDecision::Request && document_) {
    outline_.clear(); outline_.insert("Loading outline..."); outline_positions_.clear();
    sidebar_tabs_.redrawCurrentPage();
    lsp_.requestDocumentSymbols(*document_);
  }
}

void IdeWindow::openSelectedOutlineSymbol() {
  const auto selected = outline_.currentItem();
  if (!document_ || !lsp_ui_.renderedOutlineMatches({document_->path(), document_->version()})
      || selected == 0 || selected > outline_positions_.size()) return;
  auto position = outline_positions_[selected - 1];
  position.column = document_->byteColumn(position.line, position.column);
  editor_.reveal(position); editor_.setFocus(); finalcut::FWidget::setFocusWidget(&editor_); updateStatus();
}

void IdeWindow::addWatch() {
  if (root_.empty() && gdb_.mode() != DebugSessionMode::Core) {
    publishEvent(EventSource::Debug, EventSeverity::Warning, "Add watch unavailable: no project is open\n"); return;
  }
  const auto expression = prompt("Add watch", "Expression:");
  if (expression.empty()) return;
  if (!gdb_.addWatch(expression)) publishEvent(EventSource::Debug, EventSeverity::Warning, "Watch already exists or is empty: " + expression + "\n");
  else { debug_state_dirty_ = true; saveDebugState(); }
  debug_ui_.invalidateDebug();
  refreshDebugPanel();
}

void IdeWindow::evaluateExpression() {
  if (!gdb_.stopped()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Evaluate expression unavailable: debugger is not stopped\n"); return; }
  const auto expression = prompt("Evaluate expression", "Expression:");
  if (expression.empty()) return;
  if (!gdb_.evaluate(expression)) publishEvent(EventSource::Debug, EventSeverity::Warning, "Evaluate expression failed: expression is empty or debugger is unavailable\n");
  else publishEvent(EventSource::Debug, EventSeverity::Information, "Evaluating: " + expression + "\n");
}

void IdeWindow::editVariableValue() {
  if (!gdb_.stopped()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Set variable unavailable: debugger is not stopped\n"); return; }
  if (gdb_.mode() == DebugSessionMode::Core) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Set variable unavailable: core dumps are read-only\n");
    return;
  }
  std::string expression;
  const auto selected = debug_.currentItem();
  const auto* row = selected > 0 ? debug_ui_.debugRow(selected - 1) : nullptr;
  if (row && row->variable_index) {
    const auto variable = *row->variable_index;
    if (variable < gdb_.variables().size()) expression = gdb_.variables()[variable].expression;
  }
  if (expression.empty()) expression = prompt("Set variable value", "Expression:");
  if (expression.empty()) return;
  const auto value = prompt("Set variable value", "New value for " + expression + ":");
  if (value.empty()) return;
  if (!gdb_.assign(expression, value)) publishEvent(EventSource::Debug, EventSeverity::Warning, "Set variable failed: debugger is unavailable\n");
  else publishEvent(EventSource::Debug, EventSeverity::Information, "Assigning " + expression + " = " + value + "\n");
}

void IdeWindow::showDisassembly() {
  if (!gdb_.stopped()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Disassembly unavailable: debugger is not stopped\n"); return; }
  const auto address = prompt("Disassembly", "Address/expression ($pc):");
  if (address.empty()) return;
  if (!gdb_.disassemble(address)) publishEvent(EventSource::Debug, EventSeverity::Information, "Disassembly request rejected\n");
  else publishEvent(EventSource::Debug, EventSeverity::Information, "Disassembling near " + address + "\n");
}

void IdeWindow::manageSignals() {
  if (gdb_.backend() != DebugBackend::GdbMi || !gdb_.running()
      || gdb_.mode() == DebugSessionMode::Core
      || !gdb_.active() || !gdb_.stopped()) return;
  const auto action = choose("Signals", {"Inspect policies (Output)", "Configure handling", "Send signal and continue"});
  if (!action) return;
  if (action == 1) {
    if (gdb_.inspectSignals()) lower_tabs_.setCurrentIndex(0, true);
    return;
  }
  auto signals = gdbSignals();
  if (action == 3) {
    signals.insert(signals.begin(), "0 (suppress pending signal)");
    signals.push_back("SIGINT"); signals.push_back("SIGTRAP");
  }
  const auto selected = choose("Select signal", signals);
  if (!selected) return;
  const auto signal = action == 3 && selected == 1 ? std::string("0") : signals[selected - 1];
  if (action == 3) {
    // signal продолжает процесс и может завершить его: обязательно подтверждение.
    const auto confirmation = choose("Send " + signal + " and resume? May terminate program",
      {"Cancel", "Send and continue"});
    if (confirmation != 2) return;
    if (gdb_.sendSignal(signal))
      publishEvent(EventSource::Debug, EventSeverity::Information, "Sending " + signal + " and continuing\n");
    return;
  }
  const auto policy = choose("Handling of " + signal, {
    "Stop, print, pass", "Stop, print, suppress", "No stop, print, pass",
    "No stop, print, suppress", "No stop, silent, pass", "No stop, silent, suppress"});
  if (!policy) return;
  if (gdb_.setSignalPolicy(signal, policy <= 2, policy <= 4, policy % 2 == 1)) {
    lower_tabs_.setCurrentIndex(0, true);
    publishEvent(EventSource::Debug, EventSeverity::Information,
      "Changing " + signal + " policy for this GDB session (see response below)\n");
  }
}

void IdeWindow::showMemory() {
  if (!gdb_.stopped()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Memory view unavailable: debugger is not stopped\n"); return; }
  const auto address = prompt("Memory view", "Address/expression ($sp):");
  if (address.empty()) return;
  const auto count_text = prompt("Memory view", "Bytes (1..4096):");
  if (count_text.empty()) return;
  std::size_t count{};
  try {
    std::size_t consumed{};
    count = static_cast<std::size_t>(std::stoull(count_text, &consumed));
    if (consumed != count_text.size()) count = 0;
  } catch (...) {}
  if (count == 0 || count > 4096) {
    publishEvent(EventSource::Debug, EventSeverity::Information, "Memory view: byte count must be between 1 and 4096\n"); return;
  }
  if (!gdb_.readMemory(address, count)) publishEvent(EventSource::Debug, EventSeverity::Information, "Memory view request rejected\n");
  else publishEvent(EventSource::Debug, EventSeverity::Information, "Reading " + std::to_string(count) + " byte(s) at " + address + "\n");
}

void IdeWindow::removeSelectedWatch() {
  const auto index = debug_.currentItem();
  const auto* row = index > 0 ? debug_ui_.debugRow(index - 1) : nullptr;
  if (!row || !row->watch_index) {
    publishEvent(EventSource::Debug, EventSeverity::Warning, "Remove watch unavailable: select a watch row in the Debug panel\n");
    return;
  }
  if (gdb_.removeWatch(*row->watch_index)) {
    debug_state_dirty_ = true; saveDebugState();
    debug_ui_.invalidateDebug();
    refreshDebugPanel();
  }
}

void IdeWindow::loadDebugState() {
  if (root_.empty() || session_file_.empty()) return;
  DebugSession session;
  std::string error;
  if (!loadDebugSession(session_file_, session, error)) {
    publishEvent(EventSource::Debug, EventSeverity::Error, "Debug session: " + error + "\n");
    return;
  }
  for (const auto& breakpoint : session.breakpoints) {
    const auto file = breakpoint.file.is_relative() ? root_ / breakpoint.file : breakpoint.file;
    if (gdb_.addBreakpoint(file, breakpoint.line)) {
      DebugBreakpoint restored;
      restored.file = file; restored.line = breakpoint.line; restored.enabled = breakpoint.enabled;
      restored.condition = breakpoint.condition; restored.hit_count = breakpoint.hit_count;
      restored.log_message = breakpoint.log_message;
      gdb_.updateBreakpoint(restored);
    }
  }
  for (const auto& expression : session.watches) gdb_.addWatch(expression);
  gdb_.setRegistersEnabled(session.registers_enabled);
  cmake_session_.restoreSelection(session.cmake_target, session.cmake_configuration,
    session.cmake_configure_preset, session.cmake_build_preset);
  sidebar_width_ = session.sidebar_width;
  lower_panel_height_ = session.lower_panel_height;
  layout();
  refreshCMakePresets(false);
  debug_ui_.invalidateDebug(); debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel();
}

void IdeWindow::saveDebugState() {
  if (root_.empty() || session_file_.empty()) {
    debug_state_dirty_ = false;
    return;
  }
  DebugSession session;
  for (const auto& breakpoint : gdb_.breakpoints()) session.breakpoints.push_back({breakpoint.file,
    breakpoint.line, breakpoint.enabled, breakpoint.condition, breakpoint.hit_count, breakpoint.log_message});
  for (const auto& watch : gdb_.watches()) session.watches.push_back(watch.expression);
  session.registers_enabled = gdb_.registersEnabled();
  session.cmake_target = cmake_session_.preferredTarget();
  session.cmake_configuration = cmake_session_.preferredConfiguration();
  session.cmake_configure_preset = cmake_session_.configurePreset();
  session.cmake_build_preset = cmake_session_.buildPreset();
  session.sidebar_width = sidebar_width_;
  session.lower_panel_height = lower_panel_height_;
  std::string error;
  if (!saveDebugSession(session_file_, session, error)) {
    publishEvent(EventSource::Debug, EventSeverity::Error, "Debug session: " + error + "\n");
    return;
  }
  debug_state_dirty_ = false;
}

void IdeWindow::openSelectedFrame() {
  const auto index = debug_.currentItem();
  const auto* row = index > 0 ? debug_ui_.debugRow(index - 1) : nullptr;
  if (!row) return;
  if (row->thread_id) {
    gdb_.selectThread(*row->thread_id);
    debug_ui_.invalidateDebug();
    return;
  }
  if (row->variable_index) {
    if (gdb_.toggleVariable(*row->variable_index)) debug_ui_.invalidateDebug();
    return;
  }
  if (row->frame_level) {
    if (gdb_.selectFrame(*row->frame_level)) debug_ui_.invalidateDebug();
  }
  if (row->file.empty() || row->line == 0) return;
  auto path = row->file.is_relative() ? root_ / row->file : row->file;
  path = std::filesystem::absolute(path).lexically_normal();
  openFile(path);
  if (document_ && document_->path() == path) editor_.reveal({row->line - 1, 0});
}

void IdeWindow::activateDocument(std::size_t index) {
  if (!document_session_.activate(index)) return;
  editor_.setDocument(document_);
  lsp_.setActiveDocument(document_);
  editor_.setDiagnostics(&lsp_.diagnostics());
  const bool execution_visible = execution_line_ != 0
    && document_->path() == execution_file_ && execution_line_ <= document_->lines().size();
  editor_.setExecutionLine(execution_visible
    ? std::optional<std::size_t>{execution_line_ - 1} : std::nullopt);
  tabs_.setCurrentItem(index + 1);
  editor_.setFocus();
  finalcut::FWidget::setFocusWidget(&editor_);
  if (isCppSource(document_->path()) && compilation_database_.available()
      && !compilation_database_.contains(document_->path())
      && compilation_database_warnings_.insert(document_->path()).second) {
    publishEvent(EventSource::Lsp, EventSeverity::Information, "Compilation database: " + document_->path().string()
      + " has no entry; clangd will use inferred fallback flags\n");
  }
  updateStatus();
}

void IdeWindow::newFile() {
  const auto created = document_session_.createUntitled();
  activateDocument(created.index);
  refreshTabs();
}

auto IdeWindow::closeAllDocuments() -> bool {
  while (!documents_.empty()) {
    activateDocument(documents_.size() - 1);
    const auto count = documents_.size();
    closeActiveDocument();
    if (documents_.size() == count) return false;
  }
  return true;
}

void IdeWindow::unloadProject() {
  const bool had_project = !root_.empty();
  clearRecovery(recovery_file_);
  if (debug_state_dirty_ && !root_.empty()) saveDebugState();
  lsp_.stop(); gdb_.stop(); build_session_.reset(); ctest_session_.clear(); ctest_preset_.clear();
  git_session_.setRoot({}); git_files_state_.clear(); git_panel_message_.clear(); refreshGitPanel();
  analysis_session_.prepare(); analysis_text_.clear(); sanitizer_build_dir_.clear(); sanitizer_target_.clear();
  analysis_launch_ = {}; analysis_environment_.clear(); analysis_data_file_.clear();
  run_session_.stop(); console_.setControlEnabled(false);
  gdb_.clearSessionState();
  gdb_.configure(DebugBackend::GdbMi);
  execution_file_.clear(); execution_line_ = 0;
  document_session_.clear();
  editor_.setDocument(nullptr);
  editor_.setDiagnostics(nullptr);
  project_session_.close();
  cmake_session_.reset();
  project_filter_.clear();
  search_query_.clear(); search_replacement_.clear(); search_options_ = {}; search_project_ = false;
  problems_filter_.clear(); problems_signature_.clear(); problems_text_.clear(); problem_rows_.clear();
  problems_.clear(); problems_.insert(localizedUiText(user_settings_.language, "No problems"));
  outline_.clear(); outline_.insert("Open a C/C++ file for outline"); outline_positions_.clear();
  refreshTestsPanel();
  event_log_.clear(); output_.clear(); build_output_.clear(); console_.clear(); analysis_output_.clear();
  applyShortcutAccelerators();
  editor_.setTheme(effectiveEditorTheme(user_settings_), user_settings_.colors);
  debug_ui_.reset(); debug_state_dirty_ = false;
  compilation_database_.clear(); compilation_database_warnings_.clear();
  lsp_ui_.reset();
  diagnostic_index_ = 0;
  refreshFiles(); refreshTabs(); refreshDebugPanel(); refreshBreakpointsPanel();
  setText("TUI IDE — No project"); updateStatus();
  if (had_project) {
    const bool clean = documents_.empty() && !document_
      && !lsp_.running() && lsp_.diagnostics().empty() && lsp_.semanticTokens().empty()
      && !build_session_.running() && !run_session_.running() && !run_session_.consoleRunning()
      && !ctest_session_.running() && ctest_session_.tests().empty()
      && !analysis_session_.running() && analysis_session_.diagnostics().empty()
      && !gdb_.running() && gdb_.breakpoints().empty() && gdb_.watches().empty()
      && cmake_session_.targets().empty() && cmake_session_.configurePresets().empty()
      && cmake_session_.buildPresets().empty() && !project_session_.open();
    publishEvent(EventSource::System, clean ? EventSeverity::Success : EventSeverity::Error,
      clean ? "Previous project state cleared\n" : "Project state cleanup incomplete\n");
    showNotification(clean ? "Previous project state cleared" : "Project state cleanup incomplete",
      clean ? NotificationKind::Success : NotificationKind::Error);
  }
}

auto IdeWindow::loadProject(std::filesystem::path root, std::filesystem::path build_directory) -> bool {
  ProjectSession next_project;
  ProjectOpenResult open_result;
  std::string open_error;
  if (!next_project.open(std::move(root), std::move(build_directory), open_result, open_error)) {
    publishEvent(EventSource::Project, EventSeverity::Error, "Open Project error: " + open_error + "\n");
    return false;
  }
  unloadProject();
  project_session_ = std::move(next_project);
  git_session_.setRoot(root_); git_panel_message_ = "Loading Git status...";
  refreshGitPanel(); (void)git_session_.startStatus();
  cmake_session_.reset(project_session_.buildDirectory());
  if (open_result.used_default_settings)
    publishEvent(EventSource::Project, EventSeverity::Warning, "Project settings: " + open_result.warning + "; defaults are used\n");
  applyShortcutAccelerators();
  editor_.setIndentation(project_settings_.tab_width, project_settings_.use_spaces);
  editor_.setTheme(effectiveEditorTheme(user_settings_), user_settings_.colors);
  setText("TUI IDE — C/C++ — " + root_.filename().string());
  lsp_ui_.reset();
  configureDebugger();
  loadDebugState();
  refreshCompilationDatabase(true);
  restartLanguageServer();
  refreshFiles(); refreshTestsPanel(); updateStatus(); restoreRecovery();
  std::string history_error;
  if (!rememberRecentProject(project_history_file_, root_, history_error))
    publishEvent(EventSource::Project, EventSeverity::Error, "Recent projects: " + history_error + "\n");
  recent_projects_ = loadRecentProjects(project_history_file_, history_error);
  if (!history_error.empty()) publishEvent(EventSource::Project, EventSeverity::Error, "Recent projects: " + history_error + "\n");
  publishEvent(EventSource::Project, EventSeverity::Success, "Project opened: " + root_.string() + "\n");
  return true;
}

void IdeWindow::closeProject() {
  if (root_.empty()) return;
  if (!closeAllDocuments()) return;
  unloadProject();
}

void IdeWindow::projectSettings() {
  if (root_.empty()) { publishEvent(EventSource::Project, EventSeverity::Warning, "Project Settings unavailable: no project is open\n"); return; }
  if (build_session_.running() || run_session_.running()
      || run_session_.consoleRunning() || gdb_.running()) {
    publishEvent(EventSource::Project, EventSeverity::Warning, "Project Settings unavailable while build, program, or debugger is running\n");
    return;
  }
  delTimer(timer_id_);
  ProjectSettingsDialog dialog(root_, project_settings_, this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) return;
  ProjectSettings updated;
  std::string error;
  if (!dialog.settings(updated, error) || !saveProjectSettings(root_, updated, error)
      || !updateProjectGitignore(root_, updated, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  if (debug_state_dirty_) saveDebugState();
  project_session_.applySettings(std::move(updated));
  cmake_session_.reset(project_session_.buildDirectory());
  configureDebugger();
  editor_.setIndentation(project_settings_.tab_width, project_settings_.use_spaces);
  editor_.setTheme(effectiveEditorTheme(user_settings_), user_settings_.colors);
  refreshCompilationDatabase(true);
  restartLanguageServer();
  publishEvent(EventSource::Project, EventSeverity::Success, "Project settings saved; build directory: " + build_dir_.string() + "\n");
  updateStatus();
}

void IdeWindow::manageToolchainKits() {
  if (root_.empty()) {
    publishEvent(EventSource::Project, EventSeverity::Warning,
      "Toolchain kits unavailable: no project is open\n");
    return;
  }
  if (build_session_.running() || analysis_session_.running() || ctest_session_.running()
      || run_session_.running() || run_session_.consoleRunning() || gdb_.running()) {
    publishEvent(EventSource::Project, EventSeverity::Warning,
      "Toolchain kits unavailable while another operation is active\n");
    return;
  }
  publishEvent(EventSource::Project, EventSeverity::Information,
    "Discovering GCC, Clang, CMake generators, debuggers, and toolchain files...\n");
  const std::string path = std::getenv("PATH") ? std::getenv("PATH") : "";
  const auto kits = discoverToolchainKits(path, root_);
  if (kits.empty()) {
    finalcut::FMessageBox::error(this,
      "No usable GCC/Clang pair or CMake toolchain file was found.");
    return;
  }
  std::vector<std::string> descriptions;
  descriptions.reserve(kits.size());
  for (const auto& kit : kits) descriptions.push_back(describeToolchainKit(kit));
  delTimer(timer_id_);
  SelectionDialog dialog("Toolchain kits", descriptions, this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted || dialog.selected() == 0 || dialog.selected() > kits.size()) return;
  const auto& kit = kits[dialog.selected() - 1];
  if (!kit.valid) {
    finalcut::FMessageBox::error(this, "The selected kit failed its version checks.");
    return;
  }
  auto updated = project_settings_;
  updated.kit = kit.name;
  updated.generator = kit.generator;
  updated.toolchain = kit.toolchain_file;
  updated.make_program = kit.make_program;
  updated.sysroot = kit.sysroot;
  updated.c_compiler = kit.c_compiler;
  updated.cpp_compiler = kit.cpp_compiler;
  updated.debugger_backend = kit.debugger_kind == "LLDB" ? "lldb-dap" : "gdb-mi";
  updated.debugger_adapter = kit.debugger_kind == "LLDB" ? kit.debugger : std::filesystem::path{};
  std::string error;
  if (!saveProjectSettings(root_, updated, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  project_session_.applySettings(std::move(updated));
  cmake_session_.reset(project_session_.buildDirectory());
  configureDebugger();
  refreshCompilationDatabase(true);
  restartLanguageServer();
  publishEvent(EventSource::Project, EventSeverity::Success,
    "Toolchain kit selected: " + project_settings_.kit + "\n");
  updateStatus();
}

void IdeWindow::configureDebugger() {
  const auto backend = parseDebugBackend(project_settings_.debugger_backend);
  auto adapter = project_settings_.debugger_adapter;
  if (backend == DebugBackend::LldbDap && adapter.empty() && external_tools_.lldb_dap)
    adapter = *external_tools_.lldb_dap;
  gdb_.configure(backend, std::move(adapter));
}

void IdeWindow::requestLanguageInsights() {
  if (!document_ || !isCppSource(document_->path())) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning,
      "Language insights unavailable: open a C or C++ source file\n");
    return;
  }
  if (!lsp_.ready()) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning,
      "Language insights unavailable: clangd is not ready\n");
    return;
  }
  const std::vector<std::string> choices{
    "Inlay hints", "Document highlights", "Folding ranges",
    "Selection ranges", "Code lens", "Include hierarchy"};
  delTimer(timer_id_);
  SelectionDialog dialog("clangd language insights", choices, this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) return;
  switch (dialog.selected()) {
    case 1: lsp_.requestInlayHints(*document_); break;
    case 2: lsp_.requestDocumentHighlights(*document_); break;
    case 3: lsp_.requestFoldingRanges(*document_); break;
    case 4: lsp_.requestSelectionRange(*document_, document_->cursor()); break;
    case 5: lsp_.requestCodeLens(*document_); break;
    case 6: lsp_.requestIncludeHierarchy(*document_); break;
    default: return;
  }
  publishEvent(EventSource::Lsp, EventSeverity::Information,
    "Requested " + choices[dialog.selected() - 1] + " from clangd\n");
}

void IdeWindow::launchSettings() {
  if (root_.empty()) { publishEvent(EventSource::Project, EventSeverity::Warning, "Launch configuration unavailable: no project is open\n"); return; }
  if (build_session_.running() || run_session_.running()
      || run_session_.consoleRunning() || gdb_.running()) {
    publishEvent(EventSource::Project, EventSeverity::Warning, "Launch configuration unavailable while build, program, or debugger is running\n");
    return;
  }
  refreshCMakeTargets();
  auto current = project_settings_;
  synchronizeActiveLaunchConfiguration(current);
  delTimer(timer_id_);
  LaunchConfigurationManagerDialog dialog(root_, current.launch_configurations,
    current.active_launch_configuration, cmake_session_.targets(), this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) return;
  std::string error;
  auto updated = project_settings_;
  updated.launch_configurations = dialog.configurations();
  if (!selectLaunchConfiguration(updated, dialog.selectedName())) {
    finalcut::FMessageBox::error(this, "The selected launch configuration is unavailable.");
    return;
  }
  if (!saveProjectSettings(root_, updated, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  project_settings_ = std::move(updated);
  publishEvent(EventSource::Project, EventSeverity::Success,
    "Active Run/Debug configuration: " + project_settings_.active_launch_configuration + "\n");
  updateStatus();
}

void IdeWindow::selectLaunchProfile() {
  if (root_.empty()) {
    publishEvent(EventSource::Project, EventSeverity::Warning,
      "Launch configuration unavailable: no project is open\n");
    return;
  }
  if (build_session_.running() || run_session_.running()
      || run_session_.consoleRunning() || gdb_.running()) {
    publishEvent(EventSource::Project, EventSeverity::Warning,
      "Launch configuration unavailable while build, program, or debugger is running\n");
    return;
  }
  auto current = project_settings_;
  synchronizeActiveLaunchConfiguration(current);
  std::vector<std::string> labels;
  labels.reserve(current.launch_configurations.size());
  for (const auto& item : current.launch_configurations) {
    const auto detail = !item.configuration.executable.empty()
      ? item.configuration.executable.string()
      : !item.configuration.target.empty() ? item.configuration.target : "current CMake target";
    labels.push_back(item.name + " — " + detail);
  }
  const auto selection = choose("Select Run/Debug configuration", labels);
  if (selection == 0 || selection > current.launch_configurations.size()) return;
  auto updated = std::move(current);
  const auto name = updated.launch_configurations[selection - 1].name;
  if (!selectLaunchConfiguration(updated, name)) return;
  std::string error;
  if (!saveProjectSettings(root_, updated, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString{error}); return;
  }
  project_settings_ = std::move(updated);
  publishEvent(EventSource::Project, EventSeverity::Success,
    "Active Run/Debug configuration: " + project_settings_.active_launch_configuration + "\n");
  updateStatus();
}

void IdeWindow::openProject() {
  const auto start = root_.empty() ? std::filesystem::current_path() : root_.parent_path();
  delTimer(timer_id_);
  ProjectDirectoryDialog dialog("Open CMake project", start, this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  const auto selected = dialog.selectedPath();
  timer_id_ = addTimer(100);
  if (!accepted) return;
  std::string validation_error;
  if (!validateProjectDirectory(selected, validation_error)) {
    std::error_code type_error;
    const auto directory = normalizePath(selected);
    if (std::filesystem::is_directory(directory, type_error)
        && !std::filesystem::exists(directory / "CMakeLists.txt", type_error)) {
      const auto answer = finalcut::FMessageBox::info(this, "Import source directory",
        "CMakeLists.txt was not found. Import the existing C/C++ sources and create one?",
        finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No,
        finalcut::FMessageBox::ButtonType::Reject);
      if (answer == finalcut::FMessageBox::ButtonType::Yes) importProject(directory);
      else publishEvent(EventSource::Project, EventSeverity::Warning, "Project import cancelled\n");
      return;
    }
    finalcut::FMessageBox::error(this, finalcut::FString(validation_error));
    return;
  }
  if (normalizePath(selected) == root_) {
    publishEvent(EventSource::Project, EventSeverity::Warning, "Open Project: this project is already open\n");
    return;
  }
  if (!closeAllDocuments()) { publishEvent(EventSource::Project, EventSeverity::Warning, "Open Project cancelled: an open document was not closed\n"); return; }
  (void)loadProject(selected);
}

void IdeWindow::importProject(const std::filesystem::path& directory) {
  auto suggested_name = directory.filename().string();
  std::replace_if(suggested_name.begin(), suggested_name.end(), [](unsigned char character) {
    return std::isalnum(character) == 0 && character != '_';
  }, '_');
  if (suggested_name.empty()) suggested_name = "imported_project";
  if (std::isdigit(static_cast<unsigned char>(suggested_name.front())) != 0)
    suggested_name.insert(suggested_name.begin(), '_');

  delTimer(timer_id_);
  ImportProjectDialog settings_dialog(suggested_name, this, user_settings_.language);
  const auto accepted = settings_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  auto options = settings_dialog.options();
  timer_id_ = addTimer(100);
  if (!accepted) { publishEvent(EventSource::Project, EventSeverity::Warning, "Project import cancelled\n"); return; }
  options.project_directory = normalizePath(directory);

  delTimer(timer_id_);
  ProjectDirectoryDialog build_dialog("Select import build directory", directory.parent_path(), this,
    user_settings_.language);
  const auto build_accepted = build_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  options.build_directory = normalizePath(build_dialog.selectedPath());
  timer_id_ = addTimer(100);
  if (!build_accepted) { publishEvent(EventSource::Project, EventSeverity::Warning, "Project import cancelled\n"); return; }

  ProjectImportPlan plan;
  std::string error;
  if (!planProjectImport(options, plan, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  std::ostringstream preview;
  preview << "Directory: " << options.project_directory.string()
          << "\nBuild directory: " << options.build_directory.string()
          << "\nIncluded files: " << plan.files.size()
          << " (C: " << plan.c_sources << ", C++: " << plan.cpp_sources
          << ", headers: " << plan.headers << ")\n\n"
          << "The following CMakeLists.txt will be created:\n\n" << plan.cmake_text;
  delTimer(timer_id_);
  ConfirmTextDialog preview_dialog("Import preview", preview.str(), this, user_settings_.language);
  const auto confirmed = preview_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!confirmed) { publishEvent(EventSource::Project, EventSeverity::Warning, "Project import cancelled after preview\n"); return; }
  if (!closeAllDocuments()) {
    publishEvent(EventSource::Project, EventSeverity::Warning, "Project import cancelled: an open document was not closed\n");
    return;
  }
  if (!createImportedProject(options, plan, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  if (loadProject(options.project_directory, options.build_directory)) {
    openFile(options.project_directory / "CMakeLists.txt");
    publishEvent(EventSource::Project, EventSeverity::Success, "Imported " + std::to_string(plan.files.size()) + " source and header files\n");
  }
}

void IdeWindow::openRecentProject() {
  std::string history_error;
  recent_projects_ = loadRecentProjects(project_history_file_, history_error);
  if (!history_error.empty()) { publishEvent(EventSource::Project, EventSeverity::Error, "Recent projects: " + history_error + "\n"); return; }
  if (recent_projects_.empty()) { publishEvent(EventSource::Project, EventSeverity::Warning, "Recent projects: no valid projects\n"); updateStatus(); return; }
  std::vector<std::string> labels;
  labels.reserve(recent_projects_.size());
  for (const auto& project : recent_projects_) labels.push_back(project.string());
  const auto selection = choose("Recent projects", labels);
  if (selection == 0 || selection > recent_projects_.size()) return;
  const auto selected = recent_projects_[selection - 1];
  if (selected == root_) { publishEvent(EventSource::Project, EventSeverity::Warning, "Open Project: this project is already open\n"); return; }
  if (!closeAllDocuments()) { publishEvent(EventSource::Project, EventSeverity::Warning, "Open Project cancelled: an open document was not closed\n"); return; }
  (void)loadProject(selected);
}

void IdeWindow::refreshRecentFilesMenu() {
  constexpr std::size_t history_limit{10};
  constexpr std::size_t empty_index{history_limit};
  constexpr std::size_t separator_index{history_limit + 1};
  constexpr std::size_t clear_index{history_limit + 2};
  if (recent_file_items_.empty()) {
    for (std::size_t index{}; index < history_limit; ++index) {
      auto item = std::make_unique<finalcut::FMenuItem>(
        finalcut::FString{"-"}, &file_menu_.recent_files);
      item->addCallback("clicked", [this, index] {
        if (index >= user_settings_.recent_files.size()) return;
        const auto path = user_settings_.recent_files[index];
        deferred_command_ = [this, path] { openRecentFile(path); };
      });
      recent_file_items_.push_back(std::move(item));
    }
    auto empty = std::make_unique<finalcut::FMenuItem>(
      finalcut::FString{localizedUiText(user_settings_.language, "(History is empty)")}, &file_menu_.recent_files);
    empty->setDisable();
    recent_file_items_.push_back(std::move(empty));
    auto separator = std::make_unique<finalcut::FMenuItem>(&file_menu_.recent_files);
    separator->setSeparator();
    recent_file_items_.push_back(std::move(separator));
    auto clear = std::make_unique<finalcut::FMenuItem>(
      finalcut::FString{localizedUiText(user_settings_.language, "&Clear history")}, &file_menu_.recent_files);
    clear->setStatusBarMessage(finalcut::FString(localizedUiText(user_settings_.language,
      "Remove all entries from Recent Files")));
    clear->addCallback("clicked", [this] {
      deferred_command_ = [this] { clearRecentFiles(); };
    });
    recent_file_items_.push_back(std::move(clear));
  }

  user_settings_.recent_files = normalizeRecentFiles(user_settings_.recent_files);
  for (std::size_t index{}; index < history_limit; ++index) {
    auto& item = *recent_file_items_[index];
    if (index >= user_settings_.recent_files.size()) {
      item.hide();
      continue;
    }
    const auto& path = user_settings_.recent_files[index];
    item.setText(finalcut::FString{std::to_string(index + 1) + "  " + escapeMenuLabel(path)});
    item.setStatusBarMessage(finalcut::FString{localizedUiText(user_settings_.language, "Open ") + path.string()});
    item.setEnable(true);
    item.show();
  }
  const bool empty = user_settings_.recent_files.empty();
  if (empty) {
    recent_file_items_[empty_index]->show();
    recent_file_items_[separator_index]->hide();
    recent_file_items_[clear_index]->hide();
  } else {
    recent_file_items_[empty_index]->hide();
    recent_file_items_[separator_index]->show();
    recent_file_items_[clear_index]->show();
  }
}

void IdeWindow::openRecentFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    std::erase(user_settings_.recent_files, normalizePath(path));
    persistUserSettings();
    refreshRecentFilesMenu();
    publishEvent(EventSource::Editor, EventSeverity::Warning,
      "Recent Files removed an unavailable path: " + path.string() + "\n");
    showNotification("Recent file is no longer available", NotificationKind::Warning);
    return;
  }
  openFile(path);
}

void IdeWindow::clearRecentFiles() {
  user_settings_.recent_files.clear();
  persistUserSettings();
  refreshRecentFilesMenu();
  publishEvent(EventSource::Editor, EventSeverity::Information,
    "Recent Files history cleared\n");
  showNotification("Recent Files history cleared", NotificationKind::Success);
}

void IdeWindow::persistUserSettings() {
  std::string error;
  if (!saveUserSettings(user_settings_file_, user_settings_, error))
    publishEvent(EventSource::System, EventSeverity::Error,
      "User settings: " + error + "\n");
}

void IdeWindow::newProject() {
  delTimer(timer_id_); NewProjectDialog settings_dialog(this, user_settings_.language);
  const auto accepted = settings_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  auto options = settings_dialog.options(); timer_id_ = addTimer(100);
  if (!accepted) return;
  const auto start = root_.empty() ? std::filesystem::current_path() : root_.parent_path();
  delTimer(timer_id_); ProjectDirectoryDialog project_dialog("Select empty project directory", start,
    this, user_settings_.language);
  const auto project_accepted = project_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  options.project_directory = project_dialog.selectedPath(); timer_id_ = addTimer(100);
  if (!project_accepted) return;
  delTimer(timer_id_); ProjectDirectoryDialog build_dialog("Select build directory",
    options.project_directory.parent_path(), this, user_settings_.language);
  const auto build_accepted = build_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  options.build_directory = build_dialog.selectedPath(); timer_id_ = addTimer(100);
  if (!build_accepted) return;
  if (normalizePath(options.project_directory) == normalizePath(options.build_directory)) {
    finalcut::FMessageBox::error(this, "Build directory must differ from the project directory."); return;
  }
  if (!closeAllDocuments()) return;
  std::string error;
  if (!createNewProject(options, error)) { finalcut::FMessageBox::error(this, finalcut::FString(error)); return; }
  if (loadProject(options.project_directory, options.build_directory)) {
    openFile(options.project_directory / "CMakeLists.txt");
    publishEvent(EventSource::Project, EventSeverity::Information, "Validating the generated project with CMake...\n");
    configure();
  }
}

void IdeWindow::createProjectFile() {
  if (root_.empty()) {
    finalcut::FMessageBox::error(this, finalcut::FString(localizedUiText(user_settings_.language,
      "Open or create a project first.")));
    return;
  }
  const auto changed_cmake = std::find_if(documents_.begin(), documents_.end(), [](const auto& open_document) {
    return isCMakePath(open_document->path()) && open_document->modified();
  });
  if (changed_cmake != documents_.end()) {
    finalcut::FMessageBox::error(this, finalcut::FString(localizedUiText(user_settings_.language,
      "Save modified CMake files before adding a project file.")));
    return;
  }
  const std::vector<ProjectTemplate> types{
    ProjectTemplate::CHeader, ProjectTemplate::CppHeader, ProjectTemplate::CSource,
    ProjectTemplate::CppSource, ProjectTemplate::CppClass
  };
  const auto selection = choose("New project file", {
    "C header (.h)", "C++ header (.hpp)", "C implementation (.c)",
    "C++ implementation (.cpp)", "C++ class (.hpp + .cpp)"
  });
  if (selection == 0 || selection > types.size()) return;
  auto start_directory = root_;
  if (files_.hasFocus()) {
    const auto* item = files_.getCurrentItem();
    const auto entry = item ? project_item_paths_.find(item) : project_item_paths_.end();
    if (entry != project_item_paths_.end()) {
      std::error_code type_error;
      start_directory = std::filesystem::is_directory(entry->second, type_error)
        ? entry->second : entry->second.parent_path();
    }
  }
  std::string preferred_target;
  if (const auto* selected = cmake_session_.selectedTarget()) preferred_target = selected->name;
  ProjectTemplateResult result;
  std::string error;
  bool created{};
  if (types[selection - 1] == ProjectTemplate::CppClass) {
    delTimer(timer_id_);
    ClassOptionsDialog dialog(project_settings_.cpp_header_extension, this,
      user_settings_.language);
    const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
    auto options = dialog.options();
    timer_id_ = addTimer(100);
    if (!accepted) return;
    if (!validateCppClassSettings(options, error)) {
      finalcut::FMessageBox::error(this, finalcut::FString(error));
      return;
    }
    const auto header_name = options.header_file_name.empty()
      ? options.class_name + "." + project_settings_.cpp_header_extension
      : options.header_file_name;
    const auto source_name = options.source_file_name.empty()
      ? options.class_name + ".cpp" : options.source_file_name;
    const auto header = chooseProjectSavePath("C++ class header path", start_directory, header_name);
    if (!header) return;
    const auto source = chooseProjectSavePath("C++ class implementation path",
      header->parent_path(), source_name);
    if (!source) return;
    std::error_code relative_error;
    options.header_path = std::filesystem::relative(*header, root_, relative_error);
    if (relative_error) { finalcut::FMessageBox::error(this, "Cannot resolve header path."); return; }
    options.source_path = std::filesystem::relative(*source, root_, relative_error);
    if (relative_error) { finalcut::FMessageBox::error(this, "Cannot resolve source path."); return; }
    created = createCppClassTemplate(root_, options, preferred_target, result, error);
  } else {
    static const std::vector<std::string> suggestions{"new_header.h", "new_header.hpp", "new_source.c", "new_source.cpp"};
    const auto selected_path = chooseProjectSavePath("New project file path", start_directory, suggestions[selection - 1]);
    if (!selected_path) return;
    std::error_code relative_error;
    const auto relative = std::filesystem::relative(*selected_path, root_, relative_error);
    if (relative_error) { finalcut::FMessageBox::error(this, "Cannot resolve project path."); return; }
    created = createProjectTemplate(root_, types[selection - 1], relative, preferred_target, result, error);
  }
  if (!created) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  for (auto& open_document : documents_) {
    if (open_document->path() != normalizePath(result.changed_cmake_file)) continue;
    if (!open_document->load(result.changed_cmake_file, error)) publishEvent(EventSource::Project, EventSeverity::Error, "CMake reload: " + error + "\n");
    if (open_document.get() == document_) editor_.setDocument(document_);
  }
  refreshFiles();
  publishEvent(EventSource::Project, EventSeverity::Success, "Project: created " + std::to_string(result.created_files.size()) + " file(s), added "
    + std::to_string(result.cmake_references_added) + " CMake reference(s) in "
    + result.changed_cmake_file.string() + "\n");
  if (!result.created_files.empty()) openFile(result.created_files.front());
}

auto IdeWindow::chooseProjectSavePath(std::string title, const std::filesystem::path& start,
                                      std::string suggested_name) -> std::optional<std::filesystem::path> {
  delTimer(timer_id_);
  ProjectPathDialog dialog(std::move(title), root_, start, std::move(suggested_name), this,
    user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  const auto selected = dialog.selectedPath();
  timer_id_ = addTimer(100);
  return accepted ? std::optional<std::filesystem::path>{selected} : std::nullopt;
}

void IdeWindow::showCMakeCompletion() {
  if (!document_ || !isCMakePath(document_->path())) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning, "CMake completion unavailable: the active document is not a CMake file\n");
    return;
  }
  const auto cursor = document_->cursor();
  const auto completions = completeCMake(document_->lines(), cursor.line, cursor.column);
  if (completions.empty()) { publishEvent(EventSource::Lsp, EventSeverity::Information, "CMake completion: no suggestions\n"); return; }
  const auto selection = choose("CMake completion", completions);
  if (selection == 0 || selection > completions.size() || !document_ || !isCMakePath(document_->path())) return;
  document_->replaceIdentifierBeforeCursor(completions[selection - 1]);
  editor_.invalidateSyntax();
  refreshTabs();
  updateStatus();
}

void IdeWindow::requestSignatureHelp() {
  if (!document_) { publishEvent(EventSource::Lsp, EventSeverity::Warning, "Signature help unavailable: no document is open\n"); return; }
  if (!isCppSource(document_->path())) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning, "Signature help unavailable: the active document is not a C/C++ source file\n"); return;
  }
  if (!lsp_.running()) { publishEvent(EventSource::Lsp, EventSeverity::Warning, "Signature help unavailable: clangd is not running\n"); return; }
  if (!lsp_.ready()) { publishEvent(EventSource::Lsp, EventSeverity::Warning, "Signature help unavailable: clangd is still initializing\n"); return; }
  lsp_.requestSignatureHelp(*document_);
}

void IdeWindow::requestCodeActions(bool organize_includes) {
  if (!document_ || !isCppSource(document_->path()) || !lsp_.ready()) {
    publishEvent(EventSource::Lsp, EventSeverity::Information, std::string(organize_includes ? "Organize Includes" : "Code Actions")
      + " unavailable: open a C/C++ file and wait for clangd\n");
    return;
  }
  if (organize_includes) {
    lsp_.requestOrganizeIncludes(*document_);
    publishEvent(EventSource::Lsp, EventSeverity::Information, "Organize Includes: requesting changes from clangd\n");
    return;
  }
  auto start = document_->cursor();
  auto end = start;
  if (const auto selection = editor_.selectedRange()) { start = selection->first; end = selection->second; }
  lsp_.requestCodeActions(*document_, start, end);
  publishEvent(EventSource::Lsp, EventSeverity::Information, "Code Actions: requesting fixes from clangd\n");
}

void IdeWindow::switchSourceHeader() {
  if (!document_ || !isCppSource(document_->path()) || !lsp_.ready()) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning, "Switch Header/Source unavailable: open a C/C++ file and wait for clangd\n");
    return;
  }
  lsp_.requestSwitchSourceHeader(*document_);
}

void IdeWindow::requestWorkspaceSymbols() {
  if (root_.empty() || !lsp_.ready()) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning, "Workspace Symbols unavailable: open a project and wait for clangd\n"); return;
  }
  const auto query = prompt("Workspace symbols", "Name or substring:");
  if (query.empty()) { publishEvent(EventSource::Lsp, EventSeverity::Warning, "Workspace Symbols cancelled: query is empty\n"); return; }
  lsp_.requestWorkspaceSymbols(query);
}

void IdeWindow::requestHierarchy(bool type_hierarchy) {
  if (!document_ || !isCppSource(document_->path()) || !lsp_.ready()) {
    publishEvent(EventSource::Lsp, EventSeverity::Information, std::string(type_hierarchy ? "Type Hierarchy" : "Call Hierarchy")
      + " unavailable: open a C/C++ file and wait for clangd\n"); return;
  }
  if (type_hierarchy) lsp_.requestTypeHierarchy(*document_);
  else lsp_.requestCallHierarchy(*document_);
}

void IdeWindow::navigateTo(const LspNavigationItem& item) {
  openFile(item.path);
  if (!document_ || document_->path() != normalizePath(item.path)) return;
  auto position = item.position;
  position.column = document_->byteColumn(position.line, position.column);
  editor_.reveal(position);
}

void IdeWindow::exitIde() {
  // FWidget::close() завершает приложение автоматически только пока Final Cut
  // считает этот виджет главным. Модальные окна могут нарушить это состояние,
  // поэтому после подтверждённого закрытия явно останавливаем event loop.
  if (!close()) return;
  lsp_.stop();
  gdb_.stop();
  build_session_.reset();
  run_session_.stop();
  console_.setControlEnabled(false);
  finalcut::FApplication::exit(EXIT_SUCCESS);
}

void IdeWindow::closeActiveDocument(bool remember) {
  if (!document_ || active_document_ >= documents_.size()) return;
  if (document_->modified()) {
    const auto answer = finalcut::FMessageBox::info(this, "Unsaved changes", "Save this document before closing?",
      finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No, finalcut::FMessageBox::ButtonType::Cancel);
    if (answer == finalcut::FMessageBox::ButtonType::Cancel) return;
    if (answer == finalcut::FMessageBox::ButtonType::Yes) {
      if (!save()) return;
    }
  }
  if (isCppSource(document_->path())) lsp_.close(*document_);
  document_session_.closeActive(remember);
  if (documents_.empty()) {
    editor_.setDocument(nullptr); lsp_.setActiveDocument(nullptr);
  } else activateDocument(active_document_);
  refreshTabs(); updateStatus();
}

void IdeWindow::closeOtherDocuments() {
  if (!document_ || documents_.size() < 2) return;
  auto* keep = document_;
  const auto original_count = documents_.size();
  for (std::size_t index = documents_.size(); index-- > 0;) {
    if (documents_[index].get() == keep) continue;
    activateDocument(index);
    const auto count = documents_.size();
    closeActiveDocument();
    if (documents_.size() == count) {
      const auto active = std::find_if(documents_.begin(), documents_.end(),
        [keep](const auto& open) { return open.get() == keep; });
      if (active != documents_.end())
        activateDocument(static_cast<std::size_t>(std::distance(documents_.begin(), active)));
      publishEvent(EventSource::Editor, EventSeverity::Warning, "Close Others cancelled\n");
      return;
    }
  }
  const auto active = std::find_if(documents_.begin(), documents_.end(),
    [keep](const auto& open) { return open.get() == keep; });
  if (active != documents_.end())
    activateDocument(static_cast<std::size_t>(std::distance(documents_.begin(), active)));
  publishEvent(EventSource::Editor, EventSeverity::Information, "Close Others: closed " + std::to_string(original_count - documents_.size())
    + " document(s)\n");
}

void IdeWindow::reopenClosedDocument() {
  if (closed_documents_.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Reopen Closed unavailable: history is empty\n"); return; }
  const auto closed = document_session_.takeLastClosed();
  if (!closed) return;
  std::error_code error;
  if (!std::filesystem::is_regular_file(closed->path, error)) {
    publishEvent(EventSource::Editor, EventSeverity::Error, "Reopen Closed failed: file no longer exists: " + closed->path.string() + "\n");
    updateStatus();
    return;
  }
  openFile(closed->path);
  if (document_ && document_->path() == closed->path) {
    editor_.reveal(closed->cursor);
    publishEvent(EventSource::Editor, EventSeverity::Success, "Reopened: " + closed->path.string() + "\n");
  } else {
    publishEvent(EventSource::Editor, EventSeverity::Error, "Reopen Closed failed: cannot open " + closed->path.string() + "\n");
  }
}

void IdeWindow::switchDocument(int direction) {
  if (documents_.size() < 2) {
    publishEvent(EventSource::Editor, EventSeverity::Warning, "Switch file unavailable: fewer than two documents are open\n");
    return;
  }
  const auto count = static_cast<int>(documents_.size());
  const auto current = static_cast<int>(active_document_);
  activateDocument(static_cast<std::size_t>((current + direction + count) % count));
}

void IdeWindow::openFile(const std::filesystem::path& path) {
  std::string error;
  const auto opened = document_session_.open(path, error);
  if (!opened) { finalcut::FMessageBox::error(this, finalcut::FString(error)); return; }
  if (opened->newly_loaded && isCppSource(opened->document->path())) lsp_.open(*opened->document);
  activateDocument(opened->index);
  refreshTabs();
  rememberRecentFile(user_settings_.recent_files, opened->document->path());
  persistUserSettings();
  refreshRecentFilesMenu();
}

auto IdeWindow::save(bool report) -> bool {
  if (!document_) return false;
  if (document_->path().empty()) return saveAs();
  std::string error;
  if (!document_->save(error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return false;
  }
  if (isCppSource(document_->path())) lsp_.change(*document_);
  rememberRecentFile(user_settings_.recent_files, document_->path());
  persistUserSettings();
  refreshRecentFilesMenu();
  refreshTabs(); updateStatus();
  if (report) publishEvent(EventSource::Editor, EventSeverity::Success,
    "Saved: " + document_->path().string() + "\n");
  return true;
}

auto IdeWindow::saveAllDocuments() -> bool {
  if (documents_.empty()) return true;
  const auto original = active_document_;
  std::size_t saved{};
  for (std::size_t index = 0; index < documents_.size(); ++index) {
    if (!documents_[index]->modified()) continue;
    activateDocument(index);
    if (!save(false)) {
      if (!documents_.empty()) activateDocument(std::min(original, documents_.size() - 1));
      publishEvent(EventSource::Editor, EventSeverity::Warning, "Save All cancelled; project action was not started\n");
      return false;
    }
    ++saved;
  }
  if (!documents_.empty()) activateDocument(std::min(original, documents_.size() - 1));
  if (saved > 0) publishEvent(EventSource::Editor, EventSeverity::Success, "Save All: saved " + std::to_string(saved) + " document(s)\n");
  return true;
}

auto IdeWindow::saveAs() -> bool {
  if (!document_) newFile();
  delTimer(timer_id_);
  const auto selected = finalcut::FFileDialog::fileSaveChooser(this, finalcut::FString(root_.string()), fileDialogFilter);
  timer_id_ = addTimer(100);
  if (selected.isEmpty()) return false;
  const auto target = normalizePath(selected.toString());
  const auto duplicate = std::find_if(documents_.begin(), documents_.end(), [this, &target](const auto& open_document) {
    return open_document.get() != document_ && open_document->path() == target;
  });
  if (duplicate != documents_.end()) {
    finalcut::FMessageBox::error(this, "This file is already open in another editor.");
    return false;
  }
  std::string error;
  if (!document_->saveAs(target, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return false;
  }
  editor_.setDocument(document_);
  if (isCppSource(document_->path())) lsp_.open(*document_);
  lsp_.setActiveDocument(document_);
  rememberRecentFile(user_settings_.recent_files, document_->path());
  persistUserSettings();
  refreshRecentFilesMenu();
  refreshFiles(); refreshTabs(); updateStatus();
  return true;
}

void IdeWindow::find() {
  SearchRequest initial{SearchAction::None, search_query_, search_replacement_, search_options_, search_project_};
  delTimer(timer_id_);
  SearchDialog dialog(initial, !root_.empty(), this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) return;
  auto request = dialog.request();
  search_query_ = std::move(request.query);
  search_replacement_ = std::move(request.replacement);
  search_options_ = request.options;
  search_project_ = request.project;
  if (search_query_.empty()) { publishEvent(EventSource::Editor, EventSeverity::Error, "Find error: search text is empty\n"); return; }
  switch (request.action) {
    case SearchAction::Next:
      if (search_project_) showProjectSearch(); else findNext(false);
      break;
    case SearchAction::Previous:
      if (search_project_) showProjectSearch(); else findNext(true);
      break;
    case SearchAction::Replace: replaceCurrent(); break;
    case SearchAction::ReplaceAll: replaceAll(search_project_); break;
    case SearchAction::FindAll:
      if (search_project_) showProjectSearch();
      else {
        std::string error;
        const auto matches = searchText(document_ ? document_->text() : std::string{}, search_query_,
          search_replacement_, search_options_, error);
        if (!error.empty()) publishEvent(EventSource::Editor, EventSeverity::Error, "Find error: " + error + "\n");
        else publishEvent(EventSource::Editor, EventSeverity::Information, "Find: " + std::to_string(matches.size()) + " match(es) in the active file\n");
      }
      break;
    case SearchAction::None: break;
  }
}

void IdeWindow::findNext(bool previous) {
  if (!document_) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Find unavailable: no document is open\n"); return; }
  if (search_query_.empty()) { find(); return; }
  std::string error;
  const auto matches = searchText(document_->text(), search_query_, search_replacement_, search_options_, error);
  if (!error.empty()) { publishEvent(EventSource::Editor, EventSeverity::Error, "Find error: " + error + "\n"); return; }
  if (matches.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Find: no matches for " + search_query_ + "\n"); return; }
  const auto before = [](const Position& left, const Position& right) {
    return left.line < right.line || (left.line == right.line && left.column < right.column);
  };
  const auto selected = editor_.selectedRange();
  const auto anchor = selected ? (previous ? selected->first : selected->second) : document_->cursor();
  const SearchMatch* match{};
  bool wrapped{};
  if (previous) {
    for (auto iterator = matches.rbegin(); iterator != matches.rend(); ++iterator) {
      if (!before(anchor, iterator->end) && (!selected || iterator->start != selected->first)) {
        match = &*iterator; break;
      }
    }
    if (!match) { match = &matches.back(); wrapped = true; }
  } else {
    for (const auto& candidate : matches) {
      if (!before(candidate.start, anchor) && (!selected || candidate.start != selected->first)) {
        match = &candidate; break;
      }
    }
    if (!match) { match = &matches.front(); wrapped = true; }
  }
  editor_.selectRange(match->start, match->end);
  publishEvent(EventSource::Editor, EventSeverity::Information, "Find: match " + std::to_string(static_cast<std::size_t>(match - matches.data()) + 1)
    + " of " + std::to_string(matches.size()) + (wrapped ? " (wrapped)\n" : "\n"));
  updateStatus();
}

void IdeWindow::replaceCurrent() {
  if (!document_) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Replace unavailable: no document is open\n"); return; }
  std::string error;
  const auto matches = searchText(document_->text(), search_query_, search_replacement_, search_options_, error);
  if (!error.empty()) { publishEvent(EventSource::Editor, EventSeverity::Error, "Replace error: " + error + "\n"); return; }
  const auto selected = editor_.selectedRange();
  const auto match = std::find_if(matches.begin(), matches.end(), [&](const auto& candidate) {
    return selected && candidate.start == selected->first && candidate.end == selected->second;
  });
  if (match == matches.end()) {
    publishEvent(EventSource::Editor, EventSeverity::Information, "Replace: select a match first; moving to the next match\n");
    findNext(false);
    return;
  }
  document_->replaceRange(match->start, match->end, match->replacement);
  editor_.reveal(document_->cursor());
  editor_.invalidateSyntax();
  if (isCppSource(document_->path())) lsp_.change(*document_);
  refreshTabs(); updateStatus();
  publishEvent(EventSource::Editor, EventSeverity::Information, "Replace: changed 1 match\n");
  findNext(false);
}

void IdeWindow::replaceAll(bool project) {
  if (project) {
    if (root_.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Project replace unavailable: no project is open\n"); return; }
    struct PreparedFile {
      std::filesystem::path path;
      Document* target{};
      std::unique_ptr<Document> temporary;
      std::vector<SearchMatch> matches;
      std::string original_text;
    };
    std::vector<PreparedFile> prepared;
    std::size_t replacement_count{};
    std::size_t closed_count{};
    std::ostringstream preview;
    for (const auto& path : file_paths_) {
      PreparedFile file;
      file.path = normalizePath(path);
      for (auto& open : documents_) if (open->path() == file.path) { file.target = open.get(); break; }
      std::string error;
      if (!file.target) {
        file.temporary = std::make_unique<Document>();
        if (!file.temporary->load(file.path, error)) {
          publishEvent(EventSource::Editor, EventSeverity::Error, "Project replace error: " + error + "; no files changed\n"); return;
        }
        file.target = file.temporary.get();
        file.original_text = file.target->text();
      }
      file.matches = searchText(file.target->text(), search_query_, search_replacement_, search_options_, error);
      if (!error.empty()) { publishEvent(EventSource::Editor, EventSeverity::Error, "Project replace error: " + error + "; no files changed\n"); return; }
      if (file.matches.empty()) continue;
      std::error_code relative_error;
      auto relative = std::filesystem::relative(file.path, root_, relative_error);
      if (relative_error) relative = file.path;
      preview << (file.temporary ? "[disk] " : "[open] ") << relative.generic_string()
              << " — " << file.matches.size() << " match(es)\n";
      replacement_count += file.matches.size();
      if (file.temporary) ++closed_count;
      prepared.push_back(std::move(file));
    }
    if (prepared.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Project replace: no matches\n"); return; }
    std::ostringstream message;
    message << "Replace " << replacement_count << " match(es) in " << prepared.size() << " project file(s)?\n";
    if (closed_count != 0) message << closed_count << " [disk] file(s) will be saved immediately.\n";
    message << "Open files remain unsaved.\n\n" << preview.str();
    delTimer(timer_id_);
    ConfirmTextDialog dialog("Project replace preview", message.str(), this, user_settings_.language);
    const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
    timer_id_ = addTimer(100);
    if (!accepted) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Project replace cancelled; no files changed\n"); return; }

    for (auto& file : prepared) {
      if (!file.temporary) continue;
      std::vector<TextReplacement> replacements;
      replacements.reserve(file.matches.size());
      for (const auto& match : file.matches) replacements.push_back({match.start, match.end, match.replacement});
      file.target->applyReplacements(std::move(replacements));
    }
    std::vector<PreparedFile*> saved;
    for (auto& file : prepared) {
      if (!file.temporary) continue;
      std::string error;
      if (!file.target->save(error)) {
        bool rollback_failed{};
        file.target->setText(file.original_text);
        std::string rollback_error;
        if (!file.target->save(rollback_error)) rollback_failed = true;
        for (auto* previous : saved) {
          previous->target->setText(previous->original_text);
          if (!previous->target->save(rollback_error)) rollback_failed = true;
        }
        publishEvent(EventSource::Editor, EventSeverity::Error, "Project replace error: " + error + "; open files were not changed"
          + std::string(rollback_failed ? "; disk rollback failed\n" : "; disk files were restored\n"));
        return;
      }
      saved.push_back(&file);
    }
    for (auto& file : prepared) {
      if (file.temporary) continue;
      std::vector<TextReplacement> replacements;
      replacements.reserve(file.matches.size());
      for (const auto& match : file.matches) replacements.push_back({match.start, match.end, match.replacement});
      file.target->applyReplacements(std::move(replacements));
      if (isCppSource(file.path)) lsp_.change(*file.target);
    }
    editor_.invalidateSyntax(); refreshTabs(); updateStatus();
    publishEvent(EventSource::Editor, EventSeverity::Information, "Project replace: changed " + std::to_string(replacement_count) + " match(es) in "
      + std::to_string(prepared.size()) + " file(s)\n");
    return;
  } else {
    if (!document_) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Replace all unavailable: no document is open\n"); return; }
    std::string error;
    const auto matches = searchText(document_->text(), search_query_, search_replacement_, search_options_, error);
    if (!error.empty()) { publishEvent(EventSource::Editor, EventSeverity::Error, "Replace all error: " + error + "\n"); return; }
    if (matches.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Replace all: no matches\n"); return; }
    std::vector<TextReplacement> replacements;
    replacements.reserve(matches.size());
    for (const auto& match : matches) replacements.push_back({match.start, match.end, match.replacement});
    document_->applyReplacements(std::move(replacements));
    editor_.reveal(document_->cursor()); editor_.invalidateSyntax();
    if (isCppSource(document_->path())) lsp_.change(*document_);
    refreshTabs(); updateStatus();
    publishEvent(EventSource::Editor, EventSeverity::Information, "Replace all: changed " + std::to_string(matches.size()) + " match(es) in the active file\n");
    return;
  }
}

void IdeWindow::showProjectSearch() {
  if (root_.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Project search unavailable: no project is open\n"); return; }
  struct Result { std::filesystem::path path; SearchMatch match; };
  std::vector<Result> results;
  std::vector<std::string> labels;
  for (const auto& path : file_paths_) {
    Document temporary;
    Document* source = &temporary;
    for (auto& open : documents_) if (open->path() == normalizePath(path)) { source = open.get(); break; }
    std::string error;
    if (source == &temporary && !temporary.load(path, error)) { publishEvent(EventSource::Editor, EventSeverity::Error, "Project search: " + error + "\n"); continue; }
    auto matches = searchText(source->text(), search_query_, search_replacement_, search_options_, error);
    if (!error.empty()) { publishEvent(EventSource::Editor, EventSeverity::Error, "Project search error: " + error + "\n"); return; }
    std::error_code relative_error;
    auto relative = std::filesystem::relative(path, root_, relative_error);
    if (relative_error) relative = path;
    for (auto& match : matches) {
      std::string excerpt = source->line(match.start.line);
      if (excerpt.size() > 72) excerpt = excerpt.substr(0, 69) + "...";
      labels.push_back(relative.generic_string() + ":" + std::to_string(match.start.line + 1) + ":"
        + std::to_string(match.start.column + 1) + "  " + excerpt);
      results.push_back({path, std::move(match)});
    }
  }
  if (results.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Project search: no matches for " + search_query_ + "\n"); return; }
  const auto selection = choose("Project search — " + std::to_string(results.size()) + " result(s)", labels);
  if (selection == 0 || selection > results.size()) return;
  const auto result = results[selection - 1];
  openFile(result.path);
  editor_.selectRange(result.match.start, result.match.end);
  updateStatus();
}

void IdeWindow::goToLine() {
  if (!document_) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Go to line unavailable: no document is open\n"); return; }
  const auto value = prompt("Go to line", "Line number:");
  if (value.empty()) return;
  try {
    const auto line = static_cast<std::size_t>(std::stoul(value));
    if (line == 0 || line > document_->lines().size()) {
      publishEvent(EventSource::Editor, EventSeverity::Error, "Go to line error: enter a value from 1 to " + std::to_string(document_->lines().size()) + "\n");
      return;
    }
    editor_.reveal({line - 1, 0});
    publishEvent(EventSource::Editor, EventSeverity::Information,
      "Go to line: " + std::to_string(line) + "\n");
  } catch (...) { publishEvent(EventSource::Editor, EventSeverity::Error, "Invalid line number\n"); }
}

void IdeWindow::showProblems() {
  refreshProblemsPanel();
  lower_tabs_.setCurrentIndex(1, true);
  if (problem_rows_.empty()) publishEvent(EventSource::Editor, EventSeverity::Warning, "No build, analysis, or clangd diagnostics\n");
}

void IdeWindow::refreshProblemsPanel() {
  std::string signature = problems_filter_;
  for (const auto& diagnostic : build_session_.diagnostics()) signature += diagnostic.path.string()
    + std::to_string(diagnostic.line) + std::to_string(diagnostic.column) + diagnostic.message;
  for (const auto& diagnostic : analysis_session_.diagnostics()) signature += diagnostic.path.string()
    + std::to_string(diagnostic.line) + std::to_string(diagnostic.column) + diagnostic.message;
  for (const auto& diagnostic : lsp_.diagnostics()) signature += diagnostic.path.string()
    + std::to_string(diagnostic.position.line) + std::to_string(diagnostic.position.column) + diagnostic.message;
  if (signature == problems_signature_) return;
  problems_signature_ = std::move(signature); problems_.clear(); problem_rows_.clear(); problems_text_.clear();
  const auto relative_label = [this](const std::filesystem::path& path) {
    std::error_code error;
    auto relative = std::filesystem::relative(path, root_, error);
    return error ? path.string() : relative.string();
  };
  const auto visible = [this](std::string label) {
    if (problems_filter_.empty()) return true;
    std::ranges::transform(label, label.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto filter = problems_filter_;
    std::ranges::transform(filter, filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return label.find(filter) != std::string::npos;
  };
  for (const auto& diagnostic : build_session_.diagnostics()) {
    const auto severity = diagnostic.severity == DiagnosticSeverity::Error ? "error" :
      (diagnostic.severity == DiagnosticSeverity::Warning ? "warning" : "note");
    auto label = std::string("[build ") + severity + "] " + relative_label(diagnostic.path) + ":"
      + std::to_string(diagnostic.line + 1) + ":" + std::to_string(diagnostic.column + 1) + " " + diagnostic.message;
    if (!visible(label)) continue;
    problems_.insert(finalcut::FString(label)); problems_text_ += label + '\n';
    problem_rows_.push_back({diagnostic.path, {diagnostic.line, diagnostic.column}, false, diagnostic.message});
  }
  for (const auto& diagnostic : analysis_session_.diagnostics()) {
    const auto severity = diagnostic.severity == DiagnosticSeverity::Error ? "error" :
      (diagnostic.severity == DiagnosticSeverity::Warning ? "warning" : "note");
    auto label = std::string("[analysis ") + severity + "] " + relative_label(diagnostic.path) + ":"
      + std::to_string(diagnostic.line + 1) + ":" + std::to_string(diagnostic.column + 1)
      + " " + diagnostic.message;
    if (!visible(label)) continue;
    problems_.insert(finalcut::FString(label)); problems_text_ += label + '\n';
    problem_rows_.push_back({diagnostic.path, {diagnostic.line, diagnostic.column}, false, diagnostic.message});
  }
  for (const auto& diagnostic : lsp_.diagnostics()) {
    const auto severity = diagnostic.severity == 1 ? "error" : diagnostic.severity == 2 ? "warning" : diagnostic.severity == 3 ? "info" : "hint";
    auto label = std::string("[clangd ") + severity + "] " + relative_label(diagnostic.path) + ":"
      + std::to_string(diagnostic.position.line + 1) + ":" + std::to_string(diagnostic.position.column + 1) + " " + diagnostic.message;
    if (!visible(label)) continue;
    problems_.insert(finalcut::FString(label)); problems_text_ += label + '\n';
    problem_rows_.push_back({diagnostic.path, diagnostic.position, true, diagnostic.message});
  }
  if (problem_rows_.empty()) problems_.insert(localizedUiText(user_settings_.language,
    problems_filter_.empty() ? "No problems" : "No matching problems"));
  lower_tabs_.redrawCurrentPage();
}

void IdeWindow::openSelectedProblem() {
  const auto selection = problems_.currentItem();
  if (selection == 0 || selection > problem_rows_.size()) return;
  auto problem = problem_rows_[selection - 1];
  openFile(problem.path);
  if (document_ && document_->path() == std::filesystem::absolute(problem.path).lexically_normal()) {
    if (problem.utf16) problem.position.column = document_->byteColumn(problem.position.line, problem.position.column);
    editor_.reveal(problem.position);
  }
  publishEvent(EventSource::Editor, EventSeverity::Information, "Problem: " + problem.message + "\n");
}

void IdeWindow::filterProblems() {
  delTimer(timer_id_);
  PromptDialog dialog("Filter Problems", "Text (empty clears):", this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) {
    publishEvent(EventSource::Editor, EventSeverity::Information, "Problems filter unchanged\n");
    return;
  }
  problems_filter_ = dialog.value();
  problems_signature_.clear(); refreshProblemsPanel(); lower_tabs_.setCurrentIndex(1, true);
  publishEvent(EventSource::Editor, EventSeverity::Information,
    problems_filter_.empty() ? "Problems filter cleared\n"
                             : "Problems filter: " + problems_filter_ + "\n");
}

void IdeWindow::clearLowerPanel() {
  const auto panel = lower_tabs_.currentIndex();
  switch (lower_tabs_.currentIndex()) {
    case 0: event_log_.clear(EventChannel::Output); output_.clear(); break;
    case 1:
      build_session_.clearDiagnostics(); analysis_session_.clearDiagnostics(); lsp_.clearDiagnostics();
      problems_signature_.clear(); refreshProblemsPanel(); break;
    case 2: event_log_.clear(EventChannel::Build); build_output_.clear(); break;
    case 3: console_.clear(); break;
    case 4: analysis_session_.clearDiagnostics(); analysis_text_.clear(); analysis_output_.clear();
      problems_signature_.clear(); refreshProblemsPanel(); break;
    default: break;
  }
  const std::string message = panel == 0 ? "Output cleared" : panel == 1 ? "Problems cleared"
    : panel == 2 ? "Build output cleared" : panel == 3 ? "Terminal cleared" : "Analysis output cleared";
  showNotification(message, NotificationKind::Information);
  publishEvent(EventSource::System, EventSeverity::Information, message + "\n");
}

void IdeWindow::copyLowerPanel() {
  std::string text;
  switch (lower_tabs_.currentIndex()) {
    case 0: text = event_log_.text(EventChannel::Output); break; case 1: text = problems_text_; break;
    case 2: text = event_log_.text(EventChannel::Build); break; case 3: text = console_.text(); break;
    case 4: text = analysis_text_; break;
    default: break;
  }
  if (text.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Copy panel unavailable: active panel is empty\n"); return; }
  lower_clipboard_.copy(text); publishEvent(EventSource::Editor, EventSeverity::Success, "Copied active lower panel\n");
}

void IdeWindow::formatDocument(bool selection_only) {
  if (!document_ || !isCppSource(document_->path())) {
    publishEvent(EventSource::Editor, EventSeverity::Warning, "Format unavailable: open a saved C or C++ source file\n");
    return;
  }
  std::optional<FormatLineRange> lines;
  if (selection_only) {
    const auto selection = editor_.selectedRange();
    if (!selection) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Format selection unavailable: no text is selected\n"); return; }
    auto last = selection->second.line;
    if (selection->second.column == 0 && last > selection->first.line) --last;
    lines = FormatLineRange{selection->first.line, last};
  }
  const auto original = document_->text();
  const auto result = clangFormat(original, document_->path(), lines);
  if (!result.success) {
    publishEvent(EventSource::Editor, EventSeverity::Error, std::string(selection_only ? "Format selection error: " : "Format document error: ")
      + result.error + "\n");
    return;
  }
  if (result.text == original) {
    publishEvent(EventSource::Editor, EventSeverity::Information, std::string(selection_only ? "Format selection" : "Format document") + ": no changes\n");
    return;
  }
  editor_.applyFormattedText(result.text);
  if (isCppSource(document_->path())) lsp_.change(*document_);
  refreshTabs(); updateStatus();
  publishEvent(EventSource::Editor, EventSeverity::Success, std::string(selection_only ? "Formatted selected lines" : "Formatted document") + " with clang-format\n");
}

void IdeWindow::checkExternalChanges() {
  for (std::size_t index = 0; index < documents_.size(); ++index) {
    auto& open = *documents_[index];
    if (open.path().empty()) continue;
    std::string error;
    const auto change = open.diskChange(error);
    if (change == DiskChange::Unchanged) continue;
    if (change == DiskChange::Unreadable) {
      publishEvent(EventSource::Editor, EventSeverity::Error, "File watch error for " + open.path().string() + ": " + error + "\n");
      continue;
    }
    activateDocument(index);
    const bool deleted = change == DiskChange::Deleted;
    const auto selection = choose(deleted ? "File deleted outside TUI IDE" : "File changed outside TUI IDE",
      deleted ? std::vector<std::string>{"Keep editor contents", "Compare editor with missing file"}
              : std::vector<std::string>{"Reload from disk", "Keep editor contents", "Compare editor with disk"});
    if (selection == 0) return;
    if (!deleted && selection == 1) {
      const auto cursor = open.cursor();
      if (!open.load(open.path(), error)) publishEvent(EventSource::Editor, EventSeverity::Error, "Reload error: " + error + "\n");
      else {
        editor_.setDocument(&open); editor_.reveal(cursor);
        if (isCppSource(open.path())) lsp_.change(open);
        publishEvent(EventSource::Editor, EventSeverity::Information, "Reloaded external changes: " + open.path().string() + "\n");
      }
    } else if ((!deleted && selection == 2) || (deleted && selection == 1)) {
      open.acknowledgeDiskState();
      publishEvent(EventSource::Editor, EventSeverity::Warning, std::string(deleted ? "Kept editor contents after external deletion: "
        : "Kept editor contents after external modification: ") + open.path().string() + "\n");
    } else {
      std::string disk_text = "<file does not exist>";
      if (!deleted) {
        Document disk;
        if (disk.load(open.path(), error)) disk_text = disk.text();
        else disk_text = "<cannot read file: " + error + ">";
      }
      showTextDialog("External comparison — " + open.path().filename().string(),
        "--- editor (unsaved view) ---\n" + open.text()
        + "\n\n--- disk ---\n" + disk_text);
      return;
    }
    refreshTabs(); updateStatus();
    return;
  }
}

void IdeWindow::autosaveRecovery() {
  if (recovery_file_.empty()) return;
  std::vector<RecoveryDocument> recovery;
  for (const auto& open : documents_) {
    if (open->modified()) recovery.push_back({open->path(), open->text(), open->cursor()});
  }
  std::string error;
  if (!saveRecovery(recovery_file_, recovery, error)) publishEvent(EventSource::System, EventSeverity::Error, "Recovery autosave error: " + error + "\n");
}

void IdeWindow::restoreRecovery() {
  if (recovery_file_.empty() || !std::filesystem::exists(recovery_file_)) return;
  std::vector<RecoveryDocument> recovery;
  std::string error;
  if (!loadRecovery(recovery_file_, recovery, error)) {
    publishEvent(EventSource::System, EventSeverity::Error, "Recovery error: " + error + "\n"); return;
  }
  if (recovery.empty()) { clearRecovery(recovery_file_); return; }
  const auto selection = choose("Crash recovery", {
    "Restore " + std::to_string(recovery.size()) + " autosaved document(s)",
    "Discard recovery data"
  });
  if (selection == 0) return;
  if (selection == 2) {
    clearRecovery(recovery_file_); publishEvent(EventSource::System, EventSeverity::Information, "Crash recovery discarded\n"); return;
  }
  for (const auto& saved : recovery) {
    Document* target{};
    if (!saved.path.empty()) {
      const auto normalized = normalizePath(saved.path);
      const auto existing = std::find_if(documents_.begin(), documents_.end(), [&](const auto& open) {
        return open->path() == normalized;
      });
      if (existing != documents_.end()) target = existing->get();
      else if (std::filesystem::exists(normalized)) { openFile(normalized); target = document_; }
      else {
        auto restored = std::make_unique<Document>(); restored->relocate(normalized);
        documents_.push_back(std::move(restored)); target = documents_.back().get();
      }
    } else {
      documents_.push_back(std::make_unique<Document>()); target = documents_.back().get();
    }
    target->restoreText(saved.text); target->setCursor(saved.cursor);
    const auto iterator = std::find_if(documents_.begin(), documents_.end(),
      [target](const auto& open) { return open.get() == target; });
    if (iterator != documents_.end()) activateDocument(static_cast<std::size_t>(std::distance(documents_.begin(), iterator)));
    if (isCppSource(target->path())) lsp_.change(*target);
  }
  clearRecovery(recovery_file_);
  refreshTabs(); updateStatus();
  publishEvent(EventSource::System, EventSeverity::Success, "Crash recovery restored " + std::to_string(recovery.size()) + " document(s)\n");
}

void IdeWindow::build() {
  if (root_.empty()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Build unavailable: no project is open\n"); return; }
  if (ctest_session_.running() || analysis_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Build unavailable: tests or analysis are running\n"); return; }
  if (build_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Build unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(true)) return;
  build_session_.begin(BuildOperation::Build, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  if (build_session_.stage() == BuildStage::Build) (void)startBuildStage(false);
  else (void)startConfigureStage();
}

void IdeWindow::configure() {
  if (root_.empty()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Configure unavailable: no project is open\n"); return; }
  if (ctest_session_.running() || analysis_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Configure unavailable: tests or analysis are running\n"); return; }
  if (build_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Configure unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(true)) return;
  build_session_.begin(BuildOperation::Configure, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  (void)startConfigureStage();
}

void IdeWindow::rebuild() {
  if (root_.empty()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Rebuild unavailable: no project is open\n"); return; }
  if (ctest_session_.running() || analysis_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Rebuild unavailable: tests or analysis are running\n"); return; }
  if (build_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Rebuild unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(true)) return;
  build_session_.begin(BuildOperation::Rebuild, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  if (build_session_.stage() == BuildStage::Clean) (void)startBuildStage(true);
  else (void)startConfigureStage();
}

void IdeWindow::clean() {
  if (root_.empty()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Clean unavailable: no project is open\n"); return; }
  if (ctest_session_.running() || analysis_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Clean unavailable: tests or analysis are running\n"); return; }
  if (build_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Clean unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(false)) return;
  build_session_.begin(BuildOperation::Clean, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  if (!build_session_.running()) {
    publishEvent(EventSource::Build, EventSeverity::Warning, "Clean unavailable: configure the project first\n");
    build_session_.reset();
    return;
  }
  (void)startBuildStage(true);
}

auto IdeWindow::beginBuildOperation(bool save_documents) -> bool {
  if (!external_tools_.cmake) {
    publishEvent(EventSource::Build, EventSeverity::Error,
      "CMake unavailable: install cmake or add it to PATH; configure and build cannot start.\n");
    return false;
  }
  if (save_documents && !saveAllDocuments()) {
    publishEvent(EventSource::Build, EventSeverity::Warning, "CMake operation cancelled because not all documents were saved\n"); return false;
  }
  if (!refreshCMakePresets()) return false;
  build_session_.prepare();
  event_log_.clear(EventChannel::Build); build_output_.clear();
  problems_signature_.clear(); refreshProblemsPanel(); diagnostic_index_ = 0;
  lower_tabs_.setCurrentIndex(2);
  return true;
}

auto IdeWindow::startPreLaunchBuild(BuildContinuation continuation) -> bool {
  if (!beginBuildOperation(false)) return false;
  build_session_.begin(BuildOperation::Build,
    std::filesystem::exists(build_dir_ / "CMakeCache.txt"), continuation);
  return build_session_.stage() == BuildStage::Build ? startBuildStage(false) : startConfigureStage();
}

auto IdeWindow::startConfigureStage() -> bool {
  std::string query_error;
  if (!createCMakeFileApiQuery(build_dir_, query_error)) publishEvent(EventSource::Build, EventSeverity::Error, "CMake model: " + query_error + "\n");
  build_session_.enterStage(BuildStage::Configure);
  showNotification("CMake configure started", NotificationKind::Information);
  std::string command_error;
  auto command = BuildCommandService::configure(root_, build_dir_, project_settings_,
    cmake_session_.configurePreset(), cmake_session_.configurePresets(), command_error);
  if (!command) {
    publishEvent(EventSource::Build, EventSeverity::Error, command_error + "\n");
    finishBuildOperation(2, "Configure");
    return false;
  }
  publishEvent(EventSource::Build, EventSeverity::Information, command->display + "\n", EventChannel::Build);
  if (!build_session_.start(std::move(command->arguments),
      command->working_directory, project_settings_.environment)) {
    publishEvent(EventSource::Build, EventSeverity::Error, "Failed to start CMake\n");
    finishBuildOperation(127, "Configure");
    return false;
  }
  updateStatus();
  return true;
}

auto IdeWindow::startBuildStage(bool clean_stage) -> bool {
  build_session_.enterStage(clean_stage ? BuildStage::Clean : BuildStage::Build);
  showNotification(clean_stage ? "CMake clean started" : "Build started", NotificationKind::Information);
  const auto jobs = std::to_string(std::max(1U, project_settings_.build_jobs));
  const CMakeTarget* build_target{};
  if (!clean_stage && build_session_.continuation() != BuildContinuation::None
      && !project_settings_.launch.target.empty())
    build_target = cmake_session_.targetNamed(project_settings_.launch.target);
  else if (!clean_stage) build_target = cmake_session_.selectedTarget();
  auto command = BuildCommandService::build(root_, build_dir_, project_settings_.build_jobs,
    cmake_session_.buildPreset(), build_target, clean_stage);
  publishEvent(EventSource::Build, EventSeverity::Information, std::string(clean_stage ? "Clean" : "Build") + " stage: " + jobs + " parallel jobs\n", EventChannel::Build);
  publishEvent(EventSource::Build, EventSeverity::Information, command.display + "\n", EventChannel::Build);
  if (!build_session_.start(std::move(command.arguments),
      command.working_directory, project_settings_.environment)) {
    publishEvent(EventSource::Build, EventSeverity::Error, std::string("Failed to start ") + (clean_stage ? "clean" : "build") + "\n");
    finishBuildOperation(127, clean_stage ? "Clean" : "Build");
    return false;
  }
  updateStatus();
  return true;
}

void IdeWindow::finishBuildOperation(int exit_code, std::string_view failed_stage) {
  const auto result = build_session_.finish(root_);
  if (result.diagnostics_added != 0) problems_signature_.clear();
  if (exit_code != 0 && !failed_stage.empty())
    publishEvent(EventSource::Build, EventSeverity::Error, std::string(failed_stage) + " failed\n");
  std::ostringstream summary;
  summary.setf(std::ios::fixed); summary.precision(1);
  summary << result.operation << " finished with exit code " << exit_code
    << " after " << result.elapsed << " s\n";
  publishEvent(EventSource::Build, EventSeverity::Information, summary.str(), EventChannel::Build);
  publishEvent(EventSource::Build,
    exit_code == 0 ? EventSeverity::Success : EventSeverity::Error, summary.str());
  showNotification(result.operation + (exit_code == 0 ? " completed successfully" : " failed (exit "
      + std::to_string(exit_code) + ")"), exit_code == 0 ? NotificationKind::Success : NotificationKind::Error,
      std::chrono::milliseconds{6000});
  if (exit_code == 0 && result.continuation != BuildContinuation::None) {
    deferred_command_ = result.continuation == BuildContinuation::Run
      ? std::function<void()>{[this] { startRun(); }}
      : std::function<void()>{[this] { startDebug(); }};
  }
  updateStatus();
}

void IdeWindow::cancelBuild() {
  if (!build_session_.running()) { publishEvent(EventSource::Build, EventSeverity::Warning, "Cancel Build unavailable: no CMake operation is running\n"); return; }
  const auto result = build_session_.cancel(root_);
  for (const auto& chunk : result.output) publishEvent(EventSource::Build, EventSeverity::Information, chunk, EventChannel::Build);
  if (result.diagnostics_added != 0) problems_signature_.clear();
  std::ostringstream message; message.setf(std::ios::fixed); message.precision(1);
  message << "CMake operation cancelled during " << result.stage
    << " after " << result.elapsed << " s\n";
  publishEvent(EventSource::Build, EventSeverity::Information, message.str(), EventChannel::Build);
  publishEvent(EventSource::Build, EventSeverity::Warning, message.str());
  showNotification("CMake operation cancelled", NotificationKind::Warning);
  updateStatus();
}

auto IdeWindow::refreshCMakePresets(bool report_error) -> bool {
  const auto result = cmake_session_.refreshPresets(root_, project_session_.buildDirectory());
  if (report_error && !result.configure_error.empty())
    publishEvent(EventSource::Build, EventSeverity::Error, "CMake presets: " + result.configure_error + "\n");
  if (report_error && !result.build_error.empty())
    publishEvent(EventSource::Build, EventSeverity::Error, "CMake build presets: " + result.build_error + "\n");
  if (report_error && !result.validation_error.empty())
    publishEvent(EventSource::Build, EventSeverity::Error, result.validation_error + "\n");
  return result.valid;
}

void IdeWindow::selectCMakePreset() {
  delTimer(timer_id_);
  CMakePresetManagerDialog dialog(root_, CMakePresetKind::Configure,
    cmake_session_.configurePreset(), this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  activateWindow();
  raiseWindow();
  setFocus();
  editor_.setFocus();
  finalcut::FWidget::setFocusWidget(&editor_);
  if (!accepted) {
    if (dialog.changed()) (void)refreshCMakePresets();
    return;
  }
  if (!refreshCMakePresets()) return;
  const auto name = dialog.selectedPreset();
  if (!cmake_session_.selectConfigurePreset(name, project_session_.buildDirectory())) return;
  debug_state_dirty_ = true;
  saveDebugState();
  publishEvent(EventSource::Build, EventSeverity::Information, "Selected configure preset: " + (cmake_session_.configurePreset().empty()
      ? std::string("none") : cmake_session_.configurePreset())
    + " (build directory " + build_dir_.string() + ")\n");
  refreshCompilationDatabase(true);
  restartLanguageServer();
  refreshFiles();
  updateStatus();
}

void IdeWindow::selectCMakeBuildPreset() {
  delTimer(timer_id_);
  CMakePresetManagerDialog dialog(root_, CMakePresetKind::Build,
    cmake_session_.buildPreset(), this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  activateWindow();
  raiseWindow();
  setFocus();
  editor_.setFocus();
  finalcut::FWidget::setFocusWidget(&editor_);
  if (!accepted) {
    if (dialog.changed()) (void)refreshCMakePresets();
    return;
  }
  if (!refreshCMakePresets()) return;
  const auto name = dialog.selectedPreset();
  if (!cmake_session_.selectBuildPreset(name, project_session_.buildDirectory())) return;
  if (!name.empty()) refreshFiles();
  debug_state_dirty_ = true;
  saveDebugState();
  publishEvent(EventSource::Build, EventSeverity::Information, "Selected build preset: " + (cmake_session_.buildPreset().empty()
      ? std::string("none") : cmake_session_.buildPreset())
    + "\n");
  refreshCompilationDatabase(true);
  restartLanguageServer();
  updateStatus();
}

void IdeWindow::refreshCMakeTargets() {
  std::string error;
  auto targets = loadCMakeExecutableTargets(build_dir_, error);
  cmake_session_.replaceTargets(std::move(targets));
  if (cmake_session_.targets().empty() && !error.empty()) publishEvent(EventSource::Build, EventSeverity::Error, "CMake model: " + error + "\n");
}

void IdeWindow::selectCMakeTarget() {
  refreshCMakeTargets();
  if (cmake_session_.targets().empty()) { publishEvent(EventSource::Build, EventSeverity::Information, "No executable CMake targets; build the project first (Ctrl+B)\n"); return; }
  std::vector<std::string> labels;
  labels.reserve(cmake_session_.targets().size());
  for (const auto& target : cmake_session_.targets()) {
    labels.push_back((target.configuration.empty() ? std::string{} : target.configuration + " / ")
      + target.name + " -> " + target.artifact.string());
  }
  const auto selection = choose("CMake executable target", labels);
  if (selection == 0 || selection > cmake_session_.targets().size()) return;
  if (!cmake_session_.selectTarget(selection - 1)) return;
  debug_state_dirty_ = true; saveDebugState();
  publishEvent(EventSource::Build, EventSeverity::Information, "Selected target: " + labels[selection - 1] + "\n");
  updateStatus();
}

void IdeWindow::discoverTests() {
  if (root_.empty()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "CTest discovery unavailable: no project is open\n");
    return;
  }
  if (!external_tools_.cmake || !std::filesystem::exists(build_dir_ / "CTestTestfile.cmake")) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "CTest discovery unavailable: configure a CMake project with tests first\n");
    return;
  }
  if (build_session_.running() || run_session_.running() || gdb_.running()
      || ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "CTest discovery unavailable while another build, run, debug, or test operation is active\n");
    return;
  }
  if (!ctest_session_.discover(root_, build_dir_)) {
    publishEvent(EventSource::Test, EventSeverity::Error,
      "CTest discovery: " + ctest_session_.lastError() + "\n");
    return;
  }
  tests_.clear(); tests_.insert("[~] Discovering tests...");
  sidebar_tabs_.setCurrentIndex(5, true);
  menu_state_.reset();
  publishEvent(EventSource::Test, EventSeverity::Information,
    "CTest discovery started in " + build_dir_.string() + "\n");
}

void IdeWindow::runAllTests() {
  if (ctest_session_.tests().empty()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Run Tests unavailable: discover tests first\n");
    return;
  }
  if (build_session_.running() || run_session_.running() || gdb_.running()
      || ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Run Tests unavailable while another operation is active\n");
    return;
  }
  if (!saveAllDocuments()) return;
  if (!ctest_session_.runAll(root_, build_dir_)) {
    publishEvent(EventSource::Test, EventSeverity::Error, "Failed to start CTest\n"); return;
  }
  sidebar_tabs_.setCurrentIndex(5);
  lower_tabs_.setCurrentIndex(0);
  menu_state_.reset(); refreshTestsPanel();
  publishEvent(EventSource::Test, EventSeverity::Information, "Running all CTest tests\n");
}

void IdeWindow::runSelectedTest() {
  const auto index = tests_.currentItem();
  if (index == 0 || index > ctest_session_.tests().size()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Run Selected Test unavailable: select a discovered test\n");
    return;
  }
  if (build_session_.running() || run_session_.running() || gdb_.running()
      || ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Run Selected Test unavailable while another operation is active\n");
    return;
  }
  if (!saveAllDocuments()) return;
  const auto name = ctest_session_.tests()[index - 1].name;
  if (!ctest_session_.runSelected(root_, build_dir_, name)) {
    publishEvent(EventSource::Test, EventSeverity::Error, "Failed to start CTest\n"); return;
  }
  lower_tabs_.setCurrentIndex(0); menu_state_.reset();
  publishEvent(EventSource::Test, EventSeverity::Information, "Running CTest: " + name + "\n");
}

void IdeWindow::rerunFailedTests() {
  if (root_.empty() || !std::filesystem::exists(build_dir_ / "Testing/Temporary/LastTestsFailed.log")) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Rerun Failed Tests unavailable: CTest has no failed-test log\n");
    return;
  }
  if (build_session_.running() || run_session_.running() || gdb_.running()
      || ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Rerun Failed Tests unavailable while another operation is active\n");
    return;
  }
  if (!ctest_session_.rerunFailed(root_, build_dir_)) {
    publishEvent(EventSource::Test, EventSeverity::Error, "Failed to start CTest\n"); return;
  }
  lower_tabs_.setCurrentIndex(0); menu_state_.reset();
  publishEvent(EventSource::Test, EventSeverity::Information, "Rerunning failed CTest tests\n");
}

void IdeWindow::selectCTestPreset() {
  if (root_.empty()) return;
  std::string error;
  const auto presets = loadCTestPresets(root_, error);
  if (!error.empty()) {
    publishEvent(EventSource::Test, EventSeverity::Error, "CTest presets: " + error + "\n"); return;
  }
  if (presets.empty()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "No visible testPresets in CMakePresets.json or CMakeUserPresets.json\n");
    return;
  }
  std::vector<std::string> labels;
  for (const auto& preset : presets)
    labels.push_back(preset.display_name + (preset.display_name == preset.name ? "" : " [" + preset.name + "]"));
  const auto selection = choose("CTest preset", labels);
  if (selection == 0 || selection > presets.size()) return;
  if (build_session_.running() || run_session_.running() || gdb_.running()
      || ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Test preset unavailable while another operation is active\n"); return;
  }
  ctest_preset_ = presets[selection - 1].name;
  if (!ctest_session_.runPreset(root_, ctest_preset_)) {
    publishEvent(EventSource::Test, EventSeverity::Error, "Failed to start CTest preset\n"); return;
  }
  lower_tabs_.setCurrentIndex(0); menu_state_.reset();
  publishEvent(EventSource::Test, EventSeverity::Information,
    "Running CTest preset: " + ctest_preset_ + "\n");
}

void IdeWindow::stopTests() {
  if (!ctest_session_.running()) {
    publishEvent(EventSource::Test, EventSeverity::Warning,
      "Stop Tests unavailable: CTest is not running\n"); return;
  }
  ctest_session_.stop(); menu_state_.reset(); refreshTestsPanel();
  publishEvent(EventSource::Test, EventSeverity::Warning, "CTest operation stopped\n");
}

void IdeWindow::refreshTestsPanel() {
  const auto selected = tests_.currentItem();
  tests_.clear();
  if (ctest_session_.tests().empty()) {
    tests_.insert(root_.empty() ? "No project" : "Tests not discovered");
  } else {
    for (const auto& test : ctest_session_.tests()) {
      std::string label{ctestStatusLabel(test.status)};
      label += " " + test.name;
      if (!test.labels.empty()) {
        label += "  [";
        for (std::size_t i{}; i < test.labels.size(); ++i) {
          if (i != 0) label += ",";
          label += test.labels[i];
        }
        label += "]";
      }
      tests_.insert(finalcut::FString(label));
    }
    if (selected > 0 && selected <= ctest_session_.tests().size()) tests_.setCurrentItem(selected);
  }
  sidebar_tabs_.redrawCurrentPage();
}

void IdeWindow::openSelectedTestFailure() {
  const auto index = tests_.currentItem();
  if (index == 0 || index > ctest_session_.tests().size()) return;
  const auto& test = ctest_session_.tests()[index - 1];
  if (test.failures.empty()) {
    publishEvent(EventSource::Test, EventSeverity::Information,
      "No source failure location recorded for " + test.name + "; press Space to run it\n");
    return;
  }
  const auto location = test.failures.front();
  openFile(location.path);
  if (document_ && document_->path() == location.path) editor_.reveal({location.line, 0});
  publishEvent(EventSource::Test, EventSeverity::Information,
    "Test failure: " + test.name + " at " + location.path.string() + ":"
      + std::to_string(location.line + 1) + "\n");
}

void IdeWindow::runAnalysis() {
  if (root_.empty()) {
    publishEvent(EventSource::Analysis, EventSeverity::Warning,
      "Analysis unavailable: no project is open\n"); return;
  }
  if (analysis_session_.running() || build_session_.running() || ctest_session_.running()
      || run_session_.running() || gdb_.running()) {
    publishEvent(EventSource::Analysis, EventSeverity::Warning,
      "Analysis unavailable while another build, test, run, debug, or analysis operation is active\n");
    return;
  }
  const std::vector<std::string> tools{
    "clang-tidy" + std::string(external_tools_.clang_tidy ? "" : "  [unavailable]"),
    "cppcheck" + std::string(external_tools_.cppcheck ? "" : "  [unavailable]"),
    "include-what-you-use" + std::string(external_tools_.include_what_you_use ? "" : "  [unavailable]"),
    "AddressSanitizer + UndefinedBehaviorSanitizer",
    "Coverage (gcovr)" + std::string(external_tools_.gcovr ? "" : "  [unavailable]"),
    "Valgrind Memcheck" + std::string(external_tools_.valgrind ? "" : "  [unavailable]"),
    "CPU profile (perf)" + std::string(external_tools_.perf ? "" : "  [unavailable]")};
  const auto tool_selection = choose("Analysis and profiling tool", tools);
  if (tool_selection == 0 || tool_selection > tools.size()) return;
  const auto tool = static_cast<AnalysisTool>(tool_selection - 1);
  const bool configured_build = tool == AnalysisTool::Sanitizers || tool == AnalysisTool::Coverage;
  const bool runtime_profile = tool == AnalysisTool::Valgrind || tool == AnalysisTool::Perf;
  const std::vector<std::string> scopes = configured_build
    ? std::vector<std::string>{"Selected CMake target", "Whole project"}
    : runtime_profile ? std::vector<std::string>{"Active Run/Debug configuration"}
    : std::vector<std::string>{"Active file", "Selected CMake target", "Whole project"};
  const auto scope_selection = choose("Analysis scope", scopes);
  if (scope_selection == 0 || scope_selection > scopes.size()) return;
  const auto scope = configured_build
    ? (scope_selection == 1 ? AnalysisScope::Target : AnalysisScope::Project)
    : runtime_profile ? AnalysisScope::Target : static_cast<AnalysisScope>(scope_selection - 1);

  std::vector<std::filesystem::path> sources;
  sanitizer_target_.clear();
  analysis_launch_ = {};
  analysis_environment_ = project_settings_.environment;
  for (const auto& [name, value] : project_settings_.launch.environment)
    analysis_environment_[name] = value;
  if (runtime_profile) {
    std::string launch_error;
    if (!launchCommand(analysis_launch_, launch_error)) {
      publishEvent(EventSource::Analysis, EventSeverity::Warning,
        "Profiler unavailable: " + launch_error + "\n"); return;
    }
    for (const auto& [name, value] : analysis_launch_.environment)
      analysis_environment_[name] = value;
  } else if (scope == AnalysisScope::File) {
    if (!document_ || document_->path().empty() || !isCppSource(document_->path())) {
      publishEvent(EventSource::Analysis, EventSeverity::Warning,
        "File analysis unavailable: open a saved C or C++ source file\n"); return;
    }
    sources.push_back(document_->path());
  } else if (scope == AnalysisScope::Target) {
    refreshCMakeTargets();
    if (!cmake_session_.selectedTarget()) selectCMakeTarget();
    const auto* target = cmake_session_.selectedTarget();
    if (!target) {
      publishEvent(EventSource::Analysis, EventSeverity::Warning,
        "Target analysis unavailable: select a CMake executable target\n"); return;
    }
    sanitizer_target_ = target->name;
    sources = target->sources;
    if (!compilation_database_.available()) refreshCompilationDatabase(true);
    std::erase_if(sources, [this](const auto& path) {
      return !isCppSource(path) || (compilation_database_.available()
        && !compilation_database_.contains(path));
    });
    if (!configured_build && sources.empty()) {
      publishEvent(EventSource::Analysis, EventSeverity::Warning,
        "Target analysis unavailable: CMake File API returned no C/C++ sources\n"); return;
    }
  } else {
    if (!compilation_database_.available()) refreshCompilationDatabase(true);
    sources.assign(compilation_database_.sources().begin(), compilation_database_.sources().end());
    std::ranges::sort(sources);
    if (!configured_build && sources.empty()) {
      publishEvent(EventSource::Analysis, EventSeverity::Warning,
        "Project analysis unavailable: configure the project to create compile_commands.json\n"); return;
    }
  }
  if (!saveAllDocuments()) return;

  std::optional<std::filesystem::path> executable;
  if (tool == AnalysisTool::ClangTidy) executable = external_tools_.clang_tidy;
  else if (tool == AnalysisTool::Cppcheck) executable = external_tools_.cppcheck;
  else if (tool == AnalysisTool::IncludeWhatYouUse) executable = external_tools_.include_what_you_use;
  else if (tool == AnalysisTool::Valgrind) executable = external_tools_.valgrind;
  else if (tool == AnalysisTool::Perf) executable = external_tools_.perf;
  else executable = external_tools_.cmake;
  if (tool == AnalysisTool::Coverage && !external_tools_.gcovr) {
    publishEvent(EventSource::Analysis, EventSeverity::Error,
      "Coverage is unavailable; install gcovr or add it to PATH\n"); return;
  }
  if (!executable) {
    publishEvent(EventSource::Analysis, EventSeverity::Error,
      std::string(analysisToolName(tool)) + " is unavailable; install it or add it to PATH\n");
    return;
  }

  analysis_session_.prepare(); analysis_text_.clear(); analysis_output_.clear();
  problems_signature_.clear(); diagnostic_index_ = 0;
  AnalysisCommand command;
  auto stage = AnalysisStage::Check;
  if (tool == AnalysisTool::Sanitizers || tool == AnalysisTool::Coverage) {
    sanitizer_build_dir_ = build_dir_ /
      (tool == AnalysisTool::Coverage ? ".tuiide-coverage" : ".tuiide-sanitizers");
    if (tool == AnalysisTool::Coverage) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(sanitizer_build_dir_, cleanup_error);
      if (cleanup_error) {
        publishEvent(EventSource::Analysis, EventSeverity::Error,
          "Cannot reset coverage build directory: " + cleanup_error.message() + "\n");
        return;
      }
    }
    std::string query_error;
    if (!createCMakeFileApiQuery(sanitizer_build_dir_, query_error)) {
      publishEvent(EventSource::Analysis, EventSeverity::Error,
        std::string(tool == AnalysisTool::Coverage ? "Coverage" : "Sanitizer")
          + " CMake model: " + query_error + "\n"); return;
    }
    if (tool == AnalysisTool::Coverage) {
      command = makeCoverageConfigureCommand(*executable, root_, sanitizer_build_dir_,
        project_settings_.generator, project_settings_.toolchain,
        project_settings_.make_program, project_settings_.sysroot, project_settings_.c_compiler,
        project_settings_.cpp_compiler);
      stage = AnalysisStage::CoverageConfigure;
    } else {
      command = makeSanitizerConfigureCommand(*executable, root_, sanitizer_build_dir_,
        project_settings_.generator, project_settings_.toolchain,
        project_settings_.make_program, project_settings_.sysroot,
        project_settings_.c_compiler, project_settings_.cpp_compiler);
      stage = AnalysisStage::SanitizerConfigure;
    }
  } else if (tool == AnalysisTool::Valgrind) {
    command = makeValgrindCommand(*executable, analysis_launch_.executable,
      analysis_launch_.arguments, analysis_launch_.working_directory);
    stage = AnalysisStage::ValgrindRun;
  } else if (tool == AnalysisTool::Perf) {
    analysis_data_file_ = build_dir_ / ".tuiide-perf.data";
    std::error_code remove_error;
    std::filesystem::remove(analysis_data_file_, remove_error);
    command = makePerfRecordCommand(*executable, analysis_data_file_,
      analysis_launch_.executable, analysis_launch_.arguments,
      analysis_launch_.working_directory);
    stage = AnalysisStage::PerfRecord;
  } else command = makeAnalysisCommand(tool, scope, *executable, root_, build_dir_, sources);
  analysis_text_ = "$ " + command.display + "\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  lower_tabs_.setCurrentIndex(4);
  if (!analysis_session_.start(std::move(command), tool, scope, stage,
      analysis_environment_)) {
    publishEvent(EventSource::Analysis, EventSeverity::Error,
      "Failed to start " + std::string(analysisToolName(tool)) + "\n"); return;
  }
  menu_state_.reset();
  publishEvent(EventSource::Analysis, EventSeverity::Information,
    "Started " + std::string(analysisToolName(tool)) + " for "
      + std::string(analysisScopeName(scope)) + "\n");
}

auto IdeWindow::startSanitizerBuild() -> bool {
  if (!external_tools_.cmake) return false;
  auto command = makeSanitizerBuildCommand(*external_tools_.cmake,
    sanitizer_build_dir_, project_settings_.build_jobs, sanitizer_target_);
  analysis_text_ += "$ " + command.display + "\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  return analysis_session_.start(std::move(command), AnalysisTool::Sanitizers,
    sanitizer_target_.empty() ? AnalysisScope::Project : AnalysisScope::Target,
    AnalysisStage::SanitizerBuild, analysis_environment_);
}

auto IdeWindow::startSanitizerRun() -> bool {
  std::string error;
  const auto targets = loadCMakeExecutableTargets(sanitizer_build_dir_, error);
  const auto found = std::ranges::find_if(targets, [this](const auto& target) {
    return target.name == sanitizer_target_;
  });
  if (found == targets.end()) {
    publishEvent(EventSource::Analysis, EventSeverity::Error,
      "Sanitizer executable unavailable for target " + sanitizer_target_
        + (error.empty() ? "" : ": " + error) + "\n");
    return false;
  }
  auto working_directory = project_settings_.launch.working_directory;
  if (working_directory.empty()) working_directory = found->artifact.parent_path();
  else if (working_directory.is_relative()) working_directory = root_ / working_directory;
  auto command = makeSanitizerRunCommand(found->artifact,
    project_settings_.launch.arguments, working_directory);
  analysis_text_ += "$ " + command.display + "\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  return analysis_session_.start(std::move(command), AnalysisTool::Sanitizers,
    AnalysisScope::Target, AnalysisStage::SanitizerRun,
    analysis_environment_);
}

auto IdeWindow::startCoverageBuild() -> bool {
  if (!external_tools_.cmake) return false;
  auto command = makeCoverageBuildCommand(*external_tools_.cmake,
    sanitizer_build_dir_, project_settings_.build_jobs, sanitizer_target_);
  analysis_text_ += "$ " + command.display + "\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  return analysis_session_.start(std::move(command), AnalysisTool::Coverage,
    sanitizer_target_.empty() ? AnalysisScope::Project : AnalysisScope::Target,
    AnalysisStage::CoverageBuild, analysis_environment_);
}

auto IdeWindow::startCoverageRun() -> bool {
  AnalysisCommand command;
  if (sanitizer_target_.empty()) {
    command = makeCoverageTestCommand("ctest", sanitizer_build_dir_);
  } else {
    std::string error;
    const auto targets = loadCMakeExecutableTargets(sanitizer_build_dir_, error);
    const auto found = std::ranges::find_if(targets, [this](const auto& target) {
      return target.name == sanitizer_target_;
    });
    if (found == targets.end()) {
      publishEvent(EventSource::Analysis, EventSeverity::Error,
        "Coverage executable unavailable for target " + sanitizer_target_
          + (error.empty() ? "" : ": " + error) + "\n");
      return false;
    }
    auto working_directory = project_settings_.launch.working_directory;
    if (working_directory.empty()) working_directory = found->artifact.parent_path();
    else if (working_directory.is_relative()) working_directory = root_ / working_directory;
    command = makeSanitizerRunCommand(found->artifact,
      project_settings_.launch.arguments, working_directory);
  }
  analysis_text_ += "$ " + command.display + "\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  return analysis_session_.start(std::move(command), AnalysisTool::Coverage,
    sanitizer_target_.empty() ? AnalysisScope::Project : AnalysisScope::Target,
    AnalysisStage::CoverageRun, analysis_environment_);
}

auto IdeWindow::startCoverageReport() -> bool {
  if (!external_tools_.gcovr) return false;
  analysis_data_file_ = sanitizer_build_dir_ / "tuiide-coverage.json";
  std::error_code remove_error;
  std::filesystem::remove(analysis_data_file_, remove_error);
  auto command = makeCoverageReportCommand(*external_tools_.gcovr, root_,
    sanitizer_build_dir_, analysis_data_file_);
  analysis_text_ += "$ " + command.display + "\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  return analysis_session_.start(std::move(command), AnalysisTool::Coverage,
    sanitizer_target_.empty() ? AnalysisScope::Project : AnalysisScope::Target,
    AnalysisStage::CoverageReport, analysis_environment_);
}

auto IdeWindow::startPerfReport() -> bool {
  if (!external_tools_.perf || analysis_data_file_.empty()) return false;
  auto command = makePerfReportCommand(*external_tools_.perf, analysis_data_file_, root_);
  analysis_text_ += "$ " + command.display + "\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  return analysis_session_.start(std::move(command), AnalysisTool::Perf,
    AnalysisScope::Target, AnalysisStage::PerfReport, analysis_environment_);
}

void IdeWindow::stopAnalysis() {
  if (!analysis_session_.running()) {
    publishEvent(EventSource::Analysis, EventSeverity::Warning,
      "Stop Analysis unavailable: no analyzer is running\n"); return;
  }
  analysis_session_.stop(); menu_state_.reset();
  analysis_text_ += "\nAnalysis stopped by user\n";
  analysis_output_.setText(finalcut::FString(analysis_text_));
  publishEvent(EventSource::Analysis, EventSeverity::Warning, "Analysis stopped\n");
}

void IdeWindow::run() {
  if (root_.empty()) { publishEvent(EventSource::Run, EventSeverity::Warning, "Run unavailable: no project is open\n"); return; }
  if (ctest_session_.running() || analysis_session_.running()) { publishEvent(EventSource::Run, EventSeverity::Warning, "Run unavailable: tests or analysis are running\n"); return; }
  if (build_session_.running()) { publishEvent(EventSource::Run, EventSeverity::Warning, "Run unavailable: a CMake operation is in progress\n"); return; }
  if (!saveAllDocuments()) { publishEvent(EventSource::Run, EventSeverity::Warning, "Run cancelled because not all documents were saved\n"); return; }
  if (project_settings_.launch.pre_launch_build) {
    (void)startPreLaunchBuild(BuildContinuation::Run);
    return;
  }
  startRun();
}

void IdeWindow::startRun() {
  LaunchCommand launch;
  std::string error;
  if (!launchCommand(launch, error)) { publishEvent(EventSource::Run, EventSeverity::Warning, "Run unavailable: " + error + "\n"); return; }
  run_session_.stop(); console_.setControlEnabled(false);
  auto arguments = launch.external_terminal ? launchProcessArguments(launch) : integratedLaunchArguments(launch);
  auto environment = project_settings_.environment;
  for (const auto& [name, value] : launch.environment) environment[name] = value;
  publishEvent(EventSource::Run, EventSeverity::Information, "$ " + launch.executable.string()
    + (launch.arguments.empty() ? std::string{} : " " + formatArgumentList(launch.arguments)) + "\n");
  if (!launch.external_terminal) {
    console_.clear(); console_.setControlEnabled(true); lower_tabs_.setCurrentIndex(3, true); console_.focusInput();
  }
  const auto transport = launch.external_terminal
    ? RunTransport::Process : RunTransport::Terminal;
  if (!run_session_.start(std::move(arguments), transport,
      launch.working_directory, environment, console_.columns(), console_.rows())) {
    console_.setControlEnabled(false);
    publishEvent(EventSource::Run, EventSeverity::Error, "Run failed: cannot start " + launch.executable.string() + "\n");
    showNotification("Program failed to start", NotificationKind::Error);
  } else showNotification("Program started", NotificationKind::Information);
}

void IdeWindow::stopRun() {
  if (!run_session_.running()) { publishEvent(EventSource::Run, EventSeverity::Warning, "Stop Program unavailable: no program is running\n"); return; }
  run_session_.stop();
  console_.setControlEnabled(false);
  publishEvent(EventSource::Run, EventSeverity::Information, "Run terminated by user\n");
  showNotification("Program terminated by user", NotificationKind::Warning); updateStatus();
}

void IdeWindow::debugRun() {
  if (ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Debug unavailable: tests or analysis are running\n");
    return;
  }
  if (gdb_.backend() == DebugBackend::GdbMi && !external_tools_.gdb) {
    publishEvent(EventSource::Debug, EventSeverity::Error,
      "GDB unavailable: install gdb or add it to PATH; debugging cannot start.\n"); return;
  }
  if (gdb_.backend() == DebugBackend::LldbDap && project_settings_.debugger_adapter.empty()
      && !external_tools_.lldb_dap) {
    publishEvent(EventSource::Debug, EventSeverity::Error,
      "LLDB/DAP unavailable: install lldb-dap or select its path in Project Settings.\n"); return;
  }
  if (build_session_.running()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug unavailable: a CMake operation is in progress\n"); return; }
  if (gdb_.running() && gdb_.mode() == DebugSessionMode::Attach) {
    if (!gdb_.active())
      publishEvent(EventSource::Debug, EventSeverity::Warning,
        "Debug continue unavailable: attached process has exited or attach failed\n");
    else if (gdb_.stopped()) gdb_.continueExecution();
    else publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Debug continue unavailable: attached process is already running\n");
    return;
  }
  if (gdb_.running() && gdb_.mode() == DebugSessionMode::Core) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Debug continue unavailable: a core dump is open in read-only mode\n");
    return;
  }
  if (root_.empty()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug unavailable: no project is open\n"); return; }
  if (!gdb_.running()) {
    if (!saveAllDocuments()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug cancelled because not all documents were saved\n"); return; }
    if (project_settings_.launch.pre_launch_build) {
      (void)startPreLaunchBuild(BuildContinuation::Debug);
      return;
    }
    startDebug();
  } else if (!gdb_.active()) {
    if (!saveAllDocuments()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug cancelled because not all documents were saved\n"); return; }
    if (project_settings_.launch.pre_launch_build) {
      gdb_.stop();
      run_session_.stop();
      console_.setControlEnabled(false);
      (void)startPreLaunchBuild(BuildContinuation::Debug);
      return;
    }
    if (gdb_.backend() == DebugBackend::LldbDap) {
      gdb_.stop();
      startDebug();
    } else {
      publishEvent(EventSource::Debug, EventSeverity::Information, "GDB: starting program again\n");
      gdb_.run();
    }
  } else if (gdb_.stopped()) gdb_.continueExecution();
  else publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug continue unavailable: debuggee is already running\n");
}

void IdeWindow::attachToProcess() {
  if (gdb_.backend() != DebugBackend::GdbMi) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Attach to Process currently requires the GDB/MI backend\n");
    return;
  }
  if (!external_tools_.gdb) {
    publishEvent(EventSource::Debug, EventSeverity::Error,
      "Attach unavailable: install gdb or add it to PATH\n");
    return;
  }
  if (gdb_.running() || build_session_.running() || run_session_.running()
      || ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Attach unavailable: another build, run, test, analysis, or debug session is active\n");
    return;
  }
  const auto own_pid = static_cast<int>(::getpid());
  auto processes = debugProcesses();
  processes.erase(std::remove_if(processes.begin(), processes.end(), [own_pid](const auto& process) {
    return process.pid == own_pid;
  }), processes.end());
  std::vector<std::string> labels;
  labels.reserve(processes.size());
  for (const auto& process : processes) {
    auto command = process.command;
    if (command.size() > 88) command = command.substr(0, 85) + "...";
    labels.push_back(std::to_string(process.pid) + "  " + command);
  }
  if (labels.empty()) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Attach unavailable: no readable processes found in /proc\n");
    return;
  }
  const auto selection = choose("Attach to Process", labels);
  if (selection == 0 || selection > processes.size()) return;
  const auto& process = processes[selection - 1];
  const auto working_directory = root_.empty() ? std::filesystem::current_path() : root_;
  if (!gdb_.attach(process.pid, working_directory)) {
    publishEvent(EventSource::Debug, EventSeverity::Error,
      "Failed to start GDB for process " + std::to_string(process.pid) + "\n");
    return;
  }
  run_session_.stop(); console_.setControlEnabled(false); console_.clear();
  sidebar_tabs_.setCurrentIndex(3, true);
  lower_tabs_.setCurrentIndex(0, true);
  publishEvent(EventSource::Debug, EventSeverity::Information,
    "GDB: attaching to process " + std::to_string(process.pid) + " (" + process.command + ")\n");
  showNotification("Attaching to process " + std::to_string(process.pid), NotificationKind::Information);
  updateStatus();
}

void IdeWindow::openCoreDump() {
  if (gdb_.backend() != DebugBackend::GdbMi) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Open core dump currently requires the GDB/MI backend\n");
    return;
  }
  if (!external_tools_.gdb) {
    publishEvent(EventSource::Debug, EventSeverity::Error,
      "Core dump unavailable: install gdb or add it to PATH\n");
    return;
  }
  if (gdb_.running() || build_session_.running() || run_session_.running()
      || ctest_session_.running() || analysis_session_.running()) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      "Core dump unavailable: another build, run, test, analysis, or debug session is active\n");
    return;
  }
  std::error_code directory_error;
  auto initial_directory = !build_dir_.empty() && std::filesystem::is_directory(build_dir_, directory_error)
    ? build_dir_ : root_;
  if (initial_directory.empty()) initial_directory = std::filesystem::current_path(directory_error);
  delTimer(timer_id_);
  CoreDumpDialog dialog(initial_directory, this, user_settings_.language);
  const bool accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) return;
  std::filesystem::path executable;
  std::filesystem::path core_file;
  std::string error;
  if (!dialog.paths(executable, core_file, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  if (!gdb_.openCore(executable, core_file, executable.parent_path())) {
    publishEvent(EventSource::Debug, EventSeverity::Error,
      "Failed to start GDB for core dump " + core_file.string() + "\n");
    return;
  }
  run_session_.stop(); console_.setControlEnabled(false); console_.clear();
  sidebar_tabs_.setCurrentIndex(3, true);
  lower_tabs_.setCurrentIndex(0, true);
  publishEvent(EventSource::Debug, EventSeverity::Information,
    "GDB: opening core dump " + core_file.string() + " with " + executable.string()
      + " (read-only)\n");
  showNotification("Opening core dump in read-only mode", NotificationKind::Information);
  updateStatus();
}

void IdeWindow::startDebug() {
  LaunchCommand launch;
  std::string error;
  if (!launchCommand(launch, error)) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug unavailable: " + error + "\n"); return; }
  auto environment = project_settings_.environment;
  for (const auto& [name, value] : launch.environment) environment[name] = value;
  if (launch.external_terminal)
    publishEvent(EventSource::Debug, EventSeverity::Information,
      "Debug: external terminal is a Run-only setting; using the integrated debug console\n");
  run_session_.stop(); console_.setControlEnabled(false); console_.clear();
  if (!run_session_.openDebugConsole(console_.columns(), console_.rows())) {
    publishEvent(EventSource::Debug, EventSeverity::Error, "Failed to create debuggee PTY\n"); return;
  }
  if (!gdb_.start(launch.executable, launch.working_directory, environment, launch.arguments,
      launch.stdin_file, run_session_.debugTerminal())) {
    run_session_.stop(); console_.setControlEnabled(false);
    publishEvent(EventSource::Debug, EventSeverity::Error,
      "Failed to start " + gdb_.backendName() + "\n"); return;
  }
  run_session_.activateDebugConsole(); console_.setControlEnabled(true); lower_tabs_.setCurrentIndex(3, true); console_.focusInput();
  publishEvent(EventSource::Debug, EventSeverity::Information,
    std::string(gdb_.backend() == DebugBackend::GdbMi ? "GDB: " : "LLDB/DAP: ")
      + launch.executable.string() + "\n");
  gdb_.run();
  showNotification("Debug session started", NotificationKind::Information);
}

void IdeWindow::debugStop() {
  if (!gdb_.running()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug Stop unavailable: debugger is not started\n"); return; }
  const bool attached = gdb_.mode() == DebugSessionMode::Attach;
  const bool core_dump = gdb_.mode() == DebugSessionMode::Core;
  const bool detached = gdb_.stop();
  execution_file_.clear(); execution_line_ = 0; editor_.setExecutionLine(std::nullopt);
  run_session_.stop();
  console_.setControlEnabled(false);
  debug_ui_.invalidateDebug();
  refreshDebugPanel(); updateStatus();
  publishEvent(EventSource::Debug, detached ? EventSeverity::Information : EventSeverity::Warning,
    attached ? (detached ? "Debug session detached; target process left running\n"
                         : "GDB stopped without a confirmed target detach; verify the target process\n")
             : (core_dump ? "Core dump closed\n" : "Debug session stopped\n"));
  showNotification("Debug session stopped", NotificationKind::Warning);
}

void IdeWindow::debugRestart() {
  if (root_.empty()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug Restart unavailable: no project is open\n"); return; }
  if (build_session_.running()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug Restart unavailable: a CMake operation is in progress\n"); return; }
  if (!gdb_.running()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug Restart unavailable: debugger is not started\n"); return; }
  if (gdb_.mode() == DebugSessionMode::Attach || gdb_.mode() == DebugSessionMode::Core) {
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      gdb_.mode() == DebugSessionMode::Core
        ? "Debug Restart unavailable for a read-only core dump\n"
        : "Debug Restart unavailable for an attached process; stop and attach again\n");
    return;
  }
  if (!external_tools_.gdb) { publishEvent(EventSource::Debug, EventSeverity::Error,
    "GDB unavailable: install gdb or add it to PATH; debugging cannot restart.\n"); return; }
  if (!saveAllDocuments()) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug Restart cancelled because not all documents were saved\n"); return; }
  if (project_settings_.launch.pre_launch_build) {
    gdb_.stop(); run_session_.stop(); console_.setControlEnabled(false); debug_ui_.invalidateDebug(); refreshDebugPanel();
    (void)startPreLaunchBuild(BuildContinuation::Debug);
    return;
  }
  LaunchCommand launch;
  std::string error;
  if (!launchCommand(launch, error)) { publishEvent(EventSource::Debug, EventSeverity::Warning, "Debug Restart unavailable: " + error + "\n"); return; }
  gdb_.stop();
  run_session_.stop(); console_.setControlEnabled(false);
  debug_ui_.invalidateDebug(); refreshDebugPanel();
  auto environment = project_settings_.environment;
  for (const auto& [name, value] : launch.environment) environment[name] = value;
  if (!run_session_.openDebugConsole(console_.columns(), console_.rows())) {
    publishEvent(EventSource::Debug, EventSeverity::Error, "Debug Restart failed: cannot create debuggee PTY\n"); updateStatus(); return;
  }
  if (!gdb_.start(launch.executable, launch.working_directory, environment, launch.arguments,
      launch.stdin_file, run_session_.debugTerminal())) {
    run_session_.stop(); console_.setControlEnabled(false);
    publishEvent(EventSource::Debug, EventSeverity::Error, "Debug Restart failed: cannot start GDB\n"); updateStatus(); return;
  }
  run_session_.activateDebugConsole(); console_.setControlEnabled(true); lower_tabs_.setCurrentIndex(3, true); console_.focusInput();
  publishEvent(EventSource::Debug, EventSeverity::Information, "GDB restarted: " + launch.executable.string() + "\n");
  gdb_.run();
  showNotification("Debug session restarted", NotificationKind::Information);
  updateStatus();
}

void IdeWindow::publishEvent(EventSource source, EventSeverity severity, std::string message,
    EventChannel channel) {
  event_log_.publish(channel, source, severity, std::move(message));
  auto& view = channel == EventChannel::Build ? build_output_ : output_;
  view.setText(finalcut::FString(event_log_.text(channel)));
  view.scrollToX(0); view.scrollToEnd();
  lower_tabs_.redrawCurrentPage();
}

void IdeWindow::showNotification(std::string message, NotificationKind kind,
    std::chrono::milliseconds duration) {
  const auto prefix = kind == NotificationKind::Success ? "[OK] "
    : kind == NotificationKind::Warning ? "[!] "
    : kind == NotificationKind::Error ? "[ERR] " : "[i] ";
  notification_.setText(finalcut::FString(prefix + std::move(message)));
  switch (kind) {
    case NotificationKind::Success:
      notification_.setForegroundColor(finalcut::FColor::Black);
      notification_.setBackgroundColor(finalcut::FColor::LightGreen);
      break;
    case NotificationKind::Warning:
      notification_.setForegroundColor(finalcut::FColor::Black);
      notification_.setBackgroundColor(finalcut::FColor::Yellow);
      break;
    case NotificationKind::Error:
      notification_.setForegroundColor(finalcut::FColor::White);
      notification_.setBackgroundColor(finalcut::FColor::Red);
      break;
    case NotificationKind::Information:
      notification_.setForegroundColor(finalcut::FColor::White);
      notification_.setBackgroundColor(finalcut::FColor::Blue);
      break;
  }
  notification_deadline_ = std::chrono::steady_clock::now() + duration;
  notification_.show(); notification_.redraw();
}

void IdeWindow::updateNotification() {
  if (notification_.isShown() && std::chrono::steady_clock::now() >= notification_deadline_) {
    notification_.hide();
    editor_.redraw();
  }
}

void IdeWindow::refreshCompilationDatabase(bool report) {
  compilation_database_warnings_.clear();
  std::string error;
  if (!compilation_database_.load(build_dir_, error)) {
    if (report) publishEvent(EventSource::Lsp, EventSeverity::Error, "Compilation database error: " + error + "\n");
    return;
  }
  if (!report) return;
  if (compilation_database_.available()) {
    publishEvent(EventSource::Lsp, EventSeverity::Information, "Compilation database: " + compilation_database_.path().string() + " ("
      + std::to_string(compilation_database_.size()) + " source file(s))\n");
  } else {
    publishEvent(EventSource::Lsp, EventSeverity::Warning, "Compilation database is missing in " + build_dir_.string()
      + "; run CMake Configure to generate it\n");
  }
}

void IdeWindow::restartLanguageServer() {
  if (root_.empty()) return;
  lsp_.stop();
  lsp_ui_.reset();
  if (!external_tools_.clangd) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning,
      "clangd unavailable: install clangd or add it to PATH; C/C++ completion and code navigation are disabled.\n");
    return;
  }
  if (!lsp_.start(root_, project_settings_.clangd_arguments, project_settings_.environment, build_dir_)) {
    publishEvent(EventSource::Lsp, EventSeverity::Warning, "clangd unavailable: failed to start clangd\n");
    return;
  }
  for (const auto& open_document : documents_)
    if (isCppSource(open_document->path())) lsp_.open(*open_document);
  lsp_.setActiveDocument(document_);
  showNotification("clangd: indexing " + build_dir_.filename().string(), NotificationKind::Information);
}

void IdeWindow::updateMenuState() {
  file_menu_.recent_projects.setEnable(!recent_projects_.empty());
  const CommandContext context{
    .has_project = !root_.empty(),
    .has_document = document_ != nullptr,
    .has_saved_document = document_ && !document_->path().empty(),
    .document_modified = document_ && document_->modified(),
    .source_document = document_ && isCppSource(document_->path()),
    .cmake_document = document_ && isCMakePath(document_->path()),
    .has_selection = editor_.hasSelection(),
    .has_modified_documents = std::any_of(documents_.begin(), documents_.end(), [](const auto& open_document) {
      return open_document->modified();
    }),
    .has_closed_document = !closed_documents_.empty(),
    .can_undo = document_ && document_->canUndo(),
    .can_redo = document_ && document_->canRedo(),
    .lsp_ready = lsp_.ready(),
    .build_running = build_session_.running(),
    .run_running = run_session_.running(),
    .gdb_running = gdb_.running(),
    .gdb_active = gdb_.active(),
    .gdb_stopped = gdb_.stopped(),
    .document_count = documents_.size(),
  };
  const auto state = commandAvailability(context);
  if (menu_state_ && *menu_state_ == state) return;
  menu_state_ = state;
  const auto enabled = [](finalcut::FMenuItem& item, bool value) { item.setEnable(value); };

  enabled(file_menu_.close_project, state.close_project);
  enabled(file_menu_.new_project_file, state.project_file);
  enabled(file_menu_.save, state.save);
  enabled(file_menu_.save_all, state.save_all);
  enabled(file_menu_.save_as, state.save_as);
  enabled(file_menu_.close, state.close_document);
  enabled(file_menu_.close_all, state.close_all);
  enabled(file_menu_.close_others, state.close_others);
  enabled(file_menu_.reopen_closed, state.reopen_closed);
  enabled(edit_menu_.undo, state.undo);
  enabled(edit_menu_.redo, state.redo);
  enabled(edit_menu_.cut, state.cut);
  enabled(edit_menu_.copy, state.copy);
  enabled(edit_menu_.paste, state.paste);
  enabled(edit_menu_.select_all, state.select_all);
  enabled(edit_menu_.toggle_comment, state.select_all);
  enabled(edit_menu_.duplicate_line, state.select_all);
  enabled(edit_menu_.move_line_up, state.select_all);
  enabled(edit_menu_.move_line_down, state.select_all);
  enabled(edit_menu_.delete_line, state.select_all);
  enabled(search_menu_.find, state.find);
  enabled(search_menu_.find_next, state.find);
  enabled(search_menu_.find_previous, state.find);
  enabled(search_menu_.replace, state.find);
  enabled(search_menu_.project_search, state.project_panel);
  enabled(search_menu_.go_to_line, state.go_to_line);
  enabled(search_menu_.definition, state.definition);
  enabled(search_menu_.references, state.references);
  enabled(search_menu_.problems, state.problems);
  enabled(search_menu_.next_diagnostic, state.problems);
  enabled(project_menu_.refresh, state.project_panel);
  enabled(project_menu_.filter, state.project_panel);
  enabled(project_menu_.clear_filter, state.project_panel && !project_filter_.empty());
  enabled(project_menu_.new_directory, state.project_panel);
  enabled(project_menu_.rename_move, state.project_panel);
  enabled(project_menu_.delete_directory, state.project_panel);
  const bool analysis_idle = !analysis_session_.running();
  enabled(run_menu_.configure, state.build && analysis_idle);
  enabled(run_menu_.build, state.build && analysis_idle);
  enabled(run_menu_.rebuild, state.build && analysis_idle);
  enabled(run_menu_.clean, state.build && analysis_idle);
  enabled(run_menu_.cancel_build, state.cancel_build);
  enabled(run_menu_.run, state.run && analysis_idle);
  enabled(run_menu_.stop_run, state.stop_run);
  enabled(run_menu_.launch_select, state.cmake_configuration);
  enabled(run_menu_.launch_settings, state.cmake_configuration);
  enabled(run_menu_.configure_preset, state.cmake_configuration);
  enabled(run_menu_.build_preset, state.cmake_configuration);
  enabled(run_menu_.target, state.cmake_configuration);
  const bool test_idle = state.cmake_configuration && !ctest_session_.running()
    && !analysis_session_.running();
  enabled(run_menu_.discover_tests, test_idle);
  enabled(run_menu_.run_all_tests, test_idle && !ctest_session_.tests().empty());
  enabled(run_menu_.run_selected_test, test_idle && !ctest_session_.tests().empty());
  enabled(run_menu_.rerun_failed_tests, test_idle);
  enabled(run_menu_.test_preset, test_idle);
  enabled(run_menu_.stop_tests, ctest_session_.running());
  const bool core_dump = gdb_.mode() == DebugSessionMode::Core;
  enabled(debug_menu_.start, state.debug_start && analysis_idle && !core_dump);
  enabled(debug_menu_.pause, state.debug_pause);
  enabled(debug_menu_.stop, state.debug_stop);
  enabled(debug_menu_.restart, state.debug_restart && !core_dump);
  enabled(debug_menu_.attach, gdb_.backend() == DebugBackend::GdbMi
    && !gdb_.running() && !build_session_.running()
    && !run_session_.running() && !ctest_session_.running() && analysis_idle);
  enabled(debug_menu_.core_dump, gdb_.backend() == DebugBackend::GdbMi
    && !gdb_.running() && !build_session_.running()
    && !run_session_.running() && !ctest_session_.running() && analysis_idle);
  enabled(debug_menu_.breakpoint, state.breakpoint);
  enabled(debug_menu_.breakpoint_properties, state.debug_panel);
  enabled(debug_menu_.breakpoint_enable, state.debug_panel);
  enabled(debug_menu_.breakpoint_remove, state.debug_panel);
  enabled(debug_menu_.breakpoint_clear, state.debug_panel);
  enabled(debug_menu_.next, state.debug_step && !core_dump);
  enabled(debug_menu_.step, state.debug_step && !core_dump);
  enabled(debug_menu_.finish, state.debug_step && !core_dump);
  enabled(debug_menu_.watch, state.watch || (core_dump && gdb_.stopped()));
  enabled(debug_menu_.evaluate, state.debug_step);
  enabled(debug_menu_.set_variable, state.debug_step && !core_dump);
  enabled(debug_menu_.disassembly, state.debug_step);
  enabled(debug_menu_.memory, state.debug_step);
  enabled(debug_menu_.registers, state.registers || (core_dump && gdb_.stopped()));
  enabled(debug_menu_.signals, state.debug_step && !core_dump
    && gdb_.backend() == DebugBackend::GdbMi);
  enabled(tools_menu_.completion, state.completion);
  enabled(tools_menu_.signature, state.signature_help);
  enabled(tools_menu_.hover, state.hover);
  enabled(tools_menu_.rename, state.rename);
  enabled(tools_menu_.code_actions, state.code_actions);
  enabled(tools_menu_.organize_includes, state.code_actions);
  enabled(tools_menu_.switch_source_header, state.code_actions);
  enabled(tools_menu_.workspace_symbols, state.workspace_symbols);
  enabled(tools_menu_.call_hierarchy, state.hierarchy);
  enabled(tools_menu_.type_hierarchy, state.hierarchy);
  enabled(tools_menu_.format_document, state.format_document);
  enabled(tools_menu_.format_selection, state.format_selection);
  enabled(tools_menu_.toolchain_kits, state.cmake_configuration && analysis_idle);
  enabled(tools_menu_.language_insights, context.source_document && lsp_.ready());
  enabled(tools_menu_.run_analysis, state.cmake_configuration && !analysis_session_.running());
  enabled(tools_menu_.stop_analysis, analysis_session_.running());
  enabled(tools_menu_.project_settings, state.cmake_configuration);
  enabled(window_menu_.previous, state.switch_document);
  enabled(window_menu_.next, state.switch_document);
  enabled(window_menu_.open_files, true);
  enabled(window_menu_.project, true);
  enabled(window_menu_.outline, true);
  enabled(window_menu_.debug, true);
  enabled(window_menu_.breakpoints, true);
  enabled(window_menu_.tests, true);
  enabled(window_menu_.git, true);
  enabled(window_menu_.clear_lower, true);
  enabled(window_menu_.copy_lower, true);
  enabled(window_menu_.filter_problems, true);
  enabled(window_menu_.sidebar_narrower, true);
  enabled(window_menu_.sidebar_wider, true);
  enabled(window_menu_.lower_shorter, true);
  enabled(window_menu_.lower_taller, true);
  enabled(window_menu_.reset_panels, true);
  menu_bar_.redraw();
}

void IdeWindow::updateStatus() {
  updateMenuState();
  std::ostringstream text;
  if (root_.empty()) text << localizedUiText(user_settings_.language, "No project");
  else if (document_) {
    const auto cursor = document_->cursor();
    const auto column = displayColumn(document_->line(cursor.line), cursor.column, project_settings_.tab_width);
    text << (document_->modified() ? "● " : "  ") << document_->path().filename().string()
      << "  " << localizedUiText(user_settings_.language, "Ln") << ' ' << cursor.line + 1
      << ", " << localizedUiText(user_settings_.language, "Col") << ' ' << column + 1
      << "  UTF-8" << (document_->hasUtf8Bom() ? " BOM" : "")
      << ' ' << (document_->lineEnding() == LineEnding::CrLf ? "CRLF" : "LF")
      << (document_->hasFinalNewline() ? "" : " no-final-EOL");
  } else text << localizedUiText(user_settings_.language, "No file");
  text << "  | " << documents_.size() << ' ' << localizedUiText(user_settings_.language, "file(s)")
       << " | clangd: " << (lsp_.running() ? "on" : "off")
       << " | CDB: " << (!compilation_database_.available() ? "missing"
         : (!document_ || !isCppSource(document_->path()) ? "ready"
           : (compilation_database_.contains(document_->path()) ? "entry" : "fallback")))
       << " | debug(" << gdb_.backendName() << "): "
       << (gdb_.mode() == DebugSessionMode::Core && gdb_.running() && gdb_.stopped()
         ? "core/read-only" : (gdb_.exited() ? "exited"
           : (gdb_.running() ? (gdb_.stopped() ? "stopped" : "running") : "off")))
       << " | cmake: ";
  if (!build_session_.running()) text << "idle";
  else {
    text << (build_session_.stage() == BuildStage::Configure ? "configure"
      : build_session_.stage() == BuildStage::Clean ? "clean" : "build");
    if (const auto progress = build_session_.progress()) text << ' ' << *progress << '%';
    text << ' ' << static_cast<unsigned>(build_session_.stageElapsed()) << 's';
  }
  text << " | tests: " << (ctest_session_.running() ? "running" : std::to_string(ctest_session_.tests().size()));
  text << " | analysis: " << (analysis_session_.running()
    ? std::string(analysisToolName(analysis_session_.tool())) : "idle");
  const auto* selected_target = cmake_session_.selectedTarget();
  text
       << " | preset: " << (cmake_session_.configurePreset().empty()
         ? "none" : cmake_session_.configurePreset())
       << "/" << (cmake_session_.buildPreset().empty()
         ? "default" : cmake_session_.buildPreset())
       << " | run: " << project_settings_.active_launch_configuration
       << " | target: " << (!project_settings_.launch.executable.empty()
         ? "launch:" + project_settings_.launch.executable.filename().string()
         : (selected_target ? selected_target->name : "unselected"))
       << " | Ctrl+B Build  F6 Run  F5 Debug  F9 Break  Ctrl+Space Complete";
  status_.setText(finalcut::FString(text.str())); status_.redraw();
}

auto IdeWindow::handleCommand(finalcut::FKey key) -> bool {
  // Depending on terminfo availability, Final Cut reports Alt+F8 as either
  // the extended F56 key or the fallback Meta_f8 key.
  if (key == finalcut::FKey::Meta_f8) key = finalcut::FKey::F56;
  const auto top_menu = [this, key]() -> finalcut::FMenuItem* {
    switch (key) {
      case finalcut::FKey::Meta_f: return file_menu_.menu.getItem();
      case finalcut::FKey::Meta_e: return edit_menu_.menu.getItem();
      case finalcut::FKey::Meta_s: return search_menu_.menu.getItem();
      case finalcut::FKey::Meta_r: return run_menu_.menu.getItem();
      case finalcut::FKey::Meta_p: return project_menu_.menu.getItem();
      case finalcut::FKey::Meta_d: return debug_menu_.menu.getItem();
      case finalcut::FKey::Meta_t: return tools_menu_.menu.getItem();
      case finalcut::FKey::Meta_w: return window_menu_.menu.getItem();
      case finalcut::FKey::Meta_h: return help_menu_.menu.getItem();
      default: return nullptr;
    }
  }();
  if (top_menu != nullptr) {
    finalcut::FAccelEvent accelerator(finalcut::Event::Accelerator, finalcut::FWidget::getFocusWidget());
    finalcut::FApplication::sendEvent(top_menu, &accelerator);
    return true;
  }
  bool custom_match{};
  for (const auto& command : ideCommands()) {
    const auto custom = user_settings_.shortcuts.find(std::string(command.id));
    if (custom == user_settings_.shortcuts.end()) continue;
    const auto configured = shortcutKey(custom->second);
    if (configured && *configured == key) {
      key = command.default_key;
      custom_match = true;
      break;
    }
  }
  if (!custom_match) {
    for (const auto& command : ideCommands()) {
      if (command.default_key == key && user_settings_.shortcuts.contains(std::string(command.id)))
        return true;
    }
  }
  const auto requireLspDocument = [this](std::string_view command) {
    if (!document_) { publishEvent(EventSource::Lsp, EventSeverity::Warning, std::string(command) + " unavailable: no document is open\n"); return false; }
    if (!isCppSource(document_->path())) {
      publishEvent(EventSource::Lsp, EventSeverity::Warning, std::string(command) + " unavailable: the active document is not a C/C++ source file\n");
      return false;
    }
    if (!lsp_.running()) { publishEvent(EventSource::Lsp, EventSeverity::Warning, std::string(command) + " unavailable: clangd is not running\n"); return false; }
    if (!lsp_.ready()) { publishEvent(EventSource::Lsp, EventSeverity::Warning, std::string(command) + " unavailable: clangd is still initializing\n"); return false; }
    return true;
  };
  const auto requireStoppedDebugger = [this](std::string_view command) {
    if (!gdb_.running()) { publishEvent(EventSource::Debug, EventSeverity::Warning, std::string(command) + " unavailable: debugger is not started\n"); return false; }
    if (!gdb_.active()) { publishEvent(EventSource::Debug, EventSeverity::Warning, std::string(command) + " unavailable: the program has exited\n"); return false; }
    if (!gdb_.stopped()) { publishEvent(EventSource::Debug, EventSeverity::Warning, std::string(command) + " unavailable: debuggee is running\n"); return false; }
    return true;
  };
  const auto requireMutableStoppedDebugger = [this, &requireStoppedDebugger](std::string_view command) {
    if (!requireStoppedDebugger(command)) return false;
    if (gdb_.mode() != DebugSessionMode::Core) return true;
    publishEvent(EventSource::Debug, EventSeverity::Warning,
      std::string(command) + " unavailable: core dumps are read-only\n");
    return false;
  };
  switch (key) {
    case finalcut::FKey::Meta_k:
      deferred_command_ = [this] { showCommandPalette(); };
      return true;
    case finalcut::FKey::Ctrl_n: newFile(); return true;
    case finalcut::FKey::Ctrl_o:
      deferred_command_ = [this] {
        delTimer(timer_id_);
        const auto path = finalcut::FFileDialog::fileOpenChooser(this, finalcut::FString(root_.string()), fileDialogFilter);
        timer_id_ = addTimer(100);
        if (!path.isEmpty()) openFile(path.toString());
      };
      return true;
    case finalcut::FKey::Ctrl_s:
      if (!document_) publishEvent(EventSource::Editor, EventSeverity::Warning, "Save unavailable: no document is open\n"); else (void)save();
      return true;
    case finalcut::FKey::Ctrl_w:
      if (!document_) publishEvent(EventSource::Editor, EventSeverity::Warning, "Close unavailable: no document is open\n"); else closeActiveDocument();
      return true;
    case finalcut::FKey::Meta_W: {
      if (documents_.empty()) { publishEvent(EventSource::Editor, EventSeverity::Warning, "Close All unavailable: no document is open\n"); return true; }
      const auto count = documents_.size();
      if (closeAllDocuments()) publishEvent(EventSource::Editor, EventSeverity::Information, "Close All: closed " + std::to_string(count) + " document(s)\n");
      else publishEvent(EventSource::Editor, EventSeverity::Warning, "Close All cancelled\n");
      return true;
    }
    case finalcut::FKey::Meta_u: reopenClosedDocument(); return true;
    case finalcut::FKey::Ctrl_page_up: switchDocument(-1); return true;
    case finalcut::FKey::Ctrl_page_down: switchDocument(1); return true;
    case finalcut::FKey::Meta_page_up:
      sidebar_tabs_.selectRelative(-1, true);
      showNotification("Sidebar: " + sidebar_tabs_.currentTitle());
      publishEvent(EventSource::System, EventSeverity::Information,
        "Sidebar tab: " + sidebar_tabs_.currentTitle() + "\n");
      return true;
    case finalcut::FKey::Meta_page_down:
      sidebar_tabs_.selectRelative(1, true);
      showNotification("Sidebar: " + sidebar_tabs_.currentTitle());
      publishEvent(EventSource::System, EventSeverity::Information,
        "Sidebar tab: " + sidebar_tabs_.currentTitle() + "\n");
      return true;
    case finalcut::FKey::Shift_Meta_page_up:
      lower_tabs_.selectRelative(-1, true);
      showNotification("Lower panel: " + lower_tabs_.currentTitle());
      publishEvent(EventSource::System, EventSeverity::Information,
        "Lower panel tab: " + lower_tabs_.currentTitle() + "\n");
      return true;
    case finalcut::FKey::Shift_Meta_page_down:
      lower_tabs_.selectRelative(1, true);
      showNotification("Lower panel: " + lower_tabs_.currentTitle());
      publishEvent(EventSource::System, EventSeverity::Information,
        "Lower panel tab: " + lower_tabs_.currentTitle() + "\n");
      return true;
    case finalcut::FKey::Ctrl_f:
      if (!document_) publishEvent(EventSource::Editor, EventSeverity::Warning, "Find unavailable: no document is open\n");
      else deferred_command_ = [this] { find(); };
      return true;
    case finalcut::FKey::Ctrl_g:
      if (!document_) publishEvent(EventSource::Editor, EventSeverity::Warning, "Go to line unavailable: no document is open\n");
      else deferred_command_ = [this] { goToLine(); };
      return true;
    case finalcut::FKey::Ctrl_e:
      deferred_command_ = [this] {
        if (!sidebar_tabs_.isTabVisible(1)) {
          sidebar_tabs_.setTabVisible(1, true, true);
          window_menu_.project.setChecked();
        } else sidebar_tabs_.setCurrentIndex(1, true);
      };
      return true;
    case finalcut::FKey::Ctrl_q:
      // Дочерний виджет с фокусом ещё может обрабатывать эту клавишу. Синхронное
      // закрытие главного окна из callback оставляет event loop Final Cut активным;
      // переносим закрытие на следующий timer tick.
      deferred_command_ = [this] { exitIde(); };
      return true;
    case finalcut::FKey::Meta_x:
      deferred_command_ = [this] { exitIde(); };
      return true;
    case finalcut::FKey::Ctrl_d: exitIde(); return true;
    case finalcut::FKey::Ctrl_space:
      if (document_ && isCMakePath(document_->path())) deferred_command_ = [this] { showCMakeCompletion(); };
      else if (requireLspDocument("Completion")) lsp_.requestCompletion(*document_);
      return true;
    case finalcut::FKey::Meta_U:
      if (root_.empty() && gdb_.mode() != DebugSessionMode::Core)
        publishEvent(EventSource::Debug, EventSeverity::Warning, "Add watch unavailable: no project is open\n");
      else deferred_command_ = [this] { addWatch(); };
      return true;
    case finalcut::FKey::Meta_a:
      if (requireLspDocument("Code Actions")) requestCodeActions(false);
      return true;
    case finalcut::FKey::Ctrl_p:
      if (root_.empty()) publishEvent(EventSource::Build, EventSeverity::Warning, "Configure preset unavailable: no project is open\n");
      else if (build_session_.running()) publishEvent(EventSource::Build, EventSeverity::Warning, "Configure preset unavailable: a build is in progress\n");
      else deferred_command_ = [this] { selectCMakePreset(); };
      return true;
    case finalcut::FKey::Meta_b:
      if (root_.empty()) publishEvent(EventSource::Build, EventSeverity::Warning, "Build preset unavailable: no project is open\n");
      else if (build_session_.running()) publishEvent(EventSource::Build, EventSeverity::Warning, "Build preset unavailable: a build is in progress\n");
      else deferred_command_ = [this] { selectCMakeBuildPreset(); };
      return true;
    case finalcut::FKey::Ctrl_t:
      if (root_.empty()) publishEvent(EventSource::Build, EventSeverity::Warning, "Target selection unavailable: no project is open\n");
      else if (build_session_.running()) publishEvent(EventSource::Build, EventSeverity::Warning, "Target selection unavailable: a build is in progress\n");
      else deferred_command_ = [this] { selectCMakeTarget(); };
      return true;
    case finalcut::FKey::Meta_l:
      if (root_.empty()) publishEvent(EventSource::Project, EventSeverity::Warning,
        "Launch configuration unavailable: no project is open\n");
      else if (build_session_.running() || run_session_.running()
               || run_session_.consoleRunning() || gdb_.running())
        publishEvent(EventSource::Project, EventSeverity::Warning,
          "Launch configuration unavailable while build, program, or debugger is running\n");
      else deferred_command_ = [this] { launchSettings(); };
      return true;
    case finalcut::FKey::Meta_L:
      if (root_.empty()) publishEvent(EventSource::Project, EventSeverity::Warning,
        "Launch configuration unavailable: no project is open\n");
      else if (build_session_.running() || run_session_.running()
               || run_session_.consoleRunning() || gdb_.running())
        publishEvent(EventSource::Project, EventSeverity::Warning,
          "Launch configuration unavailable while build, program, or debugger is running\n");
      else deferred_command_ = [this] { selectLaunchProfile(); };
      return true;
    case finalcut::FKey::Ctrl_k:
      if (root_.empty()) publishEvent(EventSource::Build, EventSeverity::Warning, "Problems unavailable: no project is open\n");
      else deferred_command_ = [this] { showProblems(); };
      return true;
    case finalcut::FKey::Ctrl_r:
      if (root_.empty() && gdb_.mode() != DebugSessionMode::Core) {
        publishEvent(EventSource::Debug, EventSeverity::Warning, "Registers unavailable: no project is open\n"); return true;
      }
      gdb_.setRegistersEnabled(!gdb_.registersEnabled());
      debug_state_dirty_ = true; saveDebugState();
      publishEvent(EventSource::Debug, EventSeverity::Information, std::string("Register view ") + (gdb_.registersEnabled() ? "enabled\n" : "disabled\n"));
      debug_ui_.invalidateDebug(); refreshDebugPanel();
      return true;
    case finalcut::FKey::Del_char:
      if (breakpoints_.hasFocus()) { removeSelectedBreakpoint(); return true; }
      if (debug_.hasFocus()) { removeSelectedWatch(); return true; }
      return false;
    case finalcut::FKey::F1:
      if (requireLspDocument("Symbol information")) lsp_.requestHover(*document_);
      return true;
    case finalcut::FKey::F2:
      if (requireLspDocument("Rename")) {
        deferred_command_ = [this] {
          const auto name = prompt("Rename symbol", "New name:");
          if (!name.empty() && document_ && isCppSource(document_->path())) lsp_.requestRename(*document_, name);
        };
      }
      return true;
    case finalcut::FKey::F3:
      if (requireLspDocument("Go to definition")) lsp_.requestDefinition(*document_);
      return true;
    case finalcut::FKey::F4:
      if (requireLspDocument("Find references")) lsp_.requestReferences(*document_);
      return true;
    case finalcut::FKey::F5: debugRun(); return true;
    case finalcut::FKey::F6: run(); return true;
    case finalcut::FKey::Ctrl_b: build(); return true;
    case finalcut::FKey::F32: {
      const auto& diagnostics = lsp_.diagnostics();
      const auto build_count = build_session_.diagnostics().size();
      const auto analysis_count = analysis_session_.diagnostics().size();
      const auto count = build_count + analysis_count + diagnostics.size();
      if (count == 0) { publishEvent(EventSource::Build, EventSeverity::Warning, "No build, analysis, or clangd diagnostics\n"); return true; }
      diagnostic_index_ %= count;
      if (diagnostic_index_ < build_count) {
        const auto& diagnostic = build_session_.diagnostics()[diagnostic_index_];
        openFile(diagnostic.path);
        if (document_ && document_->path() == diagnostic.path) editor_.reveal({diagnostic.line, diagnostic.column});
        publishEvent(EventSource::Build, EventSeverity::Information, "Build diagnostic: " + diagnostic.message + "\n");
      } else if (diagnostic_index_ < build_count + analysis_count) {
        const auto& diagnostic = analysis_session_.diagnostics()[diagnostic_index_ - build_count];
        openFile(diagnostic.path);
        if (document_ && document_->path() == diagnostic.path)
          editor_.reveal({diagnostic.line, diagnostic.column});
        publishEvent(EventSource::Analysis, EventSeverity::Information,
          "Analysis diagnostic: " + diagnostic.message + "\n");
      } else {
        const auto& diagnostic = diagnostics[diagnostic_index_ - build_count - analysis_count];
        if (!document_ || diagnostic.path != document_->path()) openFile(diagnostic.path);
        if (document_ && document_->path() == diagnostic.path) {
          auto position = diagnostic.position;
          position.column = document_->byteColumn(position.line, position.column);
          editor_.reveal(position);
        }
        publishEvent(EventSource::Lsp, EventSeverity::Information, "Diagnostic: " + diagnostic.message + "\n");
      }
      ++diagnostic_index_;
      return true;
    }
    case finalcut::FKey::F9:
      if (!document_) publishEvent(EventSource::Debug, EventSeverity::Warning, "Breakpoint unavailable: no document is open\n");
      else if (document_->path().empty()) publishEvent(EventSource::Debug, EventSeverity::Warning, "Breakpoint unavailable: save the document first\n");
      else if (!isCppSource(document_->path())) publishEvent(EventSource::Debug, EventSeverity::Warning, "Breakpoint unavailable: the active document is not C/C++ source\n");
      else {
        const bool enabled = gdb_.toggleBreakpoint(document_->path(), document_->cursor().line + 1);
        debug_state_dirty_ = true; saveDebugState();
        publishEvent(EventSource::Debug, EventSeverity::Success, std::string(enabled ? "Breakpoint set: " : "Breakpoint removed: ")
          + document_->path().string() + ":" + std::to_string(document_->cursor().line + 1) + "\n");
        debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
      }
      return true;
    case finalcut::FKey::F17:
      if (!gdb_.running()) publishEvent(EventSource::Debug, EventSeverity::Warning, "Pause unavailable: debugger is not started\n");
      else if (!gdb_.active()) publishEvent(EventSource::Debug, EventSeverity::Warning, "Pause unavailable: the program has exited\n");
      else if (gdb_.stopped()) publishEvent(EventSource::Debug, EventSeverity::Warning, "Pause unavailable: debuggee is already stopped\n");
      else gdb_.interrupt();
      return true;
    case finalcut::FKey::F8: if (requireMutableStoppedDebugger("Next")) gdb_.next(); return true;
    case finalcut::FKey::F7: if (requireMutableStoppedDebugger("Step into")) gdb_.step(); return true;
    case finalcut::FKey::F56: if (requireMutableStoppedDebugger("Step out")) gdb_.finish(); return true;
    default: return false;
  }
}

void IdeWindow::onTimer(finalcut::FTimerEvent* event) {
  if (event->getTimerId() != timer_id_) return;
  if (active_context_menu_ && !active_context_menu_->isShown()) {
    if (context_focus_) {
      context_focus_->setFocus();
      finalcut::FWidget::setFocusWidget(context_focus_);
      context_focus_->redraw();
    }
    active_context_menu_ = nullptr; context_focus_ = nullptr;
  }
  if (deferred_command_) {
    auto command = std::move(deferred_command_);
    deferred_command_ = {};
    command();
    return;
  }
  ++maintenance_ticks_;
  if (auto git_update = git_session_.poll()) {
    if (git_update->exit_code != 0) {
      if (git_update->operation == GitOperation::Status) {
        git_files_state_.clear(); git_panel_message_ = "Git unavailable or not a repository";
        refreshGitPanel();
      } else {
        publishEvent(EventSource::Project, EventSeverity::Error,
          "Git command failed: " + git_update->output + "\n");
        showNotification("Git command failed", NotificationKind::Error);
      }
    } else if (git_update->operation == GitOperation::Status) {
      git_files_state_ = std::move(git_update->files); git_panel_message_.clear(); refreshGitPanel();
    } else if (git_update->operation == GitOperation::Stage
        || git_update->operation == GitOperation::Unstage) {
      (void)git_session_.startStatus();
    } else if (git_update->operation == GitOperation::Diff
        || git_update->operation == GitOperation::History) {
      const auto title = git_update->operation == GitOperation::Diff ? "Git diff" : "Git history";
      showTextDialog(title, git_update->output.empty() ? "No changes or history" : std::move(git_update->output));
    }
  }
  if (maintenance_ticks_ % 50 == 0 && sidebar_tabs_.currentIndex() == 6
      && !root_.empty() && !git_session_.running())
    (void)git_session_.startStatus();
  if (maintenance_ticks_ % 20 == 0) checkExternalChanges();
  if (maintenance_ticks_ % 50 == 0) autosaveRecovery();
  lsp_.poll();
  const auto lsp_changes = lsp_ui_.observe(lsp_.ready(), lsp_.diagnosticsRevision(),
    lsp_.semanticTokensRevision());
  if (lsp_changes.became_ready)
    showNotification("clangd indexing is ready", NotificationKind::Success);
  const auto active_lsp_document = document_ ? std::optional<LspDocumentIdentity>{
    {document_->path(), document_->version()}} : std::nullopt;
  auto lsp_events = LspUiController::route(lsp_ui_.collect(lsp_), active_lsp_document);
  refreshOutline(std::move(lsp_events.document_symbols));
  if (lsp_changes.diagnostics_changed) {
    editor_.setDiagnostics(&lsp_.diagnostics());
  }
  if (lsp_changes.semantic_tokens_changed) {
    editor_.setSemanticTokens(&lsp_.semanticTokens());
  }
  if (lsp_events.discarded_completions != 0)
    publishEvent(EventSource::Lsp, EventSeverity::Warning,
      "Completion discarded: the source document changed while clangd was responding\n");
  if (lsp_events.discarded_code_actions != 0)
    publishEvent(EventSource::Lsp, EventSeverity::Warning,
      "Code Action discarded: the source document changed while clangd was responding\n");
  auto completions = std::move(lsp_events.completions);
  if (!completions.empty()) {
    std::vector<LspCompletionItem> unique;
    std::vector<std::string> labels;
    std::unordered_set<std::string> seen;
    for (auto& completion : completions) {
      const auto key = completion.label + '\n' + completion.insertion;
      if (completion.label.empty() || !seen.insert(key).second) continue;
      labels.push_back("[" + std::string(completionKindName(completion.kind)) + "] " + completion.label
        + (completion.detail.empty() ? std::string{} : " — " + completion.detail));
      unique.push_back(std::move(completion));
    }
    if (labels.empty()) publishEvent(EventSource::Lsp, EventSeverity::Warning, "Completion: clangd returned no usable suggestions\n");
    else {
      const auto selection = choose("clangd completion", labels);
      if (selection > 0 && selection <= unique.size() && document_) {
        const auto& completion = unique[selection - 1];
        lsp_.resolveCompletion(completion);
        if (completion.edit && completion.edit->start.line < document_->lines().size()
            && completion.edit->end.line < document_->lines().size()) {
          auto start = completion.edit->start; auto end = completion.edit->end;
          start.column = document_->byteColumn(start.line, start.column);
          end.column = document_->byteColumn(end.line, end.column);
          document_->replaceRange(start, end, completion.edit->text);
        } else document_->replaceIdentifierBeforeCursor(completion.insertion);
        lsp_.change(*document_); editor_.invalidateSyntax(); updateStatus();
        if (!completion.documentation.empty())
          publishEvent(EventSource::Lsp, EventSeverity::Information, "Completion documentation:\n" + completion.documentation + "\n");
      }
    }
  }
  if (lsp_events.resolved_completion && !lsp_events.resolved_completion->documentation.empty())
    publishEvent(EventSource::Lsp, EventSeverity::Information,
      "Completion documentation: " + lsp_events.resolved_completion->documentation + "\n");
  auto signatures = std::move(lsp_events.signatures);
  if (!signatures.empty()) {
    std::ostringstream text;
    for (const auto& signature : signatures) {
      text << (signature.active ? "> " : "  ") << signature.label << '\n';
      if (!signature.documentation.empty()) text << signature.documentation << '\n';
      for (std::size_t index = 0; index < signature.parameters.size(); ++index)
        text << (signature.active && index == signature.active_parameter ? "  * " : "    ")
             << signature.parameters[index] << '\n';
      text << '\n';
    }
    showTextDialog("Signature help", text.str());
  }
  auto hover = std::move(lsp_events.hover);
  if (!hover.empty()) showTextDialog("Symbol information", std::move(hover));
  auto definitions = std::move(lsp_events.definitions);
  if (!definitions.empty()) {
    const auto& location = definitions.front();
    openFile(location.path);
    if (document_ && document_->path() == location.path) {
      auto position = location.position;
      position.column = document_->byteColumn(position.line, position.column);
      editor_.reveal(position);
    }
  }
  auto references = std::move(lsp_events.references);
  if (!references.empty()) {
    std::vector<std::string> labels;
    labels.reserve(references.size());
    for (const auto& location : references) {
      std::error_code error;
      auto path = std::filesystem::relative(location.path, root_, error);
      if (error) path = location.path;
      labels.push_back(path.string() + ":" + std::to_string(location.position.line + 1) + ":" + std::to_string(location.position.column + 1));
    }
    const auto selection = choose("References", labels);
    if (selection > 0 && selection <= references.size()) {
      const auto location = references[selection - 1];
      openFile(location.path);
      if (document_ && document_->path() == location.path) {
        auto position = location.position;
        position.column = document_->byteColumn(position.line, position.column);
        editor_.reveal(position);
      }
    }
  }
  if (lsp_events.rename_edit) applyWorkspaceEdit(std::move(*lsp_events.rename_edit), "Rename");
  for (auto& request : lsp_events.workspace_apply_requests) {
    std::string failure_reason;
    const bool applied = applyWorkspaceEdit(std::move(request.edit), request.label, &failure_reason);
    lsp_.respondWorkspaceApplyEdit(request.id, applied, std::move(failure_reason));
  }
  auto code_actions = std::move(lsp_events.code_actions);
  if (!code_actions.empty()) {
    std::size_t selection = 1;
    if (code_actions.size() > 1 || !code_actions.front().automatic) {
      std::vector<std::string> labels;
      labels.reserve(code_actions.size());
      for (const auto& action : code_actions)
        labels.push_back(action.title + (action.kind.empty() ? std::string{} : "  [" + action.kind + "]"));
      selection = choose(code_actions.front().automatic ? "Organize Includes" : "Code Actions", labels);
    }
    if (selection > 0 && selection <= code_actions.size()) {
      auto& action = code_actions[selection - 1];
      publishEvent(EventSource::Lsp, EventSeverity::Information, "Code Action: " + action.title + "\n");
      applyWorkspaceEdit(std::move(action.edit), action.title);
    }
  }
  if (lsp_events.switched_source_header) openFile(*lsp_events.switched_source_header);
  auto workspace_symbols = std::move(lsp_events.workspace_symbols);
  if (!workspace_symbols.empty()) {
    std::vector<std::string> labels;
    labels.reserve(workspace_symbols.size());
    for (const auto& symbol : workspace_symbols) {
      std::error_code error;
      auto path = std::filesystem::relative(symbol.path, root_, error);
      if (error) path = symbol.path;
      labels.push_back(symbol.name + (symbol.detail.empty() ? std::string{} : " — " + symbol.detail)
        + "  " + path.string() + ":" + std::to_string(symbol.position.line + 1));
    }
    const auto selection = choose("Workspace Symbols", labels);
    if (selection > 0 && selection <= workspace_symbols.size()) navigateTo(workspace_symbols[selection - 1]);
  }
  const auto showHierarchy = [this](std::string title, std::optional<LspHierarchy> hierarchy) {
    if (!hierarchy || hierarchy->items.empty()) return;
    std::vector<std::string> labels;
    labels.reserve(hierarchy->items.size());
    for (const auto& item : hierarchy->items) {
      std::error_code error;
      auto path = std::filesystem::relative(item.path, root_, error);
      if (error) path = item.path;
      labels.push_back("[" + item.relation + "] " + item.name
        + (item.detail.empty() ? std::string{} : " — " + item.detail)
        + "  " + path.string() + ":" + std::to_string(item.position.line + 1));
    }
    const auto selection = choose(std::move(title) + " — " + hierarchy->root, labels);
    if (selection > 0 && selection <= hierarchy->items.size()) navigateTo(hierarchy->items[selection - 1]);
  };
  showHierarchy("Call Hierarchy", std::move(lsp_events.call_hierarchy));
  showHierarchy("Type Hierarchy", std::move(lsp_events.type_hierarchy));
  const auto showInsight = [this](std::string title, std::vector<std::string> rows) {
    std::string text = std::move(title) + ": " + std::to_string(rows.size()) + " item(s)\n";
    for (const auto& row : rows) text += "  " + row + "\n";
    publishEvent(EventSource::Lsp, rows.empty() ? EventSeverity::Information : EventSeverity::Success,
      std::move(text));
  };
  if (!lsp_events.inlay_hints.empty() || !lsp_events.document_highlights.empty()
      || !lsp_events.folding_ranges.empty() || !lsp_events.selection_ranges.empty()
      || !lsp_events.code_lens.empty() || !lsp_events.include_relations.empty()) {
    std::vector<std::string> rows;
    rows.reserve(lsp_events.inlay_hints.size() + lsp_events.document_highlights.size()
      + lsp_events.folding_ranges.size() + lsp_events.selection_ranges.size()
      + lsp_events.code_lens.size() + lsp_events.include_relations.size());
    for (const auto& hint : lsp_events.inlay_hints)
      rows.push_back("inlay " + std::to_string(hint.position.line + 1) + ":"
        + std::to_string(hint.position.column + 1) + " " + hint.label);
    for (const auto& highlight : lsp_events.document_highlights)
      rows.push_back("highlight " + std::to_string(highlight.start.line + 1) + ":"
        + std::to_string(highlight.start.column + 1) + "-"
        + std::to_string(highlight.end.line + 1) + ":" + std::to_string(highlight.end.column + 1));
    for (const auto& range : lsp_events.folding_ranges)
      rows.push_back("fold " + std::to_string(range.start_line + 1) + "-"
        + std::to_string(range.end_line + 1) + (range.kind.empty() ? std::string{} : " [" + range.kind + "]"));
    for (const auto& range : lsp_events.selection_ranges)
      rows.push_back("selection " + std::to_string(range.start.line + 1) + ":"
        + std::to_string(range.start.column + 1) + "-" + std::to_string(range.end.line + 1)
        + ":" + std::to_string(range.end.column + 1));
    for (const auto& lens : lsp_events.code_lens)
      rows.push_back("lens " + std::to_string(lens.start.line + 1) + ": " + lens.title
        + (lens.command.empty() ? std::string{} : " (" + lens.command + ")"));
    for (const auto& relation : lsp_events.include_relations)
      rows.push_back(relation.relation + " include " + relation.path.string() + ":"
        + std::to_string(relation.line + 1));
    showInsight("clangd language insights", std::move(rows));
  }
  for (const auto& feedback : lsp_events.feedback) {
    publishEvent(EventSource::Lsp, feedback.error ? EventSeverity::Error : EventSeverity::Information,
      std::string(lspOperationLabel(feedback.operation))
        + (feedback.error ? " error: " : ": ") + feedback.message + "\n");
    if (feedback.operation == LspOperation::Server && feedback.error)
      showNotification(feedback.message, NotificationKind::Error, std::chrono::milliseconds{6000});
  }

  const auto build_poll = build_session_.poll(root_);
  for (const auto& chunk : build_poll.output) publishEvent(EventSource::Build, EventSeverity::Information, chunk, EventChannel::Build);
  if (build_poll.diagnostics_added != 0) problems_signature_.clear();
  if (build_poll.completion) {
    const auto code = build_poll.completion->exit_code;
    const auto finished_stage = build_poll.completion->stage;
    const auto stage_name = finished_stage == BuildStage::Configure ? "Configure"
      : finished_stage == BuildStage::Clean ? "Clean" : "Build";
    const auto stage_elapsed = build_poll.completion->elapsed;
    if (code != 0) {
      finishBuildOperation(code, stage_name);
    } else {
      if (finished_stage == BuildStage::Configure) {
        refreshCompilationDatabase(true);
        refreshCMakeTargets();
      }
      const auto next_stage = build_session_.nextStageAfterSuccess();
      if (next_stage == BuildStage::Build) {
        std::ostringstream stage_summary;
        stage_summary.setf(std::ios::fixed);
        stage_summary.precision(1);
        stage_summary << stage_name << " finished with exit code 0 after " << stage_elapsed << " s\n";
        publishEvent(EventSource::Build, EventSeverity::Information, stage_summary.str(), EventChannel::Build);
        publishEvent(EventSource::Build, EventSeverity::Success, stage_summary.str());
        (void)startBuildStage(false);
      } else {
        finishBuildOperation(0, {});
      }
    }
  }
  const auto analysis_stage = analysis_session_.stage();
  const auto analysis_tool = analysis_session_.tool();
  auto analysis_poll = analysis_session_.poll(root_);
  for (const auto& chunk : analysis_poll.output) analysis_text_ += chunk;
  if (analysis_text_.size() > 300000) analysis_text_.erase(0, analysis_text_.size() - 240000);
  if (!analysis_poll.output.empty()) {
    analysis_output_.setText(finalcut::FString(analysis_text_));
    lower_tabs_.redrawCurrentPage();
  }
  if (analysis_poll.diagnostics_added != 0) {
    problems_signature_.clear(); refreshProblemsPanel();
  }
  if (analysis_poll.completion) {
    const auto code = *analysis_poll.completion;
    if (analysis_stage == AnalysisStage::SanitizerConfigure && code == 0) {
      if (!startSanitizerBuild()) {
        analysis_text_ += "Failed to start sanitizer build\n";
        analysis_output_.setText(finalcut::FString(analysis_text_));
        publishEvent(EventSource::Analysis, EventSeverity::Error,
          "Failed to start sanitizer build\n");
      }
    } else if (analysis_stage == AnalysisStage::SanitizerBuild && code == 0
        && !sanitizer_target_.empty()) {
      if (!startSanitizerRun()) {
        analysis_text_ += "Failed to start sanitizer executable\n";
        analysis_output_.setText(finalcut::FString(analysis_text_));
      }
    } else if (analysis_stage == AnalysisStage::CoverageConfigure && code == 0) {
      if (!startCoverageBuild()) {
        analysis_text_ += "Failed to start coverage build\n";
        analysis_output_.setText(finalcut::FString(analysis_text_));
        publishEvent(EventSource::Analysis, EventSeverity::Error,
          "Failed to start coverage build\n");
      }
    } else if (analysis_stage == AnalysisStage::CoverageBuild && code == 0) {
      if (!startCoverageRun()) {
        analysis_text_ += "Failed to start coverage workload\n";
        analysis_output_.setText(finalcut::FString(analysis_text_));
      }
    } else if (analysis_stage == AnalysisStage::CoverageRun) {
      if (!startCoverageReport()) {
        analysis_text_ += "Failed to start gcovr report\n";
        analysis_output_.setText(finalcut::FString(analysis_text_));
      }
    } else if (analysis_stage == AnalysisStage::CoverageReport) {
      std::string summary;
      std::string error;
      auto diagnostics = loadCoverageDiagnostics(analysis_data_file_, root_, summary, error);
      if (error.empty()) {
        analysis_session_.addDiagnostics(std::move(diagnostics));
        analysis_text_ += "\n" + summary + "\n";
        problems_signature_.clear(); refreshProblemsPanel();
      } else analysis_text_ += "\nCoverage report error: " + error + "\n";
      analysis_output_.setText(finalcut::FString(analysis_text_));
      publishEvent(EventSource::Analysis, error.empty() ? EventSeverity::Success : EventSeverity::Error,
        error.empty() ? summary + "\n" : "Coverage report error: " + error + "\n");
      showNotification(error.empty() ? "Coverage report completed" : "Coverage report failed",
        error.empty() ? NotificationKind::Success : NotificationKind::Error);
    } else if (analysis_stage == AnalysisStage::PerfRecord && code == 0) {
      if (!startPerfReport()) {
        analysis_text_ += "Failed to start perf report\n";
        analysis_output_.setText(finalcut::FString(analysis_text_));
      }
    } else {
      const auto label = std::string(analysisToolName(analysis_tool));
      analysis_text_ += "\n" + label + " finished with exit code " + std::to_string(code) + "\n";
      analysis_output_.setText(finalcut::FString(analysis_text_));
      publishEvent(EventSource::Analysis,
        code == 0 ? EventSeverity::Success : EventSeverity::Error,
        label + " finished with exit code " + std::to_string(code) + "\n");
      showNotification(label + (code == 0 ? " completed" : " failed"),
        code == 0 ? NotificationKind::Success : NotificationKind::Error);
    }
    menu_state_.reset();
  }
  const auto ctest_operation = ctest_session_.operation();
  auto ctest_poll = ctest_session_.poll();
  if (ctest_operation != CTestOperation::Discover) {
    for (auto& chunk : ctest_poll.output)
      publishEvent(EventSource::Test, EventSeverity::Information, std::move(chunk));
  }
  if (ctest_poll.tests_changed) refreshTestsPanel();
  if (ctest_poll.completion) {
    menu_state_.reset();
    if (!ctest_poll.error.empty()) {
      publishEvent(EventSource::Test, EventSeverity::Error,
        "CTest discovery failed: " + ctest_poll.error + "\n");
      showNotification("CTest discovery failed", NotificationKind::Error);
    } else if (ctest_operation == CTestOperation::Discover) {
      const auto count = ctest_session_.tests().size();
      publishEvent(EventSource::Test,
        *ctest_poll.completion == 0 ? EventSeverity::Success : EventSeverity::Error,
        "CTest discovery finished with exit code " + std::to_string(*ctest_poll.completion)
          + ": " + std::to_string(count) + " test(s)\n");
      showNotification("CTest: discovered " + std::to_string(count) + " test(s)",
        *ctest_poll.completion == 0 ? NotificationKind::Success : NotificationKind::Error);
    } else {
      const auto code = *ctest_poll.completion;
      publishEvent(EventSource::Test, code == 0 ? EventSeverity::Success : EventSeverity::Error,
        "CTest finished with exit code " + std::to_string(code) + "\n");
      showNotification(code == 0 ? "CTest completed successfully" : "CTest failed",
        code == 0 ? NotificationKind::Success : NotificationKind::Error);
    }
  }
  auto run_poll = run_session_.poll();
  for (auto& chunk : run_poll.output)
    publishEvent(EventSource::Run, EventSeverity::Information, std::move(chunk));
  for (auto& chunk : run_poll.console_output) {
    console_.append(chunk);
    lower_tabs_.redrawCurrentPage();
  }
  if (run_poll.completion) {
    const auto code = *run_poll.completion;
    console_.setControlEnabled(false);
    lower_tabs_.setCurrentIndex(0);
    publishEvent(EventSource::Run, code == 0 ? EventSeverity::Success : EventSeverity::Error,
      "Run finished with exit code " + std::to_string(code) + "\n");
    showNotification("Program finished (exit " + std::to_string(code) + ")",
      code == 0 ? NotificationKind::Success : NotificationKind::Error);
    editor_.setFocus(); finalcut::FWidget::setFocusWidget(&editor_);
  }
  gdb_.poll();
  refreshExecutionLocation();
  const bool debug_active = gdb_.active();
  if (debug_ui_.observeActive(debug_active, gdb_.exited())) {
    if (gdb_.mode() == DebugSessionMode::Core)
      showNotification("Core dump could not be loaded", NotificationKind::Error);
    else showNotification("Debuggee finished", NotificationKind::Success);
  }
  for (auto& line : gdb_.takeOutput())
    publishEvent(EventSource::Debug, EventSeverity::Information, std::move(line) + "\n");
  for (auto& result : gdb_.takeResults()) {
    const auto operation = result.kind == DebugResultKind::Evaluation ? "Evaluation"
      : result.kind == DebugResultKind::Assignment ? "Assignment"
      : result.kind == DebugResultKind::Disassembly ? "Disassembly" : "Memory";
    if (!result.error.empty()) {
      publishEvent(EventSource::Debug, EventSeverity::Error,
        std::string(operation) + " error for " + result.expression + ": " + result.error + "\n");
      showTextDialog(std::string(operation) + " error", result.expression + "\n\n" + result.error);
    } else {
      publishEvent(EventSource::Debug, EventSeverity::Success,
        std::string(operation) + ": " + result.expression + " = " + result.value + "\n");
      showTextDialog(operation, result.expression + " = " + result.value);
    }
  }
  refreshProblemsPanel();
  refreshDebugPanel();
  refreshBreakpointsPanel();
  updateNotification();
  updateStatus();
}

void IdeWindow::onKeyPress(finalcut::FKeyEvent* event) {
  if (handleCommand(event->key())) event->accept();
  else FDialog::onKeyPress(event);
}

void IdeWindow::onClose(finalcut::FCloseEvent* event) {
  const bool has_changes = std::any_of(documents_.begin(), documents_.end(), [](const auto& document) { return document->modified(); });
  if (has_changes) {
    const auto answer = finalcut::FMessageBox::info(this, "Unsaved changes", "Save all changed documents before closing?",
      finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No, finalcut::FMessageBox::ButtonType::Cancel);
    if (answer == finalcut::FMessageBox::ButtonType::Cancel) { event->ignore(); return; }
    if (answer == finalcut::FMessageBox::ButtonType::Yes) {
      if (!saveAllDocuments()) { event->ignore(); return; }
    }
  }
  clearRecovery(recovery_file_);
  event->accept();
}

auto IdeWindow::prompt(std::string title, std::string label) -> std::string {
  delTimer(timer_id_);
  PromptDialog dialog(title, label, this, user_settings_.language);
  const auto result = dialog.exec() == finalcut::FDialog::ResultCode::Accept ? dialog.value() : std::string{};
  timer_id_ = addTimer(100);
  return result;
}

auto IdeWindow::choose(std::string title, const std::vector<std::string>& items) -> std::size_t {
  if (items.empty()) return 0;
  std::vector<std::string> labels;
  labels.reserve(items.size());
  for (const auto& item : items)
    labels.push_back(localizedUiText(user_settings_.language, item));
  delTimer(timer_id_);
  SelectionDialog dialog(title, labels, this, user_settings_.language);
  const auto result = dialog.exec() == finalcut::FDialog::ResultCode::Accept ? dialog.selected() : 0;
  timer_id_ = addTimer(100);
  return result;
}

auto IdeWindow::applyWorkspaceEdit(WorkspaceEdit workspace, std::string title,
    std::string* failure_reason) -> bool {
  struct PreparedFileEdit {
    std::filesystem::path path;
    Document* target{};
    std::unique_ptr<Document> temporary;
    std::vector<TextReplacement> replacements;
    std::vector<std::string> originals;
    std::string original_text;
  };

  const auto displayText = [](std::string value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
      if (character == '\n') result += "\\n";
      else if (character == '\r') result += "\\r";
      else if (character == '\t') result += "\\t";
      else result += character;
      if (result.size() >= 72) { result.resize(69); result += "..."; break; }
    }
    return result;
  };
  const auto fail = [this, failure_reason](std::string message) {
    if (failure_reason) *failure_reason = message;
    publishEvent(EventSource::Lsp, EventSeverity::Error, "Workspace edit error: " + message + "; no pending text changes were applied\n");
    return false;
  };
  WorkspaceFileTransaction file_transaction;
  std::string transaction_error;
  if (!file_transaction.prepare(root_, std::move(workspace.file_operations), transaction_error))
    return fail(transaction_error);

  std::vector<PreparedFileEdit> prepared;
  prepared.reserve(workspace.files.size());
  std::unordered_set<std::filesystem::path> prepared_paths;
  std::ostringstream preview;
  std::size_t changed_ranges{};
  std::size_t closed_files{};
  for (const auto& operation : file_transaction.operations()) {
    const auto relative = [this](const std::filesystem::path& path) {
      std::error_code error;
      const auto result = std::filesystem::relative(path, root_, error);
      return error ? path.string() : result.string();
    };
    if (operation.kind == WorkspaceFileOperationKind::Create)
      preview << "[create] " << relative(operation.path) << '\n';
    else if (operation.kind == WorkspaceFileOperationKind::Rename)
      preview << "[rename] " << relative(operation.path) << " -> " << relative(operation.new_path) << '\n';
    else
      preview << "[delete] " << relative(operation.path)
              << (operation.recursive ? " (recursive)" : "") << '\n';
    for (const auto& open_document : documents_) {
      if (operation.kind == WorkspaceFileOperationKind::Delete
          && open_document->path() == operation.path)
        return fail("Close the file before deleting it: " + operation.path.string());
      if (operation.kind == WorkspaceFileOperationKind::Rename
          && open_document->path() == operation.new_path)
        return fail("Rename destination is already open: " + operation.new_path.string());
    }
  }
  for (auto& file_edit : workspace.files) {
    PreparedFileEdit file;
    file.path = normalizePath(file_edit.path);
    if (!prepared_paths.insert(file.path).second) {
      return fail("Language server returned duplicate edits for " + file.path.string());
    }
    auto source_path = file.path;
    bool created_file{};
    for (const auto& operation : file_transaction.operations()) {
      if (operation.kind == WorkspaceFileOperationKind::Rename && operation.new_path == file.path)
        source_path = operation.path;
      if (operation.kind == WorkspaceFileOperationKind::Create && operation.path == file.path)
        created_file = true;
    }
    for (auto& open_document : documents_) {
      if (open_document->path() == source_path) { file.target = open_document.get(); break; }
    }
    if (file_edit.version && (!file.target || file.target->version() != *file_edit.version))
      return fail("Versioned edit is stale or targets a closed document: " + file.path.string());
    std::string error;
    if (!file.target) {
      file.temporary = std::make_unique<Document>();
      if (created_file && !std::filesystem::exists(source_path)) {
        file.temporary->relocate(file.path); file.temporary->setText({});
      } else if (!file.temporary->load(source_path, error)) {
        return fail(error);
      }
      if (source_path != file.path) file.temporary->relocate(file.path);
      file.target = file.temporary.get();
      file.original_text = file.target->text();
      ++closed_files;
    }

    std::vector<PreparedReplacement> replacements;
    if (!prepareWorkspaceReplacements(*file.target, file_edit.edits, replacements, error)) {
      return fail(error + " in " + file.path.string());
    }
    file.replacements.reserve(replacements.size());
    file.originals.reserve(replacements.size());
    for (auto& replacement : replacements) {
      file.replacements.push_back(std::move(replacement.replacement));
      file.originals.push_back(std::move(replacement.original));
    }
    if (file.replacements.empty()) continue;

    std::error_code relative_error;
    auto display_path = root_.empty() ? file.path : std::filesystem::relative(file.path, root_, relative_error);
    if (relative_error || display_path.empty()) display_path = file.path;
    preview << (file.temporary ? "[disk] " : "[open] ") << display_path.string() << '\n';
    for (std::size_t index = 0; index < file.replacements.size(); ++index) {
      const auto& replacement = file.replacements[index];
      preview << "  " << replacement.start.line + 1 << ':' << replacement.start.column + 1 << "  `"
              << displayText(file.originals[index]) << "` -> `" << displayText(replacement.text) << "`\n";
    }
    changed_ranges += file.replacements.size();
    prepared.push_back(std::move(file));
  }

  if (prepared.empty() && file_transaction.operations().empty()) return fail("The edit contains no applicable changes");
  std::ostringstream message;
  message << "Review " << changed_ranges << " text change(s) in " << prepared.size() << " file(s) and "
          << file_transaction.operations().size() << " file operation(s).\n";
  if (closed_files != 0) {
    message << closed_files << " [disk] file(s) are closed and will be saved immediately.\n";
  }
  message << "Open files remain unsaved. No changes are made until Apply.\n\n" << preview.str();

  delTimer(timer_id_);
  ConfirmTextDialog dialog(title + " preview", message.str(), this, user_settings_.language);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) {
    if (failure_reason) *failure_reason = "User cancelled the workspace edit.";
    publishEvent(EventSource::Lsp, EventSeverity::Warning, title + " cancelled; no files changed\n"); return false;
  }

  if (!file_transaction.apply(transaction_error)) return fail(transaction_error);
  std::vector<std::pair<Document*, std::filesystem::path>> relocated_documents;
  for (const auto index : file_transaction.appliedOperationIndices()) {
    const auto& operation = file_transaction.operations()[index];
    if (operation.kind != WorkspaceFileOperationKind::Rename) continue;
    for (auto& open_document : documents_) {
      if (open_document->path() != operation.path) continue;
      if (isCppSource(open_document->path())) lsp_.close(*open_document);
      relocated_documents.emplace_back(open_document.get(), operation.path);
      open_document->relocate(operation.new_path);
      if (isCppSource(open_document->path())) lsp_.open(*open_document);
    }
  }
  const auto rollbackResources = [this, &file_transaction, &relocated_documents](std::string& error) {
    const bool rolled_back = file_transaction.rollback(error);
    for (auto iterator = relocated_documents.rbegin(); iterator != relocated_documents.rend(); ++iterator) {
      if (isCppSource(iterator->first->path())) lsp_.close(*iterator->first);
      iterator->first->relocate(iterator->second);
      if (isCppSource(iterator->first->path())) lsp_.open(*iterator->first);
    }
    return rolled_back;
  };

  for (auto& file : prepared) {
    if (file.temporary) file.target->applyReplacements(file.replacements);
  }
  std::vector<PreparedFileEdit*> saved;
  for (auto& file : prepared) {
    if (!file.temporary) continue;
    std::string error;
    if (!file.target->save(error)) {
      bool rollback_failed{};
      file.target->setText(file.original_text);
      std::string failed_file_rollback_error;
      if (!file.target->save(failed_file_rollback_error)) rollback_failed = true;
      for (auto* rollback : saved) {
        rollback->target->setText(rollback->original_text);
        std::string rollback_error;
        if (!rollback->target->save(rollback_error)) rollback_failed = true;
      }
      std::string resource_rollback_error;
      if (!rollbackResources(resource_rollback_error)) rollback_failed = true;
      return fail(error + std::string(rollback_failed
        ? "; rollback of a saved file or file operation failed" : "; saved files and file operations were restored"));
    }
    saved.push_back(&file);
  }
  for (auto& file : prepared) {
    if (file.temporary) continue;
    file.target->applyReplacements(std::move(file.replacements));
    lsp_.change(*file.target);
  }
  std::string commit_error;
  if (!file_transaction.commit(commit_error))
    publishEvent(EventSource::Lsp, EventSeverity::Warning, "Workspace edit warning: cannot remove a transaction backup: " + commit_error + "\n");
  publishEvent(EventSource::Lsp, EventSeverity::Success, title + " applied " + std::to_string(changed_ranges) + " text change(s) in "
    + std::to_string(prepared.size()) + " file(s) and "
    + std::to_string(file_transaction.operations().size()) + " file operation(s)\n");
  if (failure_reason) failure_reason->clear();
  refreshFiles(); refreshTabs(); editor_.invalidateSyntax(); updateStatus();
  return true;
}

auto IdeWindow::launchCommand(LaunchCommand& command, std::string& error) -> bool {
  if (project_settings_.launch.executable.empty()) refreshCMakeTargets();
  const CMakeTarget* target{};
  if (!project_settings_.launch.target.empty()) {
    target = cmake_session_.targetNamed(project_settings_.launch.target);
    if (!target) {
      error = "Configured CMake target was not found: " + project_settings_.launch.target; return false;
    }
  } else target = cmake_session_.selectedTarget();
  if (!resolveLaunchCommand(root_, project_settings_.launch, target, command, error)) return false;
  if (!command.explicit_executable && target) {
    cmake_session_.rememberTarget(*target);
    debug_state_dirty_ = true; saveDebugState();
  }
  return true;
}

}  // namespace tuiide
