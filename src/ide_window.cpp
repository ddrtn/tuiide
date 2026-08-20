#include "tuiide/ide_window.hpp"

#include "tuiide/build_command.hpp"
#include "tuiide/debug_dialogs.hpp"
#include "tuiide/document_labels.hpp"
#include "tuiide/project_dialogs.hpp"
#include "tuiide/run_dialogs.hpp"
#include "tuiide/search_dialogs.hpp"
#include "tuiide/text_display.hpp"
#include "tuiide/ui_dialogs.hpp"
#include "tuiide/workspace_file_transaction.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <csignal>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

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
    {"search.problems", "Search: Problems", finalcut::FKey::Meta_e, "Alt+E"},
    {"run.debug", "Debug: Start / Continue", finalcut::FKey::F5, "F5"},
    {"run.run", "Run: Run", finalcut::FKey::F6, "F6"},
    {"run.build", "Run: Build", finalcut::FKey::F7, "F7"},
    {"debug.breakpoint", "Debug: Toggle Breakpoint", finalcut::FKey::F9, "F9"},
    {"debug.stepInto", "Debug: Step Into", finalcut::FKey::F11, "F11"},
    {"debug.stepOut", "Debug: Step Out", finalcut::FKey::F12, "F12"},
    {"debug.watch", "Debug: Add Watch", finalcut::FKey::Meta_w, "Alt+W"},
    {"tools.completion", "Tools: Completion", finalcut::FKey::Ctrl_space, "Ctrl+Space"},
    {"tools.hover", "Tools: Symbol Information", finalcut::FKey::F1, "F1"},
    {"tools.rename", "Tools: Rename Symbol", finalcut::FKey::F2, "F2"},
    {"tools.codeActions", "Tools: Code Actions", finalcut::FKey::Meta_a, "Alt+A"},
    {"window.previous", "Window: Previous File", finalcut::FKey::Ctrl_page_up, "Ctrl+PageUp"},
    {"window.next", "Window: Next File", finalcut::FKey::Ctrl_page_down, "Ctrl+PageDown"},
  };
  return commands;
}

auto shortcutKey(std::string value) -> std::optional<finalcut::FKey> {
  value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }), value.end());
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::toupper(character));
  });
  static const std::map<std::string, finalcut::FKey> keys{
    {"CTRL+A", finalcut::FKey::Ctrl_a}, {"CTRL+B", finalcut::FKey::Ctrl_b},
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
    {"ALT+A", finalcut::FKey::Meta_a}, {"ALT+B", finalcut::FKey::Meta_b}, {"ALT+E", finalcut::FKey::Meta_e},
    {"ALT+K", finalcut::FKey::Meta_k}, {"ALT+P", finalcut::FKey::Meta_p},
    {"ALT+R", finalcut::FKey::Meta_r}, {"ALT+T", finalcut::FKey::Meta_t},
    {"ALT+U", finalcut::FKey::Meta_u}, {"ALT+W", finalcut::FKey::Meta_w},
    {"ALT+SHIFT+W", finalcut::FKey::Meta_W},
    {"F1", finalcut::FKey::F1}, {"F2", finalcut::FKey::F2}, {"F3", finalcut::FKey::F3},
    {"F4", finalcut::FKey::F4}, {"F5", finalcut::FKey::F5}, {"F6", finalcut::FKey::F6},
    {"F7", finalcut::FKey::F7}, {"F8", finalcut::FKey::F8}, {"F9", finalcut::FKey::F9},
    {"F10", finalcut::FKey::F10}, {"F11", finalcut::FKey::F11}, {"F12", finalcut::FKey::F12},
  };
  const auto found = keys.find(value);
  return found == keys.end() ? std::nullopt : std::optional<finalcut::FKey>{found->second};
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
}  // namespace

IdeWindow::IdeWindow(std::filesystem::path initial_root, finalcut::FWidget* parent)
    : FDialog(parent), root_(project_session_.root()), build_dir_(cmake_session_.buildDirectory()),
      session_file_(project_session_.sessionFile()), recovery_file_(project_session_.recoveryFile()),
      project_history_file_(defaultProjectHistoryPath()), project_settings_(project_session_.settings()),
      documents_(document_session_.documents()), closed_documents_(document_session_.closedDocuments()),
      document_(document_session_.activeDocument()), active_document_(document_session_.activeIndex()) {
  setText("TUI IDE — C/C++");
  unsetBorder();
  unsetTitleBarButtonVisibility();
  menu_bar_.setForegroundColor(finalcut::FColor::Black);
  menu_bar_.setBackgroundColor(finalcut::FColor::LightGray);
  menu_bar_.delAccelerator();
  menu_bar_.addAccelerator(finalcut::FKey::F10, &menu_bar_);
  menu_bar_.addAccelerator(finalcut::FKey::Menu, &menu_bar_);
  setupMenus();
  sidebar_tabs_.addTab("Open files", tabs_);
  sidebar_tabs_.addTab("Project", files_);
  sidebar_tabs_.addTab("Outline", outline_);
  sidebar_tabs_.addTab("Debug", debug_);
  sidebar_tabs_.addTab("Breakpoints", breakpoints_);
  lower_tabs_.addTab("Output", output_);
  lower_tabs_.addTab("Problems", problems_);
  lower_tabs_.addTab("Build", build_output_);
  lower_tabs_.addTab("Terminal", console_);
  notification_.hide();
  output_.setText("Output");
  build_output_.setText("Build output");
  problems_.insert("No problems");
  problems_.setCommandHandler([this](finalcut::FKey key) {
    if (key == finalcut::FKey::Return) { openSelectedProblem(); return true; }
    return handleCommand(key);
  });
  problems_.addCallback("clicked", [this] { openSelectedProblem(); });
  console_.setInputHandler([this](std::string line) {
    if (!terminal_.running()) { appendOutput("Console input unavailable: no PTY program is running\n"); return; }
    line.push_back('\n');
    if (!terminal_.write(line)) appendOutput("Console input failed\n");
  });
  console_.setControlHandler([this](char control) {
    if (control == 'c') {
      if (gdb_.running() && gdb_.active()) gdb_.interrupt();
      else if (!terminal_.sendSignal(SIGINT)) appendOutput("Console interrupt unavailable\n");
    } else if (control == 'd' && terminal_.running()) {
      (void)terminal_.write("\x04");
    }
  });
  editor_.setChangedHandler([this] {
    if (document_ && isCppSource(document_->path())) lsp_.change(*document_);
    refreshTabs();
    updateStatus();
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
    if (key == finalcut::FKey::F5) { refreshFiles(); return true; }
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
  outline_.addCallback("row-selected", [this] { openSelectedOutlineSymbol(); });
  outline_.addCallback("clicked", [this] { openSelectedOutlineSymbol(); });

  timer_id_ = addTimer(100);
  ui_ready_ = true;
  std::string history_error;
  recent_projects_ = loadRecentProjects(project_history_file_, history_error);
  if (!history_error.empty()) appendOutput("Recent projects: " + history_error + "\n");
  if (initial_root.empty()) {
    setText("TUI IDE — No project");
    appendOutput("Welcome to TUI IDE. Use File > Open Project or File > New Project to begin.\n");
    refreshFiles(); refreshDebugPanel(); refreshBreakpointsPanel(); updateStatus();
  } else if (!loadProject(std::move(initial_root))) {
    setText("TUI IDE — No project");
    refreshFiles(); refreshDebugPanel(); refreshBreakpointsPanel(); updateStatus();
  }
  layout();
}

IdeWindow::~IdeWindow() {
  if (debug_state_dirty_) saveDebugState();
  lsp_.stop(); gdb_.stop(); build_session_.reset(); run_process_.stop(); terminal_.stop(); console_.setControlEnabled(false);
  gdb_.clearSessionState();
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
  debug_menu_.separator1.setSeparator(); debug_menu_.separator2.setSeparator();
  tools_menu_.separator.setSeparator(); tools_menu_.separator2.setSeparator(); tools_menu_.separator3.setSeparator();
  tools_menu_.separator4.setSeparator();
  window_menu_.separator.setSeparator();
  help_menu_.separator.setSeparator();

  const auto bind = [this](finalcut::FMenuItem& item, finalcut::FKey key, std::string message) {
    item.setStatusBarMessage(finalcut::FString(std::move(message)));
    item.addCallback("clicked", [this, key] { queueMenuCommand(key); });
  };
  file_menu_.new_project.addCallback("clicked", [this] { deferred_command_ = [this] { newProject(); }; });
  file_menu_.open_project.setStatusBarMessage("Open a directory containing CMakeLists.txt");
  file_menu_.open_project.addCallback("clicked", [this] { deferred_command_ = [this] { openProject(); }; });
  file_menu_.recent_projects.setStatusBarMessage("Open a recently used CMake project");
  file_menu_.recent_projects.addCallback("clicked", [this] { deferred_command_ = [this] { openRecentProject(); }; });
  file_menu_.close_project.setStatusBarMessage("Close the current project");
  file_menu_.close_project.addCallback("clicked", [this] { deferred_command_ = [this] { closeProject(); }; });
  bind(file_menu_.new_file, finalcut::FKey::Ctrl_n, "Create a new source file");
  file_menu_.new_project_file.setStatusBarMessage("Create a C/C++ file or class and add it to CMake");
  file_menu_.new_project_file.addCallback("clicked", [this] {
    deferred_command_ = [this] { createProjectFile(); };
  });
  bind(file_menu_.open, finalcut::FKey::Ctrl_o, "Open a C or C++ source file");
  bind(file_menu_.save, finalcut::FKey::Ctrl_s, "Save the active source file");
  file_menu_.save_all.setStatusBarMessage("Save every modified document");
  file_menu_.save_all.addCallback("clicked", [this] {
    deferred_command_ = [this] { (void)saveAllDocuments(); };
  });
  file_menu_.save_as.setStatusBarMessage("Save the active file under another name");
  file_menu_.save_as.addCallback("clicked", [this] { deferred_command_ = [this] { (void)saveAs(); }; });
  bind(file_menu_.close, finalcut::FKey::Ctrl_w, "Close the active file");
  file_menu_.close_others.setStatusBarMessage("Close every file except the active one");
  file_menu_.close_others.addCallback("clicked", [this] {
    deferred_command_ = [this] { closeOtherDocuments(); };
  });
  bind(file_menu_.close_all, finalcut::FKey::Meta_W, "Close every open file");
  bind(file_menu_.reopen_closed, finalcut::FKey::Meta_u, "Reopen the most recently closed saved file");
  file_menu_.quit.setStatusBarMessage("Exit TUI IDE");
  file_menu_.quit.addCallback("clicked", [this] { exitIde(); });

  bind(edit_menu_.undo, finalcut::FKey::Ctrl_z, "Undo the last edit");
  bind(edit_menu_.redo, finalcut::FKey::Ctrl_y, "Redo the last edit");
  bind(edit_menu_.cut, finalcut::FKey::Ctrl_x, "Cut selection to the system clipboard");
  bind(edit_menu_.copy, finalcut::FKey::Ctrl_c, "Copy selection to the system clipboard");
  bind(edit_menu_.paste, finalcut::FKey::Ctrl_v, "Paste from the system clipboard");
  bind(edit_menu_.select_all, finalcut::FKey::Ctrl_a, "Select the entire document");
  edit_menu_.toggle_comment.setStatusBarMessage("Comment or uncomment the selected lines");
  edit_menu_.toggle_comment.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.toggleComment(); };
  });
  edit_menu_.duplicate_line.setStatusBarMessage("Duplicate the current line or selected lines");
  edit_menu_.duplicate_line.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.duplicateLine(); };
  });
  edit_menu_.move_line_up.setStatusBarMessage("Move the current line or selected lines up");
  edit_menu_.move_line_up.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.moveLine(false); };
  });
  edit_menu_.move_line_down.setStatusBarMessage("Move the current line or selected lines down");
  edit_menu_.move_line_down.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.moveLine(true); };
  });
  edit_menu_.delete_line.setStatusBarMessage("Delete the current line or selected lines");
  edit_menu_.delete_line.addCallback("clicked", [this] {
    deferred_command_ = [this] { editor_.deleteLine(); };
  });

  bind(search_menu_.find, finalcut::FKey::Ctrl_f, "Find text in the active file");
  search_menu_.find_next.setStatusBarMessage("Find the next match using the current options");
  search_menu_.find_next.addCallback("clicked", [this] { deferred_command_ = [this] { findNext(false); }; });
  search_menu_.find_previous.setStatusBarMessage("Find the previous match using the current options");
  search_menu_.find_previous.addCallback("clicked", [this] { deferred_command_ = [this] { findNext(true); }; });
  search_menu_.replace.setStatusBarMessage("Find and replace text in a file or project");
  search_menu_.replace.addCallback("clicked", [this] { deferred_command_ = [this] { find(); }; });
  search_menu_.project_search.setStatusBarMessage("Search across editable project files");
  search_menu_.project_search.addCallback("clicked", [this] {
    deferred_command_ = [this] { search_project_ = true; find(); };
  });
  bind(search_menu_.go_to_line, finalcut::FKey::Ctrl_g, "Move to a line number");
  bind(search_menu_.definition, finalcut::FKey::F3, "Open the symbol definition");
  bind(search_menu_.references, finalcut::FKey::F4, "List symbol references");
  bind(search_menu_.problems, finalcut::FKey::Meta_e, "List build and clangd problems");

  project_menu_.refresh.addCallback("clicked", [this] { deferred_command_ = [this] { refreshFiles(); }; });
  project_menu_.filter.addCallback("clicked", [this] { deferred_command_ = [this] { filterProjectTree(); }; });
  project_menu_.clear_filter.addCallback("clicked", [this] {
    deferred_command_ = [this] { project_filter_.clear(); refreshFiles(); };
  });
  project_menu_.new_directory.addCallback("clicked", [this] { deferred_command_ = [this] { createProjectDirectory(); }; });
  project_menu_.rename_move.addCallback("clicked", [this] { deferred_command_ = [this] { renameSelectedProjectEntry(); }; });
  project_menu_.delete_directory.addCallback("clicked", [this] {
    deferred_command_ = [this] { deleteSelectedProjectDirectory(); };
  });

  run_menu_.configure.setStatusBarMessage("Configure the project with CMake");
  run_menu_.configure.addCallback("clicked", [this] { deferred_command_ = [this] { configure(); }; });
  bind(run_menu_.build, finalcut::FKey::F7, "Build the project; configure first when required");
  run_menu_.rebuild.setStatusBarMessage("Clean and build the project");
  run_menu_.rebuild.addCallback("clicked", [this] { deferred_command_ = [this] { rebuild(); }; });
  run_menu_.clean.setStatusBarMessage("Build the CMake clean target");
  run_menu_.clean.addCallback("clicked", [this] { deferred_command_ = [this] { clean(); }; });
  run_menu_.cancel_build.setStatusBarMessage("Stop the active CMake operation");
  run_menu_.cancel_build.addCallback("clicked", [this] { deferred_command_ = [this] { cancelBuild(); }; });
  bind(run_menu_.run, finalcut::FKey::F6, "Run the selected executable");
  run_menu_.stop_run.setStatusBarMessage("Terminate the running program and its process group");
  run_menu_.stop_run.addCallback("clicked", [this] { deferred_command_ = [this] { stopRun(); }; });
  run_menu_.launch_settings.setStatusBarMessage("Set target, arguments, environment, stdin, and launch behavior");
  run_menu_.launch_settings.addCallback("clicked", [this] { deferred_command_ = [this] { launchSettings(); }; });
  bind(run_menu_.configure_preset, finalcut::FKey::Meta_p, "Select a CMake configure preset");
  bind(run_menu_.build_preset, finalcut::FKey::Meta_b, "Select a CMake build preset");
  bind(run_menu_.target, finalcut::FKey::Meta_t, "Select an executable CMake target");

  bind(debug_menu_.start, finalcut::FKey::F5, "Start or continue debugging");
  bind(debug_menu_.pause, finalcut::FKey::F17, "Pause the debuggee");
  debug_menu_.stop.setStatusBarMessage("Stop the debug session and terminate GDB");
  debug_menu_.stop.addCallback("clicked", [this] { deferred_command_ = [this] { debugStop(); }; });
  debug_menu_.restart.setStatusBarMessage("Restart the selected executable under GDB");
  debug_menu_.restart.addCallback("clicked", [this] { deferred_command_ = [this] { debugRestart(); }; });
  bind(debug_menu_.breakpoint, finalcut::FKey::F9, "Toggle breakpoint on the current line");
  debug_menu_.breakpoint_properties.setStatusBarMessage("Edit condition, ignored hits, or logpoint message");
  debug_menu_.breakpoint_properties.addCallback("clicked", [this] { deferred_command_ = [this] { editSelectedBreakpoint(); }; });
  debug_menu_.breakpoint_enable.setStatusBarMessage("Enable or disable the selected breakpoint");
  debug_menu_.breakpoint_enable.addCallback("clicked", [this] { deferred_command_ = [this] { toggleSelectedBreakpoint(); }; });
  debug_menu_.breakpoint_remove.setStatusBarMessage("Remove the selected breakpoint");
  debug_menu_.breakpoint_remove.addCallback("clicked", [this] { deferred_command_ = [this] { removeSelectedBreakpoint(); }; });
  debug_menu_.breakpoint_clear.setStatusBarMessage("Remove every project breakpoint");
  debug_menu_.breakpoint_clear.addCallback("clicked", [this] { deferred_command_ = [this] { clearBreakpoints(); }; });
  bind(debug_menu_.next, finalcut::FKey::F34, "Step over the current source line");
  bind(debug_menu_.step, finalcut::FKey::F11, "Step into the current call");
  bind(debug_menu_.finish, finalcut::FKey::F12, "Finish the current stack frame");
  bind(debug_menu_.watch, finalcut::FKey::Meta_w, "Add a GDB watch expression");
  debug_menu_.evaluate.setStatusBarMessage("Evaluate a C/C++ expression in the selected stack frame");
  debug_menu_.evaluate.addCallback("clicked", [this] { deferred_command_ = [this] { evaluateExpression(); }; });
  debug_menu_.set_variable.setStatusBarMessage("Change a variable in the selected stack frame");
  debug_menu_.set_variable.addCallback("clicked", [this] { deferred_command_ = [this] { editVariableValue(); }; });
  debug_menu_.disassembly.setStatusBarMessage("Disassemble machine instructions near an address or $pc");
  debug_menu_.disassembly.addCallback("clicked", [this] { deferred_command_ = [this] { showDisassembly(); }; });
  debug_menu_.memory.setStatusBarMessage("Read a bounded memory range as hex and ASCII");
  debug_menu_.memory.addCallback("clicked", [this] { deferred_command_ = [this] { showMemory(); }; });
  bind(debug_menu_.registers, finalcut::FKey::Meta_r, "Show or hide amd64 registers");

  bind(tools_menu_.completion, finalcut::FKey::Ctrl_space, "Request clangd completion");
  tools_menu_.signature.setStatusBarMessage("Show clangd function signature help");
  tools_menu_.signature.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestSignatureHelp(); };
  });
  bind(tools_menu_.hover, finalcut::FKey::F1, "Show clangd symbol information");
  bind(tools_menu_.rename, finalcut::FKey::F2, "Rename a symbol across the workspace");
  bind(tools_menu_.code_actions, finalcut::FKey::Meta_a, "Show clangd quick fixes and refactorings");
  tools_menu_.organize_includes.setStatusBarMessage("Sort and remove unused includes with clangd");
  tools_menu_.organize_includes.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestCodeActions(true); };
  });
  tools_menu_.switch_source_header.setStatusBarMessage("Switch between matching C/C++ header and source files");
  tools_menu_.switch_source_header.addCallback("clicked", [this] {
    deferred_command_ = [this] { switchSourceHeader(); };
  });
  tools_menu_.workspace_symbols.setStatusBarMessage("Search symbols across the clangd workspace index");
  tools_menu_.workspace_symbols.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestWorkspaceSymbols(); };
  });
  tools_menu_.call_hierarchy.setStatusBarMessage("Show incoming and outgoing calls for the symbol at the cursor");
  tools_menu_.call_hierarchy.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestHierarchy(false); };
  });
  tools_menu_.type_hierarchy.setStatusBarMessage("Show supertypes and subtypes for the type at the cursor");
  tools_menu_.type_hierarchy.addCallback("clicked", [this] {
    deferred_command_ = [this] { requestHierarchy(true); };
  });
  tools_menu_.separator_lsp.setSeparator();
  tools_menu_.format_document.setStatusBarMessage("Format the active C/C++ document with clang-format");
  tools_menu_.format_document.addCallback("clicked", [this] {
    deferred_command_ = [this] { formatDocument(false); };
  });
  tools_menu_.format_selection.setStatusBarMessage("Format selected C/C++ lines with clang-format");
  tools_menu_.format_selection.addCallback("clicked", [this] {
    deferred_command_ = [this] { formatDocument(true); };
  });
  bind(tools_menu_.command_palette, finalcut::FKey::Meta_k, "Search and execute an IDE command");
  tools_menu_.configure_shortcut.setStatusBarMessage("Override a command shortcut for this project");
  tools_menu_.configure_shortcut.addCallback("clicked", [this] {
    deferred_command_ = [this] { configureShortcut(); };
  });
  tools_menu_.shortcut_conflicts.setStatusBarMessage("Show effective shortcuts and conflicts");
  tools_menu_.shortcut_conflicts.addCallback("clicked", [this] {
    deferred_command_ = [this] { showShortcutConflicts(); };
  });
  tools_menu_.theme.setStatusBarMessage("Select an accessible editor color theme");
  tools_menu_.theme.addCallback("clicked", [this] { deferred_command_ = [this] { selectTheme(); }; });
  tools_menu_.colors.setStatusBarMessage("Override a syntax or diagnostic color role");
  tools_menu_.colors.addCallback("clicked", [this] { deferred_command_ = [this] { configureEditorColor(); }; });
  tools_menu_.project_settings.setStatusBarMessage("Configure CMake, compilers, environment, and clangd");
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
  const auto togglePanel = [this](finalcut::FCheckMenuItem& item, std::size_t index) {
    auto* menu_item = &item;
    item.addCallback("clicked", [this, menu_item, index] {
      const bool show = menu_item->isChecked();
      deferred_command_ = [this, menu_item, index, show] {
        if (!sidebar_tabs_.setTabVisible(index, show, true)) {
          menu_item->setChecked();
          appendOutput("At least one sidebar panel must remain visible.\n");
        }
      };
    });
  };
  togglePanel(window_menu_.open_files, 0);
  togglePanel(window_menu_.project, 1);
  togglePanel(window_menu_.outline, 2);
  togglePanel(window_menu_.debug, 3);
  togglePanel(window_menu_.breakpoints, 4);
  window_menu_.clear_lower.setStatusBarMessage("Clear Output, Problems, Build, or Terminal content");
  window_menu_.clear_lower.addCallback("clicked", [this] { deferred_command_ = [this] { clearLowerPanel(); }; });
  window_menu_.copy_lower.setStatusBarMessage("Copy all text from the active lower panel");
  window_menu_.copy_lower.addCallback("clicked", [this] { deferred_command_ = [this] { copyLowerPanel(); }; });
  window_menu_.filter_problems.setStatusBarMessage("Filter diagnostics by file, severity, or message");
  window_menu_.filter_problems.addCallback("clicked", [this] { deferred_command_ = [this] { filterProblems(); }; });
  window_menu_.separator3.setSeparator();
  window_menu_.sidebar_narrower.addCallback("clicked", [this] { deferred_command_ = [this] { resizeSidebar(-2); }; });
  window_menu_.sidebar_wider.addCallback("clicked", [this] { deferred_command_ = [this] { resizeSidebar(2); }; });
  window_menu_.lower_shorter.addCallback("clicked", [this] { deferred_command_ = [this] { resizeLowerPanel(-1); }; });
  window_menu_.lower_taller.addCallback("clicked", [this] { deferred_command_ = [this] { resizeLowerPanel(1); }; });
  window_menu_.reset_panels.addCallback("clicked", [this] { deferred_command_ = [this] { resetPanelSizes(); }; });

  help_menu_.keyboard.setStatusBarMessage("Show the keyboard reference");
  help_menu_.keyboard.addCallback("clicked", [this] { deferred_command_ = [this] { showKeyboardHelp(); }; });
  help_menu_.about.setStatusBarMessage("About TUI IDE");
  help_menu_.about.addCallback("clicked", [this] { deferred_command_ = [this] { showAbout(); }; });
}

void IdeWindow::applyShortcutAccelerators() {
  const std::map<std::string_view, finalcut::FMenuItem*> items{
    {"file.new", &file_menu_.new_file}, {"file.open", &file_menu_.open},
    {"file.save", &file_menu_.save}, {"file.close", &file_menu_.close},
    {"file.closeAll", &file_menu_.close_all}, {"file.reopen", &file_menu_.reopen_closed},
    {"search.find", &search_menu_.find}, {"search.goToLine", &search_menu_.go_to_line},
    {"search.definition", &search_menu_.definition}, {"search.references", &search_menu_.references},
    {"search.problems", &search_menu_.problems}, {"run.debug", &debug_menu_.start},
    {"run.run", &run_menu_.run}, {"run.build", &run_menu_.build},
    {"debug.breakpoint", &debug_menu_.breakpoint}, {"debug.stepInto", &debug_menu_.step},
    {"debug.stepOut", &debug_menu_.finish}, {"debug.watch", &debug_menu_.watch},
    {"tools.completion", &tools_menu_.completion}, {"tools.hover", &tools_menu_.hover},
    {"tools.rename", &tools_menu_.rename}, {"tools.codeActions", &tools_menu_.code_actions},
    {"window.previous", &window_menu_.previous},
    {"window.next", &window_menu_.next},
  };
  for (const auto& command : ideCommands()) {
    const auto item = items.find(command.id);
    if (item == items.end()) continue;
    auto key = std::optional<finalcut::FKey>{command.default_key};
    const auto custom = project_settings_.shortcuts.find(std::string(command.id));
    if (custom != project_settings_.shortcuts.end()) key = shortcutKey(custom->second);
    item->second->delAccelerator();
    if (key) item->second->addAccelerator(*key);
  }
}

void IdeWindow::queueMenuCommand(finalcut::FKey key) {
  deferred_command_ = [this, key] {
    auto effective_key = key;
    const auto command = std::find_if(ideCommands().begin(), ideCommands().end(), [key](const auto& item) {
      return item.default_key == key;
    });
    if (command != ideCommands().end()) {
      const auto custom = project_settings_.shortcuts.find(std::string(command->id));
      if (custom != project_settings_.shortcuts.end()) {
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
  finalcut::FMessageBox::info(this, "About TUI IDE",
    "TUI IDE 0.1\nC/C++ terminal IDE for Linux/amd64\nFinal Cut + clangd + CMake + GDB/MI");
}

void IdeWindow::showKeyboardHelp() {
  finalcut::FMessageBox::info(this, "Keyboard shortcuts",
    "F10  Menu     Ctrl+N/O/S/W  Files\n"
    "F1/F2/F3/F4  Info/Rename/Definition/References\n"
    "F5/F6/F7     Debug/Run/Build\n"
    "F9           Breakpoint   Ctrl+F10  Next\n"
    "F11/F12      Step into/out\n"
    "Ctrl+F       Find/Replace text or project\n"
    "Alt+P/B/T    Configure/Build preset/Target\n"
    "Alt+K        Search the command palette\n"
    "Alt+A        clangd Code Actions / Quick Fixes\n"
    "Ctrl+E       Focus Project explorer\n"
    "Insert       Add file/class template in Project\n"
    "Delete       Remove selected Project file\n"
    "Window menu  Open files/Project/Debug panel");
}

void IdeWindow::showCommandPalette() {
  std::vector<std::string> items;
  for (const auto& command : ideCommands()) {
    const auto custom = project_settings_.shortcuts.find(std::string(command.id));
    items.push_back(std::string(command.title) + "  ["
      + (custom == project_settings_.shortcuts.end() ? std::string(command.default_shortcut) : custom->second) + "]");
  }
  delTimer(timer_id_);
  CommandPaletteDialog dialog(std::move(items), this);
  const auto selected = dialog.exec() == finalcut::FDialog::ResultCode::Accept ? dialog.selected() : 0;
  timer_id_ = addTimer(100);
  if (selected == 0 || selected > ideCommands().size()) return;
  const auto& command = ideCommands()[selected - 1];
  const auto custom = project_settings_.shortcuts.find(std::string(command.id));
  const auto key = custom == project_settings_.shortcuts.end()
    ? std::optional<finalcut::FKey>{command.default_key} : shortcutKey(custom->second);
  if (key) (void)handleCommand(*key);
}

void IdeWindow::configureShortcut() {
  if (root_.empty()) {
    appendOutput("Shortcut configuration unavailable: no project is open\n");
    return;
  }
  std::vector<std::string> items;
  for (const auto& command : ideCommands()) {
    const auto custom = project_settings_.shortcuts.find(std::string(command.id));
    items.push_back(std::string(command.title) + "  ["
      + (custom == project_settings_.shortcuts.end() ? std::string(command.default_shortcut) : custom->second) + "]");
  }
  const auto selected = choose("Configure shortcut", items);
  if (selected == 0 || selected > ideCommands().size()) return;
  const auto& command = ideCommands()[selected - 1];
  const auto value = prompt("Configure shortcut", "Shortcut (or Default):");
  if (value.empty()) return;
  auto normalized = value;
  normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](unsigned char character) {
    return std::isspace(character) != 0;
  }), normalized.end());
  if (normalized == "Default" || normalized == "default") {
    project_settings_.shortcuts.erase(std::string(command.id));
  } else {
    const auto key = shortcutKey(normalized);
    if (!key) {
      finalcut::FMessageBox::error(this,
        "Unsupported shortcut. Use Ctrl+letter, Alt+letter, Ctrl+PageUp/PageDown, or F1-F12.");
      return;
    }
    if (*key == finalcut::FKey::Meta_k) {
      finalcut::FMessageBox::error(this, "Alt+K is reserved for the command palette.");
      return;
    }
    for (const auto& other : ideCommands()) {
      if (other.id == command.id) continue;
      const auto custom = project_settings_.shortcuts.find(std::string(other.id));
      const auto other_key = custom == project_settings_.shortcuts.end()
        ? std::optional<finalcut::FKey>{other.default_key} : shortcutKey(custom->second);
      if (other_key && *other_key == *key) {
        finalcut::FMessageBox::error(this, finalcut::FString(
          "Shortcut conflicts with: " + std::string(other.title)));
        return;
      }
    }
    project_settings_.shortcuts[std::string(command.id)] = normalized;
  }
  std::string error;
  if (!saveProjectSettings(root_, project_settings_, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  applyShortcutAccelerators();
  appendOutput("Shortcut updated: " + std::string(command.title) + "\n");
}

void IdeWindow::showShortcutConflicts() {
  std::map<finalcut::FKey, std::vector<std::string>> owners;
  std::ostringstream table;
  table << "Effective shortcut table\n\n";
  owners[finalcut::FKey::Meta_k].push_back("Tools: Command Palette (reserved)");
  table << "Alt+K\tTools: Command Palette (reserved)\n";
  for (const auto& command : ideCommands()) {
    const auto custom = project_settings_.shortcuts.find(std::string(command.id));
    const auto label = custom == project_settings_.shortcuts.end()
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
  if (root_.empty()) { appendOutput("Theme settings unavailable: no project is open\n"); return; }
  const std::vector<std::string> themes{"Dark", "Light", "High contrast"};
  const auto selected = choose("Editor theme", themes);
  if (selected == 0 || selected > themes.size()) return;
  project_settings_.theme = themes[selected - 1];
  std::string error;
  if (!saveProjectSettings(root_, project_settings_, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  editor_.setTheme(project_settings_.theme, project_settings_.colors);
  appendOutput("Editor theme: " + project_settings_.theme + "\n");
}

void IdeWindow::configureEditorColor() {
  if (root_.empty()) { appendOutput("Color settings unavailable: no project is open\n"); return; }
  const std::vector<std::string> roles{"foreground", "background", "gutter", "breakpoint",
    "diagnosticError", "diagnosticWarning", "diagnosticNote", "selectionForeground", "selectionBackground",
    "plain", "keyword", "type", "string", "number", "comment", "preprocessor", "namespace",
    "function", "variable", "parameter", "property", "macro", "enumMember"};
  std::vector<std::string> role_items;
  for (const auto& role : roles) {
    const auto configured = project_settings_.colors.find(role);
    role_items.push_back(role + "  [" + (configured == project_settings_.colors.end() ? "theme default" : configured->second) + "]");
  }
  const auto role_index = choose("Editor color role", role_items);
  if (role_index == 0 || role_index > roles.size()) return;
  const std::vector<std::string> colors{"Theme default", "Black", "Blue", "Green", "Cyan", "Red", "Magenta",
    "Brown", "LightGray", "DarkGray", "LightBlue", "LightGreen", "LightCyan", "LightRed",
    "LightMagenta", "Yellow", "White"};
  const auto color_index = choose("Color for " + roles[role_index - 1], colors);
  if (color_index == 0 || color_index > colors.size()) return;
  const auto& role = roles[role_index - 1];
  if (color_index == 1) project_settings_.colors.erase(role);
  else project_settings_.colors[role] = colors[color_index - 1];
  std::string error;
  if (!saveProjectSettings(root_, project_settings_, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  editor_.setTheme(project_settings_.theme, project_settings_.colors);
  appendOutput("Editor color updated: " + role + "\n");
}

void IdeWindow::showTextDialog(std::string title, std::string text) {
  delTimer(timer_id_);
  TextDialog dialog(std::move(title), std::move(text), this);
  (void)dialog.exec();
  timer_id_ = addTimer(100);
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
        if (closeAllDocuments()) appendOutput("Close All: closed " + std::to_string(count) + " document(s)\n");
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
    project_context_->refresh.addCallback("clicked", [this] { deferred_command_ = [this] { refreshFiles(); }; });
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
      deferred_command_ = [this] { (void)handleCommand(finalcut::FKey::Meta_r); };
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
  if (terminal_.running()) (void)terminal_.resize(console_.columns(), console_.rows());
  status_.setGeometry({1, static_cast<int>(height)}, {width, 1});
  const auto workspace_width = width > sidebar ? width - sidebar : std::size_t{1};
  const auto notification_width = std::min<std::size_t>(48, workspace_width > 2 ? workspace_width - 2 : workspace_width);
  notification_.setGeometry({static_cast<int>(width - notification_width + 1), 2}, {notification_width, 1});
}

void IdeWindow::resizeSidebar(int delta) {
  const auto width = std::max<std::size_t>(60, getClientWidth());
  const auto current = sidebar_width_ == 0 ? std::clamp<std::size_t>(width / 3, 20, 38) : sidebar_width_;
  const auto changed = static_cast<long long>(current) + delta;
  sidebar_width_ = std::clamp<std::size_t>(changed < 0 ? 0 : static_cast<std::size_t>(changed), 18, width - 28);
  debug_state_dirty_ = true; saveDebugState(); layout(); redraw();
}

void IdeWindow::resizeLowerPanel(int delta) {
  const auto height = std::max<std::size_t>(18, getClientHeight());
  const auto current = lower_panel_height_ == 0 ? std::clamp<std::size_t>(height / 3, 5, 12) : lower_panel_height_;
  const auto changed = static_cast<long long>(current) + delta;
  lower_panel_height_ = std::clamp<std::size_t>(changed < 0 ? 0 : static_cast<std::size_t>(changed), 4, height - 9);
  debug_state_dirty_ = true; saveDebugState(); layout(); redraw();
}

void IdeWindow::resetPanelSizes() {
  sidebar_width_ = 0; lower_panel_height_ = 0;
  debug_state_dirty_ = true; saveDebugState(); layout(); redraw();
  appendOutput("Panel sizes reset to automatic defaults\n");
}

void IdeWindow::refreshFiles() {
  files_.clear(); file_paths_.clear(); project_item_paths_.clear();
  if (root_.empty()) { files_.redraw(); return; }
  ProjectTreeSnapshot snapshot;
  std::string scan_error;
  if (!scanProjectTree(root_, build_dir_, project_filter_, snapshot, scan_error)) {
    appendOutput("Project tree: " + scan_error + "\n"); files_.redraw(); return;
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
    appendOutput("Project tree: skipped " + std::to_string(snapshot.skipped_errors) + " inaccessible entries\n");
  files_.redraw();
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
  appendOutput(project_filter_.empty() ? "Project tree filter cleared\n"
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
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  refreshFiles(); appendOutput("Project directory created: " + (base / name).string() + "\n");
}

void IdeWindow::deleteSelectedProjectDirectory() {
  const auto* item = files_.getCurrentItem();
  const auto entry = item ? project_item_paths_.find(item) : project_item_paths_.end();
  if (entry == project_item_paths_.end() || normalizePath(entry->second) == root_) {
    finalcut::FMessageBox::info(this, "Project", "Select an empty project subdirectory."); return;
  }
  std::error_code relative_error;
  const auto relative = std::filesystem::relative(entry->second, root_, relative_error);
  const auto answer = finalcut::FMessageBox::info(this, "Delete directory",
    finalcut::FString("Delete empty directory from disk?\n" + relative.generic_string()),
    finalcut::FMessageBox::ButtonType::Yes, finalcut::FMessageBox::ButtonType::No,
    finalcut::FMessageBox::ButtonType::Reject);
  if (answer != finalcut::FMessageBox::ButtonType::Yes) return;
  std::string error;
  if (!tuiide::deleteEmptyProjectDirectory(root_, entry->second, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  refreshFiles(); appendOutput("Deleted empty project directory: " + relative.generic_string() + "\n");
}

void IdeWindow::renameSelectedProjectEntry() {
  const auto* item = files_.getCurrentItem();
  const auto entry = item ? project_item_paths_.find(item) : project_item_paths_.end();
  if (entry == project_item_paths_.end() || normalizePath(entry->second) == root_) {
    finalcut::FMessageBox::info(this, "Project", "Select a project file or subdirectory to rename."); return;
  }
  const auto source = normalizePath(entry->second);
  const auto modified_cmake = std::find_if(documents_.begin(), documents_.end(), [](const auto& document) {
    return isCMakePath(document->path()) && document->modified();
  });
  if (modified_cmake != documents_.end()) {
    finalcut::FMessageBox::error(this, "Save modified CMake files before Rename / Move."); return;
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
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
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
      if (!(*open)->load(changed_cmake, reload_error)) appendOutput("CMake reload: " + reload_error + "\n");
    }
  }
  refreshFiles(); refreshTabs(); editor_.invalidateSyntax(); updateStatus();
  appendOutput("Project entry moved: " + old_relative.generic_string() + " -> "
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
    finalcut::FMessageBox::info(this, "Project", "Select a file to remove. Directories are not removed.");
    return;
  }
  std::error_code relative_error;
  const auto relative = std::filesystem::relative(path, root_, relative_error);
  if (relative_error || relative.empty() || *relative.begin() == "..") {
    finalcut::FMessageBox::error(this, "The selected file is outside the project root.");
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
    finalcut::FMessageBox::error(this, "Save or close the modified file before deleting it.");
    return;
  }
  if (delete_from_disk) {
    const auto answer = finalcut::FMessageBox::info(this, "Delete file",
      finalcut::FString("Permanently delete from disk?\n" + relative.generic_string()),
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
  appendOutput("Project: " + cmake_message + (delete_from_disk ? "; file deleted: " : ": ")
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
  tabs_.redraw();
}

void IdeWindow::refreshDebugPanel() {
  if (!debug_ui_.updateDebug(DebugUiController::capture(gdb_))) return;
  debug_.clear();
  for (const auto& row : debug_ui_.debugRows()) debug_.insert(finalcut::FString(row.label));
  debug_.redraw();
}

void IdeWindow::refreshBreakpointsPanel() {
  const auto current = breakpoints_.currentItem();
  if (!debug_ui_.updateBreakpoints(gdb_.breakpoints(), gdb_.running(), root_)) return;
  breakpoints_.clear();
  if (debug_ui_.breakpointRows().empty()) breakpoints_.insert("No breakpoints");
  for (const auto& row : debug_ui_.breakpointRows()) breakpoints_.insert(finalcut::FString(row.label));
  if (!debug_ui_.breakpointRows().empty())
    breakpoints_.setCurrentItem(std::clamp<std::size_t>(current, 1, debug_ui_.breakpointRows().size()));
  breakpoints_.redraw();
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
    appendOutput("Breakpoint properties unavailable: select a breakpoint in the Breakpoints panel\n"); return;
  }
  auto breakpoint = row->breakpoint;
  delTimer(timer_id_); BreakpointSettingsDialog dialog(breakpoint, this);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept; timer_id_ = addTimer(100);
  if (!accepted) return;
  std::string error;
  if (!dialog.apply(breakpoint, error)) { finalcut::FMessageBox::error(this, finalcut::FString(error)); return; }
  if (!gdb_.updateBreakpoint(breakpoint)) { appendOutput("Breakpoint update failed\n"); return; }
  debug_state_dirty_ = true; saveDebugState(); debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
}

void IdeWindow::toggleSelectedBreakpoint() {
  const auto index = breakpoints_.currentItem();
  const auto* row = index > 0 ? debug_ui_.breakpointRow(index - 1) : nullptr;
  if (!row) {
    appendOutput("Enable breakpoint unavailable: select a breakpoint in the Breakpoints panel\n"); return;
  }
  auto breakpoint = row->breakpoint; breakpoint.enabled = !breakpoint.enabled;
  if (!gdb_.updateBreakpoint(breakpoint)) return;
  debug_state_dirty_ = true; saveDebugState(); debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
}

void IdeWindow::removeSelectedBreakpoint() {
  const auto index = breakpoints_.currentItem();
  const auto* row = index > 0 ? debug_ui_.breakpointRow(index - 1) : nullptr;
  if (!row) {
    appendOutput("Remove breakpoint unavailable: select a breakpoint in the Breakpoints panel\n"); return;
  }
  const auto breakpoint = row->breakpoint;
  if (!gdb_.removeBreakpoint(breakpoint.file, breakpoint.line)) return;
  debug_state_dirty_ = true; saveDebugState(); debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
}

void IdeWindow::clearBreakpoints() {
  if (gdb_.breakpoints().empty()) { appendOutput("Remove all breakpoints unavailable: no breakpoints exist\n"); return; }
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
      outline_.redraw();
    }
  }
  const auto decision = lsp_ui_.updateOutline(active, lsp_.ready(), maintenance_ticks_);
  if (decision == OutlineDecision::Clear) {
    outline_.clear();
    outline_.insert(active ? "Waiting for outline..." : "Open a C/C++ file for outline");
    outline_positions_.clear();
    outline_.redraw();
  } else if (decision == OutlineDecision::Request && document_) {
    outline_.clear(); outline_.insert("Loading outline..."); outline_positions_.clear(); outline_.redraw();
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
  if (root_.empty()) { appendOutput("Add watch unavailable: no project is open\n"); return; }
  const auto expression = prompt("Add watch", "Expression:");
  if (expression.empty()) return;
  if (!gdb_.addWatch(expression)) appendOutput("Watch already exists or is empty: " + expression + "\n");
  else { debug_state_dirty_ = true; saveDebugState(); }
  debug_ui_.invalidateDebug();
  refreshDebugPanel();
}

void IdeWindow::evaluateExpression() {
  if (!gdb_.stopped()) { appendOutput("Evaluate expression unavailable: debugger is not stopped\n"); return; }
  const auto expression = prompt("Evaluate expression", "Expression:");
  if (expression.empty()) return;
  if (!gdb_.evaluate(expression)) appendOutput("Evaluate expression failed: expression is empty or debugger is unavailable\n");
  else appendOutput("Evaluating: " + expression + "\n");
}

void IdeWindow::editVariableValue() {
  if (!gdb_.stopped()) { appendOutput("Set variable unavailable: debugger is not stopped\n"); return; }
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
  if (!gdb_.assign(expression, value)) appendOutput("Set variable failed: debugger is unavailable\n");
  else appendOutput("Assigning " + expression + " = " + value + "\n");
}

void IdeWindow::showDisassembly() {
  if (!gdb_.stopped()) { appendOutput("Disassembly unavailable: debugger is not stopped\n"); return; }
  const auto address = prompt("Disassembly", "Address/expression ($pc):");
  if (address.empty()) return;
  if (!gdb_.disassemble(address)) appendOutput("Disassembly request rejected\n");
  else appendOutput("Disassembling near " + address + "\n");
}

void IdeWindow::showMemory() {
  if (!gdb_.stopped()) { appendOutput("Memory view unavailable: debugger is not stopped\n"); return; }
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
    appendOutput("Memory view: byte count must be between 1 and 4096\n"); return;
  }
  if (!gdb_.readMemory(address, count)) appendOutput("Memory view request rejected\n");
  else appendOutput("Reading " + std::to_string(count) + " byte(s) at " + address + "\n");
}

void IdeWindow::removeSelectedWatch() {
  const auto index = debug_.currentItem();
  const auto* row = index > 0 ? debug_ui_.debugRow(index - 1) : nullptr;
  if (!row || !row->watch_index) {
    appendOutput("Remove watch unavailable: select a watch row in the Debug panel\n");
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
    appendOutput("Debug session: " + error + "\n");
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
    appendOutput("Debug session: " + error + "\n");
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
  tabs_.setCurrentItem(index + 1);
  editor_.setFocus();
  finalcut::FWidget::setFocusWidget(&editor_);
  if (isCppSource(document_->path()) && compilation_database_.available()
      && !compilation_database_.contains(document_->path())
      && compilation_database_warnings_.insert(document_->path()).second) {
    appendOutput("Compilation database: " + document_->path().string()
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
  clearRecovery(recovery_file_);
  if (debug_state_dirty_ && !root_.empty()) saveDebugState();
  lsp_.stop(); gdb_.stop(); build_session_.reset(); run_process_.stop(); terminal_.stop(); console_.setControlEnabled(false);
  gdb_.clearSessionState();
  project_session_.close();
  cmake_session_.reset();
  closed_documents_.clear();
  project_filter_.clear();
  applyShortcutAccelerators();
  editor_.setTheme("Dark", {});
  debug_ui_.reset(); debug_state_dirty_ = false; run_active_ = false;
  compilation_database_.clear(); compilation_database_warnings_.clear();
  lsp_ui_.reset();
  refreshFiles(); refreshTabs(); refreshDebugPanel();
  setText("TUI IDE — No project"); updateStatus();
}

auto IdeWindow::loadProject(std::filesystem::path root, std::filesystem::path build_directory) -> bool {
  ProjectSession next_project;
  ProjectOpenResult open_result;
  std::string open_error;
  if (!next_project.open(std::move(root), std::move(build_directory), open_result, open_error)) {
    appendOutput("Open Project error: " + open_error + "\n");
    return false;
  }
  unloadProject();
  project_session_ = std::move(next_project);
  cmake_session_.reset(project_session_.buildDirectory());
  if (open_result.used_default_settings)
    appendOutput("Project settings: " + open_result.warning + "; defaults are used\n");
  applyShortcutAccelerators();
  editor_.setIndentation(project_settings_.tab_width, project_settings_.use_spaces);
  editor_.setTheme(project_settings_.theme, project_settings_.colors);
  setText("TUI IDE — C/C++ — " + root_.filename().string());
  lsp_ui_.reset();
  loadDebugState();
  refreshCompilationDatabase(true);
  restartLanguageServer();
  refreshFiles(); updateStatus(); restoreRecovery();
  std::string history_error;
  if (!rememberRecentProject(project_history_file_, root_, history_error))
    appendOutput("Recent projects: " + history_error + "\n");
  recent_projects_ = loadRecentProjects(project_history_file_, history_error);
  if (!history_error.empty()) appendOutput("Recent projects: " + history_error + "\n");
  appendOutput("Project opened: " + root_.string() + "\n");
  return true;
}

void IdeWindow::closeProject() {
  if (root_.empty()) return;
  if (!closeAllDocuments()) return;
  unloadProject();
}

void IdeWindow::projectSettings() {
  if (root_.empty()) { appendOutput("Project Settings unavailable: no project is open\n"); return; }
  if (build_session_.running() || run_process_.running() || terminal_.running() || gdb_.running()) {
    appendOutput("Project Settings unavailable while build, program, or debugger is running\n");
    return;
  }
  delTimer(timer_id_);
  ProjectSettingsDialog dialog(root_, project_settings_, this);
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
  editor_.setIndentation(project_settings_.tab_width, project_settings_.use_spaces);
  editor_.setTheme(project_settings_.theme, project_settings_.colors);
  refreshCompilationDatabase(true);
  restartLanguageServer();
  appendOutput("Project settings saved; build directory: " + build_dir_.string() + "\n");
  updateStatus();
}

void IdeWindow::launchSettings() {
  if (root_.empty()) { appendOutput("Launch configuration unavailable: no project is open\n"); return; }
  if (build_session_.running() || run_process_.running() || terminal_.running() || gdb_.running()) {
    appendOutput("Launch configuration unavailable while build, program, or debugger is running\n");
    return;
  }
  refreshCMakeTargets();
  delTimer(timer_id_);
  LaunchSettingsDialog dialog(root_, project_settings_.launch, cmake_session_.targets(), this);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) return;
  LaunchConfiguration launch;
  std::string error;
  if (!dialog.configuration(launch, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  auto updated = project_settings_;
  updated.launch = std::move(launch);
  if (!saveProjectSettings(root_, updated, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error)); return;
  }
  project_settings_ = std::move(updated);
  const auto launch_name = !project_settings_.launch.executable.empty()
    ? project_settings_.launch.executable.string()
    : !project_settings_.launch.target.empty() ? project_settings_.launch.target : "selected CMake target";
  appendOutput("Launch configuration saved: " + launch_name + "\n");
  updateStatus();
}

void IdeWindow::openProject() {
  const auto start = root_.empty() ? std::filesystem::current_path() : root_.parent_path();
  delTimer(timer_id_);
  ProjectDirectoryDialog dialog("Open CMake project", start, this);
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
      else appendOutput("Project import cancelled\n");
      return;
    }
    finalcut::FMessageBox::error(this, finalcut::FString(validation_error));
    return;
  }
  if (normalizePath(selected) == root_) {
    appendOutput("Open Project: this project is already open\n");
    return;
  }
  if (!closeAllDocuments()) { appendOutput("Open Project cancelled: an open document was not closed\n"); return; }
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
  ImportProjectDialog settings_dialog(suggested_name, this);
  const auto accepted = settings_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  auto options = settings_dialog.options();
  timer_id_ = addTimer(100);
  if (!accepted) { appendOutput("Project import cancelled\n"); return; }
  options.project_directory = normalizePath(directory);

  delTimer(timer_id_);
  ProjectDirectoryDialog build_dialog("Select import build directory", directory.parent_path(), this);
  const auto build_accepted = build_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  options.build_directory = normalizePath(build_dialog.selectedPath());
  timer_id_ = addTimer(100);
  if (!build_accepted) { appendOutput("Project import cancelled\n"); return; }

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
  ConfirmTextDialog preview_dialog("Import preview", preview.str(), this);
  const auto confirmed = preview_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!confirmed) { appendOutput("Project import cancelled after preview\n"); return; }
  if (!closeAllDocuments()) {
    appendOutput("Project import cancelled: an open document was not closed\n");
    return;
  }
  if (!createImportedProject(options, plan, error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return;
  }
  if (loadProject(options.project_directory, options.build_directory)) {
    openFile(options.project_directory / "CMakeLists.txt");
    appendOutput("Imported " + std::to_string(plan.files.size()) + " source and header files\n");
  }
}

void IdeWindow::openRecentProject() {
  std::string history_error;
  recent_projects_ = loadRecentProjects(project_history_file_, history_error);
  if (!history_error.empty()) { appendOutput("Recent projects: " + history_error + "\n"); return; }
  if (recent_projects_.empty()) { appendOutput("Recent projects: no valid projects\n"); updateStatus(); return; }
  std::vector<std::string> labels;
  labels.reserve(recent_projects_.size());
  for (const auto& project : recent_projects_) labels.push_back(project.string());
  const auto selection = choose("Recent projects", labels);
  if (selection == 0 || selection > recent_projects_.size()) return;
  const auto selected = recent_projects_[selection - 1];
  if (selected == root_) { appendOutput("Open Project: this project is already open\n"); return; }
  if (!closeAllDocuments()) { appendOutput("Open Project cancelled: an open document was not closed\n"); return; }
  (void)loadProject(selected);
}

void IdeWindow::newProject() {
  delTimer(timer_id_); NewProjectDialog settings_dialog(this);
  const auto accepted = settings_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  auto options = settings_dialog.options(); timer_id_ = addTimer(100);
  if (!accepted) return;
  const auto start = root_.empty() ? std::filesystem::current_path() : root_.parent_path();
  delTimer(timer_id_); ProjectDirectoryDialog project_dialog("Select empty project directory", start, this);
  const auto project_accepted = project_dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  options.project_directory = project_dialog.selectedPath(); timer_id_ = addTimer(100);
  if (!project_accepted) return;
  delTimer(timer_id_); ProjectDirectoryDialog build_dialog("Select build directory", options.project_directory.parent_path(), this);
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
    appendOutput("Validating the generated project with CMake...\n");
    configure();
  }
}

void IdeWindow::createProjectFile() {
  if (root_.empty()) { finalcut::FMessageBox::error(this, "Open or create a project first."); return; }
  const auto changed_cmake = std::find_if(documents_.begin(), documents_.end(), [](const auto& open_document) {
    return isCMakePath(open_document->path()) && open_document->modified();
  });
  if (changed_cmake != documents_.end()) {
    finalcut::FMessageBox::error(this, "Save modified CMake files before adding a project file.");
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
    ClassOptionsDialog dialog(this);
    const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
    auto options = dialog.options();
    timer_id_ = addTimer(100);
    if (!accepted) return;
    if (!validateCppClassSettings(options, error)) {
      finalcut::FMessageBox::error(this, finalcut::FString(error));
      return;
    }
    const auto header = chooseProjectSavePath("C++ class header path", start_directory, options.class_name + ".hpp");
    if (!header) return;
    const auto source = chooseProjectSavePath("C++ class implementation path", header->parent_path(), options.class_name + ".cpp");
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
    if (!open_document->load(result.changed_cmake_file, error)) appendOutput("CMake reload: " + error + "\n");
    if (open_document.get() == document_) editor_.setDocument(document_);
  }
  refreshFiles();
  appendOutput("Project: created " + std::to_string(result.created_files.size()) + " file(s), added "
    + std::to_string(result.cmake_references_added) + " CMake reference(s) in "
    + result.changed_cmake_file.string() + "\n");
  if (!result.created_files.empty()) openFile(result.created_files.front());
}

auto IdeWindow::chooseProjectSavePath(std::string title, const std::filesystem::path& start,
                                      std::string suggested_name) -> std::optional<std::filesystem::path> {
  delTimer(timer_id_);
  ProjectPathDialog dialog(std::move(title), root_, start, std::move(suggested_name), this);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  const auto selected = dialog.selectedPath();
  timer_id_ = addTimer(100);
  return accepted ? std::optional<std::filesystem::path>{selected} : std::nullopt;
}

void IdeWindow::showCMakeCompletion() {
  if (!document_ || !isCMakePath(document_->path())) {
    appendOutput("CMake completion unavailable: the active document is not a CMake file\n");
    return;
  }
  const auto cursor = document_->cursor();
  const auto completions = completeCMake(document_->lines(), cursor.line, cursor.column);
  if (completions.empty()) { appendOutput("CMake completion: no suggestions\n"); return; }
  const auto selection = choose("CMake completion", completions);
  if (selection == 0 || selection > completions.size() || !document_ || !isCMakePath(document_->path())) return;
  document_->replaceIdentifierBeforeCursor(completions[selection - 1]);
  editor_.invalidateSyntax();
  refreshTabs();
  updateStatus();
}

void IdeWindow::requestSignatureHelp() {
  if (!document_) { appendOutput("Signature help unavailable: no document is open\n"); return; }
  if (!isCppSource(document_->path())) {
    appendOutput("Signature help unavailable: the active document is not a C/C++ source file\n"); return;
  }
  if (!lsp_.running()) { appendOutput("Signature help unavailable: clangd is not running\n"); return; }
  if (!lsp_.ready()) { appendOutput("Signature help unavailable: clangd is still initializing\n"); return; }
  lsp_.requestSignatureHelp(*document_);
}

void IdeWindow::requestCodeActions(bool organize_includes) {
  if (!document_ || !isCppSource(document_->path()) || !lsp_.ready()) {
    appendOutput(std::string(organize_includes ? "Organize Includes" : "Code Actions")
      + " unavailable: open a C/C++ file and wait for clangd\n");
    return;
  }
  if (organize_includes) {
    lsp_.requestOrganizeIncludes(*document_);
    appendOutput("Organize Includes: requesting changes from clangd\n");
    return;
  }
  auto start = document_->cursor();
  auto end = start;
  if (const auto selection = editor_.selectedRange()) { start = selection->first; end = selection->second; }
  lsp_.requestCodeActions(*document_, start, end);
  appendOutput("Code Actions: requesting fixes from clangd\n");
}

void IdeWindow::switchSourceHeader() {
  if (!document_ || !isCppSource(document_->path()) || !lsp_.ready()) {
    appendOutput("Switch Header/Source unavailable: open a C/C++ file and wait for clangd\n");
    return;
  }
  lsp_.requestSwitchSourceHeader(*document_);
}

void IdeWindow::requestWorkspaceSymbols() {
  if (root_.empty() || !lsp_.ready()) {
    appendOutput("Workspace Symbols unavailable: open a project and wait for clangd\n"); return;
  }
  const auto query = prompt("Workspace symbols", "Name or substring:");
  if (query.empty()) { appendOutput("Workspace Symbols cancelled: query is empty\n"); return; }
  lsp_.requestWorkspaceSymbols(query);
}

void IdeWindow::requestHierarchy(bool type_hierarchy) {
  if (!document_ || !isCppSource(document_->path()) || !lsp_.ready()) {
    appendOutput(std::string(type_hierarchy ? "Type Hierarchy" : "Call Hierarchy")
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
  // FWidget::close() only quits automatically while Final Cut still considers
  // this widget the main widget. Modal dialogs can disturb that bookkeeping,
  // so explicitly stop the application loop after an accepted close event.
  if (!close()) return;
  lsp_.stop();
  gdb_.stop();
  build_session_.reset();
  run_process_.stop();
  terminal_.stop();
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
      appendOutput("Close Others cancelled\n");
      return;
    }
  }
  const auto active = std::find_if(documents_.begin(), documents_.end(),
    [keep](const auto& open) { return open.get() == keep; });
  if (active != documents_.end())
    activateDocument(static_cast<std::size_t>(std::distance(documents_.begin(), active)));
  appendOutput("Close Others: closed " + std::to_string(original_count - documents_.size())
    + " document(s)\n");
}

void IdeWindow::reopenClosedDocument() {
  if (closed_documents_.empty()) { appendOutput("Reopen Closed unavailable: history is empty\n"); return; }
  const auto closed = document_session_.takeLastClosed();
  if (!closed) return;
  std::error_code error;
  if (!std::filesystem::is_regular_file(closed->path, error)) {
    appendOutput("Reopen Closed failed: file no longer exists: " + closed->path.string() + "\n");
    updateStatus();
    return;
  }
  openFile(closed->path);
  if (document_ && document_->path() == closed->path) {
    editor_.reveal(closed->cursor);
    appendOutput("Reopened: " + closed->path.string() + "\n");
  } else {
    appendOutput("Reopen Closed failed: cannot open " + closed->path.string() + "\n");
  }
}

void IdeWindow::switchDocument(int direction) {
  if (documents_.size() < 2) {
    appendOutput("Switch file unavailable: fewer than two documents are open\n");
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
}

auto IdeWindow::save() -> bool {
  if (!document_) return false;
  if (document_->path().empty()) return saveAs();
  std::string error;
  if (!document_->save(error)) {
    finalcut::FMessageBox::error(this, finalcut::FString(error));
    return false;
  }
  if (isCppSource(document_->path())) lsp_.change(*document_);
  refreshTabs(); updateStatus();
  return true;
}

auto IdeWindow::saveAllDocuments() -> bool {
  if (documents_.empty()) return true;
  const auto original = active_document_;
  std::size_t saved{};
  for (std::size_t index = 0; index < documents_.size(); ++index) {
    if (!documents_[index]->modified()) continue;
    activateDocument(index);
    if (!save()) {
      if (!documents_.empty()) activateDocument(std::min(original, documents_.size() - 1));
      appendOutput("Save All cancelled; project action was not started\n");
      return false;
    }
    ++saved;
  }
  if (!documents_.empty()) activateDocument(std::min(original, documents_.size() - 1));
  if (saved > 0) appendOutput("Save All: saved " + std::to_string(saved) + " document(s)\n");
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
  refreshFiles(); refreshTabs(); updateStatus();
  return true;
}

void IdeWindow::find() {
  SearchRequest initial{SearchAction::None, search_query_, search_replacement_, search_options_, search_project_};
  delTimer(timer_id_);
  SearchDialog dialog(initial, !root_.empty(), this);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) return;
  auto request = dialog.request();
  search_query_ = std::move(request.query);
  search_replacement_ = std::move(request.replacement);
  search_options_ = request.options;
  search_project_ = request.project;
  if (search_query_.empty()) { appendOutput("Find error: search text is empty\n"); return; }
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
        if (!error.empty()) appendOutput("Find error: " + error + "\n");
        else appendOutput("Find: " + std::to_string(matches.size()) + " match(es) in the active file\n");
      }
      break;
    case SearchAction::None: break;
  }
}

void IdeWindow::findNext(bool previous) {
  if (!document_) { appendOutput("Find unavailable: no document is open\n"); return; }
  if (search_query_.empty()) { find(); return; }
  std::string error;
  const auto matches = searchText(document_->text(), search_query_, search_replacement_, search_options_, error);
  if (!error.empty()) { appendOutput("Find error: " + error + "\n"); return; }
  if (matches.empty()) { appendOutput("Find: no matches for " + search_query_ + "\n"); return; }
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
  appendOutput("Find: match " + std::to_string(static_cast<std::size_t>(match - matches.data()) + 1)
    + " of " + std::to_string(matches.size()) + (wrapped ? " (wrapped)\n" : "\n"));
  updateStatus();
}

void IdeWindow::replaceCurrent() {
  if (!document_) { appendOutput("Replace unavailable: no document is open\n"); return; }
  std::string error;
  const auto matches = searchText(document_->text(), search_query_, search_replacement_, search_options_, error);
  if (!error.empty()) { appendOutput("Replace error: " + error + "\n"); return; }
  const auto selected = editor_.selectedRange();
  const auto match = std::find_if(matches.begin(), matches.end(), [&](const auto& candidate) {
    return selected && candidate.start == selected->first && candidate.end == selected->second;
  });
  if (match == matches.end()) {
    appendOutput("Replace: select a match first; moving to the next match\n");
    findNext(false);
    return;
  }
  document_->replaceRange(match->start, match->end, match->replacement);
  editor_.reveal(document_->cursor());
  editor_.invalidateSyntax();
  if (isCppSource(document_->path())) lsp_.change(*document_);
  refreshTabs(); updateStatus();
  appendOutput("Replace: changed 1 match\n");
  findNext(false);
}

void IdeWindow::replaceAll(bool project) {
  if (project) {
    if (root_.empty()) { appendOutput("Project replace unavailable: no project is open\n"); return; }
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
          appendOutput("Project replace error: " + error + "; no files changed\n"); return;
        }
        file.target = file.temporary.get();
        file.original_text = file.target->text();
      }
      file.matches = searchText(file.target->text(), search_query_, search_replacement_, search_options_, error);
      if (!error.empty()) { appendOutput("Project replace error: " + error + "; no files changed\n"); return; }
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
    if (prepared.empty()) { appendOutput("Project replace: no matches\n"); return; }
    std::ostringstream message;
    message << "Replace " << replacement_count << " match(es) in " << prepared.size() << " project file(s)?\n";
    if (closed_count != 0) message << closed_count << " [disk] file(s) will be saved immediately.\n";
    message << "Open files remain unsaved.\n\n" << preview.str();
    delTimer(timer_id_);
    ConfirmTextDialog dialog("Project replace preview", message.str(), this);
    const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
    timer_id_ = addTimer(100);
    if (!accepted) { appendOutput("Project replace cancelled; no files changed\n"); return; }

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
        appendOutput("Project replace error: " + error + "; open files were not changed"
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
    appendOutput("Project replace: changed " + std::to_string(replacement_count) + " match(es) in "
      + std::to_string(prepared.size()) + " file(s)\n");
    return;
  } else {
    if (!document_) { appendOutput("Replace all unavailable: no document is open\n"); return; }
    std::string error;
    const auto matches = searchText(document_->text(), search_query_, search_replacement_, search_options_, error);
    if (!error.empty()) { appendOutput("Replace all error: " + error + "\n"); return; }
    if (matches.empty()) { appendOutput("Replace all: no matches\n"); return; }
    std::vector<TextReplacement> replacements;
    replacements.reserve(matches.size());
    for (const auto& match : matches) replacements.push_back({match.start, match.end, match.replacement});
    document_->applyReplacements(std::move(replacements));
    editor_.reveal(document_->cursor()); editor_.invalidateSyntax();
    if (isCppSource(document_->path())) lsp_.change(*document_);
    refreshTabs(); updateStatus();
    appendOutput("Replace all: changed " + std::to_string(matches.size()) + " match(es) in the active file\n");
    return;
  }
}

void IdeWindow::showProjectSearch() {
  if (root_.empty()) { appendOutput("Project search unavailable: no project is open\n"); return; }
  struct Result { std::filesystem::path path; SearchMatch match; };
  std::vector<Result> results;
  std::vector<std::string> labels;
  for (const auto& path : file_paths_) {
    Document temporary;
    Document* source = &temporary;
    for (auto& open : documents_) if (open->path() == normalizePath(path)) { source = open.get(); break; }
    std::string error;
    if (source == &temporary && !temporary.load(path, error)) { appendOutput("Project search: " + error + "\n"); continue; }
    auto matches = searchText(source->text(), search_query_, search_replacement_, search_options_, error);
    if (!error.empty()) { appendOutput("Project search error: " + error + "\n"); return; }
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
  if (results.empty()) { appendOutput("Project search: no matches for " + search_query_ + "\n"); return; }
  const auto selection = choose("Project search — " + std::to_string(results.size()) + " result(s)", labels);
  if (selection == 0 || selection > results.size()) return;
  const auto result = results[selection - 1];
  openFile(result.path);
  editor_.selectRange(result.match.start, result.match.end);
  updateStatus();
}

void IdeWindow::goToLine() {
  if (!document_) { appendOutput("Go to line unavailable: no document is open\n"); return; }
  const auto value = prompt("Go to line", "Line number:");
  if (value.empty()) return;
  try {
    const auto line = static_cast<std::size_t>(std::stoul(value));
    if (line == 0 || line > document_->lines().size()) {
      appendOutput("Go to line error: enter a value from 1 to " + std::to_string(document_->lines().size()) + "\n");
      return;
    }
    editor_.reveal({line - 1, 0});
  } catch (...) { appendOutput("Invalid line number\n"); }
}

void IdeWindow::showProblems() {
  refreshProblemsPanel();
  lower_tabs_.setCurrentIndex(1, true);
  if (problem_rows_.empty()) appendOutput("No build or clangd diagnostics\n");
}

void IdeWindow::refreshProblemsPanel() {
  std::string signature = problems_filter_;
  for (const auto& diagnostic : build_session_.diagnostics()) signature += diagnostic.path.string()
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
  for (const auto& diagnostic : lsp_.diagnostics()) {
    const auto severity = diagnostic.severity == 1 ? "error" : diagnostic.severity == 2 ? "warning" : diagnostic.severity == 3 ? "info" : "hint";
    auto label = std::string("[clangd ") + severity + "] " + relative_label(diagnostic.path) + ":"
      + std::to_string(diagnostic.position.line + 1) + ":" + std::to_string(diagnostic.position.column + 1) + " " + diagnostic.message;
    if (!visible(label)) continue;
    problems_.insert(finalcut::FString(label)); problems_text_ += label + '\n';
    problem_rows_.push_back({diagnostic.path, diagnostic.position, true, diagnostic.message});
  }
  if (problem_rows_.empty()) problems_.insert(problems_filter_.empty() ? "No problems" : "No matching problems");
  problems_.redraw();
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
  appendOutput("Problem: " + problem.message + "\n");
}

void IdeWindow::filterProblems() {
  problems_filter_ = prompt("Filter Problems", "Text (empty clears):");
  problems_signature_.clear(); refreshProblemsPanel(); lower_tabs_.setCurrentIndex(1, true);
}

void IdeWindow::clearLowerPanel() {
  switch (lower_tabs_.currentIndex()) {
    case 0: output_text_.clear(); output_.clear(); break;
    case 1:
      build_session_.clearDiagnostics(); lsp_.clearDiagnostics(); problems_signature_.clear(); refreshProblemsPanel(); break;
    case 2: build_output_text_.clear(); build_output_.clear(); break;
    case 3: console_.clear(); break;
    default: break;
  }
}

void IdeWindow::copyLowerPanel() {
  std::string text;
  switch (lower_tabs_.currentIndex()) {
    case 0: text = output_text_; break; case 1: text = problems_text_; break;
    case 2: text = build_output_text_; break; case 3: text = console_.text(); break;
    default: break;
  }
  if (text.empty()) { appendOutput("Copy panel unavailable: active panel is empty\n"); return; }
  lower_clipboard_.copy(text); appendOutput("Copied active lower panel\n");
}

void IdeWindow::formatDocument(bool selection_only) {
  if (!document_ || !isCppSource(document_->path())) {
    appendOutput("Format unavailable: open a saved C or C++ source file\n");
    return;
  }
  std::optional<FormatLineRange> lines;
  if (selection_only) {
    const auto selection = editor_.selectedRange();
    if (!selection) { appendOutput("Format selection unavailable: no text is selected\n"); return; }
    auto last = selection->second.line;
    if (selection->second.column == 0 && last > selection->first.line) --last;
    lines = FormatLineRange{selection->first.line, last};
  }
  const auto original = document_->text();
  const auto result = clangFormat(original, document_->path(), lines);
  if (!result.success) {
    appendOutput(std::string(selection_only ? "Format selection error: " : "Format document error: ")
      + result.error + "\n");
    return;
  }
  if (result.text == original) {
    appendOutput(std::string(selection_only ? "Format selection" : "Format document") + ": no changes\n");
    return;
  }
  editor_.applyFormattedText(result.text);
  if (isCppSource(document_->path())) lsp_.change(*document_);
  refreshTabs(); updateStatus();
  appendOutput(std::string(selection_only ? "Formatted selected lines" : "Formatted document") + " with clang-format\n");
}

void IdeWindow::checkExternalChanges() {
  for (std::size_t index = 0; index < documents_.size(); ++index) {
    auto& open = *documents_[index];
    if (open.path().empty()) continue;
    std::string error;
    const auto change = open.diskChange(error);
    if (change == DiskChange::Unchanged) continue;
    if (change == DiskChange::Unreadable) {
      appendOutput("File watch error for " + open.path().string() + ": " + error + "\n");
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
      if (!open.load(open.path(), error)) appendOutput("Reload error: " + error + "\n");
      else {
        editor_.setDocument(&open); editor_.reveal(cursor);
        if (isCppSource(open.path())) lsp_.change(open);
        appendOutput("Reloaded external changes: " + open.path().string() + "\n");
      }
    } else if ((!deleted && selection == 2) || (deleted && selection == 1)) {
      open.acknowledgeDiskState();
      appendOutput(std::string(deleted ? "Kept editor contents after external deletion: "
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
  if (!saveRecovery(recovery_file_, recovery, error)) appendOutput("Recovery autosave error: " + error + "\n");
}

void IdeWindow::restoreRecovery() {
  if (recovery_file_.empty() || !std::filesystem::exists(recovery_file_)) return;
  std::vector<RecoveryDocument> recovery;
  std::string error;
  if (!loadRecovery(recovery_file_, recovery, error)) {
    appendOutput("Recovery error: " + error + "\n"); return;
  }
  if (recovery.empty()) { clearRecovery(recovery_file_); return; }
  const auto selection = choose("Crash recovery", {
    "Restore " + std::to_string(recovery.size()) + " autosaved document(s)",
    "Discard recovery data"
  });
  if (selection == 0) return;
  if (selection == 2) {
    clearRecovery(recovery_file_); appendOutput("Crash recovery discarded\n"); return;
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
  appendOutput("Crash recovery restored " + std::to_string(recovery.size()) + " document(s)\n");
}

void IdeWindow::build() {
  if (root_.empty()) { appendOutput("Build unavailable: no project is open\n"); return; }
  if (build_session_.running()) { appendOutput("Build unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(true)) return;
  build_session_.begin(BuildOperation::Build, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  if (build_session_.stage() == BuildStage::Build) (void)startBuildStage(false);
  else (void)startConfigureStage();
}

void IdeWindow::configure() {
  if (root_.empty()) { appendOutput("Configure unavailable: no project is open\n"); return; }
  if (build_session_.running()) { appendOutput("Configure unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(true)) return;
  build_session_.begin(BuildOperation::Configure, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  (void)startConfigureStage();
}

void IdeWindow::rebuild() {
  if (root_.empty()) { appendOutput("Rebuild unavailable: no project is open\n"); return; }
  if (build_session_.running()) { appendOutput("Rebuild unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(true)) return;
  build_session_.begin(BuildOperation::Rebuild, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  if (build_session_.stage() == BuildStage::Clean) (void)startBuildStage(true);
  else (void)startConfigureStage();
}

void IdeWindow::clean() {
  if (root_.empty()) { appendOutput("Clean unavailable: no project is open\n"); return; }
  if (build_session_.running()) { appendOutput("Clean unavailable: a CMake operation is already running\n"); return; }
  if (!beginBuildOperation(false)) return;
  build_session_.begin(BuildOperation::Clean, std::filesystem::exists(build_dir_ / "CMakeCache.txt"));
  if (!build_session_.running()) {
    appendOutput("Clean unavailable: configure the project first\n");
    build_session_.reset();
    return;
  }
  (void)startBuildStage(true);
}

auto IdeWindow::beginBuildOperation(bool save_documents) -> bool {
  if (save_documents && !saveAllDocuments()) {
    appendOutput("CMake operation cancelled because not all documents were saved\n"); return false;
  }
  if (!refreshCMakePresets()) return false;
  build_session_.prepare();
  build_output_text_.clear(); build_output_.clear();
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
  if (!createCMakeFileApiQuery(build_dir_, query_error)) appendOutput("CMake model: " + query_error + "\n");
  build_session_.enterStage(BuildStage::Configure);
  showNotification("CMake configure started", NotificationKind::Information);
  std::string command_error;
  auto command = BuildCommandService::configure(root_, build_dir_, project_settings_,
    cmake_session_.configurePreset(), cmake_session_.configurePresets(), command_error);
  if (!command) {
    appendOutput(command_error + "\n");
    finishBuildOperation(2, "Configure");
    return false;
  }
  appendBuildOutput(command->display + "\n");
  if (!build_session_.start(std::move(command->arguments),
      command->working_directory, project_settings_.environment)) {
    appendOutput("Failed to start CMake\n");
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
  appendBuildOutput(std::string(clean_stage ? "Clean" : "Build") + " stage: " + jobs + " parallel jobs\n");
  appendBuildOutput(command.display + "\n");
  if (!build_session_.start(std::move(command.arguments),
      command.working_directory, project_settings_.environment)) {
    appendOutput(std::string("Failed to start ") + (clean_stage ? "clean" : "build") + "\n");
    finishBuildOperation(127, clean_stage ? "Clean" : "Build");
    return false;
  }
  updateStatus();
  return true;
}

void IdeWindow::finishBuildOperation(int exit_code, std::string_view failed_stage) {
  const auto result = build_session_.finish(root_);
  if (result.diagnostics_added != 0) problems_signature_.clear();
  if (exit_code != 0 && !failed_stage.empty()) appendOutput(std::string(failed_stage) + " failed\n");
  std::ostringstream summary;
  summary.setf(std::ios::fixed); summary.precision(1);
  summary << result.operation << " finished with exit code " << exit_code
    << " after " << result.elapsed << " s\n";
  appendBuildOutput(summary.str()); appendOutput(summary.str());
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
  if (!build_session_.running()) { appendOutput("Cancel Build unavailable: no CMake operation is running\n"); return; }
  const auto result = build_session_.cancel(root_);
  for (const auto& chunk : result.output) appendBuildOutput(chunk);
  if (result.diagnostics_added != 0) problems_signature_.clear();
  std::ostringstream message; message.setf(std::ios::fixed); message.precision(1);
  message << "CMake operation cancelled during " << result.stage
    << " after " << result.elapsed << " s\n";
  appendBuildOutput(message.str()); appendOutput(message.str());
  showNotification("CMake operation cancelled", NotificationKind::Warning);
  updateStatus();
}

auto IdeWindow::refreshCMakePresets(bool report_error) -> bool {
  const auto result = cmake_session_.refreshPresets(root_, project_session_.buildDirectory());
  if (report_error && !result.configure_error.empty())
    appendOutput("CMake presets: " + result.configure_error + "\n");
  if (report_error && !result.build_error.empty())
    appendOutput("CMake build presets: " + result.build_error + "\n");
  if (report_error && !result.validation_error.empty())
    appendOutput(result.validation_error + "\n");
  return result.valid;
}

void IdeWindow::selectCMakePreset() {
  if (!refreshCMakePresets()) return;
  std::vector<std::string> labels{"No preset -> " + session_file_.parent_path().string()};
  for (const auto& preset : cmake_session_.configurePresets()) {
    auto label = preset.display_name + " [" + preset.name + "]";
    if (!preset.binary_directory.empty()) label += " -> " + preset.binary_directory.string();
    labels.push_back(std::move(label));
  }
  const auto selection = choose("CMake configure preset", labels);
  if (selection == 0 || selection > labels.size()) return;
  const auto name = selection == 1 ? std::string{}
    : cmake_session_.configurePresets()[selection - 2].name;
  if (!cmake_session_.selectConfigurePreset(name, project_session_.buildDirectory())) return;
  debug_state_dirty_ = true;
  saveDebugState();
  appendOutput("Selected configure preset: " + (cmake_session_.configurePreset().empty()
      ? std::string("none") : cmake_session_.configurePreset())
    + " (build directory " + build_dir_.string() + ")\n");
  refreshCompilationDatabase(true);
  restartLanguageServer();
  refreshFiles();
  updateStatus();
}

void IdeWindow::selectCMakeBuildPreset() {
  if (!refreshCMakePresets()) return;
  std::vector<const CMakeBuildPreset*> available;
  std::vector<std::string> labels{"No build preset"};
  for (const auto& preset : cmake_session_.buildPresets()) {
    const auto configure = std::ranges::find(cmake_session_.configurePresets(),
      preset.configure_preset, &CMakeConfigurePreset::name);
    if (configure == cmake_session_.configurePresets().end()) continue;
    available.push_back(&preset);
    auto label = preset.display_name + " [" + preset.name + "] / configure: " + preset.configure_preset;
    if (!preset.configuration.empty()) label += " / " + preset.configuration;
    if (!preset.targets.empty()) {
      label += " / targets:";
      for (const auto& target : preset.targets) label += " " + target;
    }
    labels.push_back(std::move(label));
  }
  const auto selection = choose("CMake build preset", labels);
  if (selection == 0 || selection > labels.size()) return;
  const auto name = selection == 1 ? std::string{} : available[selection - 2]->name;
  if (!cmake_session_.selectBuildPreset(name, project_session_.buildDirectory())) return;
  if (selection != 1) refreshFiles();
  debug_state_dirty_ = true;
  saveDebugState();
  appendOutput("Selected build preset: " + (cmake_session_.buildPreset().empty()
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
  if (cmake_session_.targets().empty() && !error.empty()) appendOutput("CMake model: " + error + "\n");
}

void IdeWindow::selectCMakeTarget() {
  refreshCMakeTargets();
  if (cmake_session_.targets().empty()) { appendOutput("No executable CMake targets; build the project first (F7)\n"); return; }
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
  appendOutput("Selected target: " + labels[selection - 1] + "\n");
  updateStatus();
}

void IdeWindow::run() {
  if (root_.empty()) { appendOutput("Run unavailable: no project is open\n"); return; }
  if (build_session_.running()) { appendOutput("Run unavailable: a CMake operation is in progress\n"); return; }
  if (!saveAllDocuments()) { appendOutput("Run cancelled because not all documents were saved\n"); return; }
  if (project_settings_.launch.pre_launch_build) {
    (void)startPreLaunchBuild(BuildContinuation::Run);
    return;
  }
  startRun();
}

void IdeWindow::startRun() {
  LaunchCommand launch;
  std::string error;
  if (!launchCommand(launch, error)) { appendOutput("Run unavailable: " + error + "\n"); return; }
  run_process_.stop(); terminal_.stop(); console_.setControlEnabled(false);
  auto arguments = launch.external_terminal ? launchProcessArguments(launch) : integratedLaunchArguments(launch);
  auto environment = project_settings_.environment;
  for (const auto& [name, value] : launch.environment) environment[name] = value;
  appendOutput("$ " + launch.executable.string()
    + (launch.arguments.empty() ? std::string{} : " " + formatArgumentList(launch.arguments)) + "\n");
  if (!launch.external_terminal) {
    console_.clear(); console_.setControlEnabled(true); lower_tabs_.setCurrentIndex(3, true); console_.focusInput();
  }
  run_active_ = launch.external_terminal
    ? run_process_.start(arguments, true, launch.working_directory, environment)
    : terminal_.start(arguments, launch.working_directory, environment, console_.columns(), console_.rows());
  if (!run_active_) {
    console_.setControlEnabled(false);
    appendOutput("Run failed: cannot start " + launch.executable.string() + "\n");
    showNotification("Program failed to start", NotificationKind::Error);
  } else showNotification("Program started", NotificationKind::Information);
}

void IdeWindow::stopRun() {
  if (!run_active_) { appendOutput("Stop Program unavailable: no program is running\n"); return; }
  run_process_.stop(); terminal_.stop(); run_active_ = false;
  console_.setControlEnabled(false);
  appendOutput("Run terminated by user\n");
  showNotification("Program terminated by user", NotificationKind::Warning); updateStatus();
}

void IdeWindow::debugRun() {
  if (root_.empty()) { appendOutput("Debug unavailable: no project is open\n"); return; }
  if (build_session_.running()) { appendOutput("Debug unavailable: a CMake operation is in progress\n"); return; }
  if (!gdb_.running()) {
    if (!saveAllDocuments()) { appendOutput("Debug cancelled because not all documents were saved\n"); return; }
    if (project_settings_.launch.pre_launch_build) {
      (void)startPreLaunchBuild(BuildContinuation::Debug);
      return;
    }
    startDebug();
  } else if (!gdb_.active()) {
    if (!saveAllDocuments()) { appendOutput("Debug cancelled because not all documents were saved\n"); return; }
    if (project_settings_.launch.pre_launch_build) {
      gdb_.stop();
      terminal_.stop();
      console_.setControlEnabled(false);
      (void)startPreLaunchBuild(BuildContinuation::Debug);
      return;
    }
    appendOutput("GDB: starting program again\n");
    gdb_.run();
  } else if (gdb_.stopped()) gdb_.continueExecution();
  else appendOutput("Debug continue unavailable: debuggee is already running\n");
}

void IdeWindow::startDebug() {
  LaunchCommand launch;
  std::string error;
  if (!launchCommand(launch, error)) { appendOutput("Debug unavailable: " + error + "\n"); return; }
  auto environment = project_settings_.environment;
  for (const auto& [name, value] : launch.environment) environment[name] = value;
  if (launch.external_terminal)
    appendOutput("Debug: external terminal is a Run-only setting; using the integrated GDB console\n");
  terminal_.stop(); console_.setControlEnabled(false); console_.clear();
  if (!terminal_.openSession(console_.columns(), console_.rows())) {
    appendOutput("Failed to create debuggee PTY\n"); return;
  }
  if (!gdb_.start(launch.executable, launch.working_directory, environment, launch.arguments,
      launch.stdin_file, terminal_.slaveName())) {
    terminal_.stop(); console_.setControlEnabled(false);
    appendOutput("Failed to start GDB\n"); return;
  }
  terminal_.activateSession(); console_.setControlEnabled(true); lower_tabs_.setCurrentIndex(3, true); console_.focusInput();
  appendOutput("GDB: " + launch.executable.string() + "\n");
  gdb_.run();
  showNotification("Debug session started", NotificationKind::Information);
}

void IdeWindow::debugStop() {
  if (!gdb_.running()) { appendOutput("Debug Stop unavailable: debugger is not started\n"); return; }
  gdb_.stop();
  terminal_.stop();
  console_.setControlEnabled(false);
  debug_ui_.invalidateDebug();
  refreshDebugPanel(); updateStatus();
  appendOutput("Debug session stopped\n");
  showNotification("Debug session stopped", NotificationKind::Warning);
}

void IdeWindow::debugRestart() {
  if (root_.empty()) { appendOutput("Debug Restart unavailable: no project is open\n"); return; }
  if (build_session_.running()) { appendOutput("Debug Restart unavailable: a CMake operation is in progress\n"); return; }
  if (!gdb_.running()) { appendOutput("Debug Restart unavailable: debugger is not started\n"); return; }
  if (!saveAllDocuments()) { appendOutput("Debug Restart cancelled because not all documents were saved\n"); return; }
  if (project_settings_.launch.pre_launch_build) {
    gdb_.stop(); terminal_.stop(); console_.setControlEnabled(false); debug_ui_.invalidateDebug(); refreshDebugPanel();
    (void)startPreLaunchBuild(BuildContinuation::Debug);
    return;
  }
  LaunchCommand launch;
  std::string error;
  if (!launchCommand(launch, error)) { appendOutput("Debug Restart unavailable: " + error + "\n"); return; }
  gdb_.stop();
  terminal_.stop(); console_.setControlEnabled(false);
  debug_ui_.invalidateDebug(); refreshDebugPanel();
  auto environment = project_settings_.environment;
  for (const auto& [name, value] : launch.environment) environment[name] = value;
  if (!terminal_.openSession(console_.columns(), console_.rows())) {
    appendOutput("Debug Restart failed: cannot create debuggee PTY\n"); updateStatus(); return;
  }
  if (!gdb_.start(launch.executable, launch.working_directory, environment, launch.arguments,
      launch.stdin_file, terminal_.slaveName())) {
    terminal_.stop(); console_.setControlEnabled(false);
    appendOutput("Debug Restart failed: cannot start GDB\n"); updateStatus(); return;
  }
  terminal_.activateSession(); console_.setControlEnabled(true); lower_tabs_.setCurrentIndex(3, true); console_.focusInput();
  appendOutput("GDB restarted: " + launch.executable.string() + "\n");
  gdb_.run();
  showNotification("Debug session restarted", NotificationKind::Information);
  updateStatus();
}

void IdeWindow::appendOutput(std::string_view text) {
  output_text_.append(text);
  if (output_text_.size() > 150000) output_text_.erase(0, output_text_.size() - 120000);
  output_.setText(finalcut::FString(output_text_));
  output_.scrollToX(0); output_.scrollToEnd();
  output_.redraw();
}

void IdeWindow::appendBuildOutput(std::string_view text) {
  build_output_text_.append(text);
  if (build_output_text_.size() > 300000)
    build_output_text_.erase(0, build_output_text_.size() - 240000);
  build_output_.setText(finalcut::FString(build_output_text_));
  build_output_.scrollToX(0); build_output_.scrollToEnd(); build_output_.redraw();
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
    if (report) appendOutput("Compilation database error: " + error + "\n");
    return;
  }
  if (!report) return;
  if (compilation_database_.available()) {
    appendOutput("Compilation database: " + compilation_database_.path().string() + " ("
      + std::to_string(compilation_database_.size()) + " source file(s))\n");
  } else {
    appendOutput("Compilation database is missing in " + build_dir_.string()
      + "; run CMake Configure to generate it\n");
  }
}

void IdeWindow::restartLanguageServer() {
  if (root_.empty()) return;
  lsp_.stop();
  lsp_ui_.reset();
  if (!lsp_.start(root_, project_settings_.clangd_arguments, project_settings_.environment, build_dir_)) {
    appendOutput("clangd unavailable: failed to start clangd\n");
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
    .run_running = run_active_,
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
  enabled(project_menu_.refresh, state.project_panel);
  enabled(project_menu_.filter, state.project_panel);
  enabled(project_menu_.clear_filter, state.project_panel && !project_filter_.empty());
  enabled(project_menu_.new_directory, state.project_panel);
  enabled(project_menu_.rename_move, state.project_panel);
  enabled(project_menu_.delete_directory, state.project_panel);
  enabled(run_menu_.configure, state.build);
  enabled(run_menu_.build, state.build);
  enabled(run_menu_.rebuild, state.build);
  enabled(run_menu_.clean, state.build);
  enabled(run_menu_.cancel_build, state.cancel_build);
  enabled(run_menu_.run, state.run);
  enabled(run_menu_.stop_run, state.stop_run);
  enabled(run_menu_.launch_settings, state.cmake_configuration);
  enabled(run_menu_.configure_preset, state.cmake_configuration);
  enabled(run_menu_.build_preset, state.cmake_configuration);
  enabled(run_menu_.target, state.cmake_configuration);
  enabled(debug_menu_.start, state.debug_start);
  enabled(debug_menu_.pause, state.debug_pause);
  enabled(debug_menu_.stop, state.debug_stop);
  enabled(debug_menu_.restart, state.debug_restart);
  enabled(debug_menu_.breakpoint, state.breakpoint);
  enabled(debug_menu_.breakpoint_properties, state.debug_panel);
  enabled(debug_menu_.breakpoint_enable, state.debug_panel);
  enabled(debug_menu_.breakpoint_remove, state.debug_panel);
  enabled(debug_menu_.breakpoint_clear, state.debug_panel);
  enabled(debug_menu_.next, state.debug_step);
  enabled(debug_menu_.step, state.debug_step);
  enabled(debug_menu_.finish, state.debug_step);
  enabled(debug_menu_.watch, state.watch);
  enabled(debug_menu_.evaluate, state.debug_step);
  enabled(debug_menu_.set_variable, state.debug_step);
  enabled(debug_menu_.disassembly, state.debug_step);
  enabled(debug_menu_.memory, state.debug_step);
  enabled(debug_menu_.registers, state.registers);
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
  enabled(tools_menu_.project_settings, state.cmake_configuration);
  enabled(window_menu_.previous, state.switch_document);
  enabled(window_menu_.next, state.switch_document);
  enabled(window_menu_.open_files, true);
  enabled(window_menu_.project, true);
  enabled(window_menu_.outline, true);
  enabled(window_menu_.debug, true);
  enabled(window_menu_.breakpoints, true);
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
  if (root_.empty()) text << "No project";
  else if (document_) {
    const auto cursor = document_->cursor();
    const auto column = displayColumn(document_->line(cursor.line), cursor.column, project_settings_.tab_width);
    text << (document_->modified() ? "● " : "  ") << document_->path().filename().string()
      << "  Ln " << cursor.line + 1 << ", Col " << column + 1
      << "  UTF-8" << (document_->hasUtf8Bom() ? " BOM" : "")
      << ' ' << (document_->lineEnding() == LineEnding::CrLf ? "CRLF" : "LF")
      << (document_->hasFinalNewline() ? "" : " no-final-EOL");
  } else text << "No file";
  text << "  | " << documents_.size() << " file(s) | clangd: " << (lsp_.running() ? "on" : "off")
       << " | CDB: " << (!compilation_database_.available() ? "missing"
         : (!document_ || !isCppSource(document_->path()) ? "ready"
           : (compilation_database_.contains(document_->path()) ? "entry" : "fallback")))
       << " | gdb: " << (gdb_.exited() ? "exited" : (gdb_.running() ? (gdb_.stopped() ? "stopped" : "running") : "off"))
       << " | cmake: ";
  if (!build_session_.running()) text << "idle";
  else {
    text << (build_session_.stage() == BuildStage::Configure ? "configure"
      : build_session_.stage() == BuildStage::Clean ? "clean" : "build");
    if (const auto progress = build_session_.progress()) text << ' ' << *progress << '%';
    text << ' ' << static_cast<unsigned>(build_session_.stageElapsed()) << 's';
  }
  const auto* selected_target = cmake_session_.selectedTarget();
  text
       << " | preset: " << (cmake_session_.configurePreset().empty()
         ? "none" : cmake_session_.configurePreset())
       << "/" << (cmake_session_.buildPreset().empty()
         ? "default" : cmake_session_.buildPreset())
       << " | target: " << (!project_settings_.launch.executable.empty()
         ? "launch:" + project_settings_.launch.executable.filename().string()
         : (selected_target ? selected_target->name : "unselected"))
       << " | F7 Build  F6 Run  F5 Debug  F9 Break  Ctrl+Space Complete";
  status_.setText(finalcut::FString(text.str())); status_.redraw();
}

auto IdeWindow::handleCommand(finalcut::FKey key) -> bool {
  bool custom_match{};
  for (const auto& command : ideCommands()) {
    const auto custom = project_settings_.shortcuts.find(std::string(command.id));
    if (custom == project_settings_.shortcuts.end()) continue;
    const auto configured = shortcutKey(custom->second);
    if (configured && *configured == key) {
      key = command.default_key;
      custom_match = true;
      break;
    }
  }
  if (!custom_match) {
    for (const auto& command : ideCommands()) {
      if (command.default_key == key && project_settings_.shortcuts.contains(std::string(command.id)))
        return true;
    }
  }
  const auto requireLspDocument = [this](std::string_view command) {
    if (!document_) { appendOutput(std::string(command) + " unavailable: no document is open\n"); return false; }
    if (!isCppSource(document_->path())) {
      appendOutput(std::string(command) + " unavailable: the active document is not a C/C++ source file\n");
      return false;
    }
    if (!lsp_.running()) { appendOutput(std::string(command) + " unavailable: clangd is not running\n"); return false; }
    if (!lsp_.ready()) { appendOutput(std::string(command) + " unavailable: clangd is still initializing\n"); return false; }
    return true;
  };
  const auto requireStoppedDebugger = [this](std::string_view command) {
    if (!gdb_.running()) { appendOutput(std::string(command) + " unavailable: debugger is not started\n"); return false; }
    if (!gdb_.active()) { appendOutput(std::string(command) + " unavailable: the program has exited\n"); return false; }
    if (!gdb_.stopped()) { appendOutput(std::string(command) + " unavailable: debuggee is running\n"); return false; }
    return true;
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
      if (!document_) appendOutput("Save unavailable: no document is open\n"); else (void)save();
      return true;
    case finalcut::FKey::Ctrl_w:
      if (!document_) appendOutput("Close unavailable: no document is open\n"); else closeActiveDocument();
      return true;
    case finalcut::FKey::Meta_W: {
      if (documents_.empty()) { appendOutput("Close All unavailable: no document is open\n"); return true; }
      const auto count = documents_.size();
      if (closeAllDocuments()) appendOutput("Close All: closed " + std::to_string(count) + " document(s)\n");
      else appendOutput("Close All cancelled\n");
      return true;
    }
    case finalcut::FKey::Meta_u: reopenClosedDocument(); return true;
    case finalcut::FKey::Ctrl_page_up: switchDocument(-1); return true;
    case finalcut::FKey::Ctrl_page_down: switchDocument(1); return true;
    case finalcut::FKey::Ctrl_f:
      if (!document_) appendOutput("Find unavailable: no document is open\n");
      else deferred_command_ = [this] { find(); };
      return true;
    case finalcut::FKey::Ctrl_g:
      if (!document_) appendOutput("Go to line unavailable: no document is open\n");
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
      // A focused child widget can be in the middle of dispatching this key.
      // Closing the main window synchronously from that callback leaves Final
      // Cut's event loop active. Close on the next timer tick instead.
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
    case finalcut::FKey::Meta_w:
      if (root_.empty()) appendOutput("Add watch unavailable: no project is open\n");
      else deferred_command_ = [this] { addWatch(); };
      return true;
    case finalcut::FKey::Meta_a:
      if (requireLspDocument("Code Actions")) requestCodeActions(false);
      return true;
    case finalcut::FKey::Meta_p:
      if (root_.empty()) appendOutput("Configure preset unavailable: no project is open\n");
      else if (build_session_.running()) appendOutput("Configure preset unavailable: a build is in progress\n");
      else deferred_command_ = [this] { selectCMakePreset(); };
      return true;
    case finalcut::FKey::Meta_b:
      if (root_.empty()) appendOutput("Build preset unavailable: no project is open\n");
      else if (build_session_.running()) appendOutput("Build preset unavailable: a build is in progress\n");
      else deferred_command_ = [this] { selectCMakeBuildPreset(); };
      return true;
    case finalcut::FKey::Meta_t:
      if (root_.empty()) appendOutput("Target selection unavailable: no project is open\n");
      else if (build_session_.running()) appendOutput("Target selection unavailable: a build is in progress\n");
      else deferred_command_ = [this] { selectCMakeTarget(); };
      return true;
    case finalcut::FKey::Meta_e:
      if (root_.empty()) appendOutput("Problems unavailable: no project is open\n");
      else deferred_command_ = [this] { showProblems(); };
      return true;
    case finalcut::FKey::Meta_r:
      if (root_.empty()) { appendOutput("Registers unavailable: no project is open\n"); return true; }
      gdb_.setRegistersEnabled(!gdb_.registersEnabled());
      debug_state_dirty_ = true; saveDebugState();
      appendOutput(std::string("Register view ") + (gdb_.registersEnabled() ? "enabled\n" : "disabled\n"));
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
    case finalcut::FKey::F7: build(); return true;
    case finalcut::FKey::F8: {
      const auto& diagnostics = lsp_.diagnostics();
      const auto count = build_session_.diagnostics().size() + diagnostics.size();
      if (count == 0) { appendOutput("No build or clangd diagnostics\n"); return true; }
      diagnostic_index_ %= count;
      if (diagnostic_index_ < build_session_.diagnostics().size()) {
        const auto& diagnostic = build_session_.diagnostics()[diagnostic_index_];
        openFile(diagnostic.path);
        if (document_ && document_->path() == diagnostic.path) editor_.reveal({diagnostic.line, diagnostic.column});
        appendOutput("Build diagnostic: " + diagnostic.message + "\n");
      } else {
        const auto& diagnostic = diagnostics[diagnostic_index_ - build_session_.diagnostics().size()];
        if (!document_ || diagnostic.path != document_->path()) openFile(diagnostic.path);
        if (document_ && document_->path() == diagnostic.path) {
          auto position = diagnostic.position;
          position.column = document_->byteColumn(position.line, position.column);
          editor_.reveal(position);
        }
        appendOutput("Diagnostic: " + diagnostic.message + "\n");
      }
      ++diagnostic_index_;
      return true;
    }
    case finalcut::FKey::F9:
      if (!document_) appendOutput("Breakpoint unavailable: no document is open\n");
      else if (document_->path().empty()) appendOutput("Breakpoint unavailable: save the document first\n");
      else if (!isCppSource(document_->path())) appendOutput("Breakpoint unavailable: the active document is not C/C++ source\n");
      else {
        const bool enabled = gdb_.toggleBreakpoint(document_->path(), document_->cursor().line + 1);
        debug_state_dirty_ = true; saveDebugState();
        appendOutput(std::string(enabled ? "Breakpoint set: " : "Breakpoint removed: ")
          + document_->path().string() + ":" + std::to_string(document_->cursor().line + 1) + "\n");
        debug_ui_.invalidateBreakpoints(); refreshBreakpointsPanel(); editor_.redraw();
      }
      return true;
    case finalcut::FKey::F17:
      if (!gdb_.running()) appendOutput("Pause unavailable: debugger is not started\n");
      else if (!gdb_.active()) appendOutput("Pause unavailable: the program has exited\n");
      else if (gdb_.stopped()) appendOutput("Pause unavailable: debuggee is already stopped\n");
      else gdb_.interrupt();
      return true;
    case finalcut::FKey::F34: if (requireStoppedDebugger("Next")) gdb_.next(); return true;
    case finalcut::FKey::F11: if (requireStoppedDebugger("Step into")) gdb_.step(); return true;
    case finalcut::FKey::F12: if (requireStoppedDebugger("Step out")) gdb_.finish(); return true;
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
    appendOutput("Completion discarded: the source document changed while clangd was responding\n");
  if (lsp_events.discarded_code_actions != 0)
    appendOutput("Code Action discarded: the source document changed while clangd was responding\n");
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
    if (labels.empty()) appendOutput("Completion: clangd returned no usable suggestions\n");
    else {
      const auto selection = choose("clangd completion", labels);
      if (selection > 0 && selection <= unique.size() && document_) {
        const auto& completion = unique[selection - 1];
        if (completion.edit && completion.edit->start.line < document_->lines().size()
            && completion.edit->end.line < document_->lines().size()) {
          auto start = completion.edit->start; auto end = completion.edit->end;
          start.column = document_->byteColumn(start.line, start.column);
          end.column = document_->byteColumn(end.line, end.column);
          document_->replaceRange(start, end, completion.edit->text);
        } else document_->replaceIdentifierBeforeCursor(completion.insertion);
        lsp_.change(*document_); editor_.invalidateSyntax(); updateStatus();
        if (!completion.documentation.empty())
          appendOutput("Completion documentation:\n" + completion.documentation + "\n");
      }
    }
  }
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
      appendOutput("Code Action: " + action.title + "\n");
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
  for (const auto& feedback : lsp_events.feedback) {
    appendOutput(std::string(lspOperationLabel(feedback.operation))
      + (feedback.error ? " error: " : ": ") + feedback.message + "\n");
    if (feedback.operation == LspOperation::Server && feedback.error)
      showNotification(feedback.message, NotificationKind::Error, std::chrono::milliseconds{6000});
  }

  const auto build_poll = build_session_.poll(root_);
  for (const auto& chunk : build_poll.output) appendBuildOutput(chunk);
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
        appendBuildOutput(stage_summary.str());
        appendOutput(stage_summary.str());
        (void)startBuildStage(false);
      } else {
        finishBuildOperation(0, {});
      }
    }
  }
  for (auto& chunk : run_process_.drain()) appendOutput(chunk);
  for (auto& chunk : terminal_.drain()) console_.append(chunk);
  const auto run_exit = run_process_.exitCode().has_value() ? run_process_.exitCode() : terminal_.exitCode();
  if (run_active_ && run_exit.has_value()) {
    const auto code = *run_exit;
    run_active_ = false;
    run_process_.stop(); terminal_.stop();
    console_.setControlEnabled(false);
    lower_tabs_.setCurrentIndex(0);
    appendOutput("Run finished with exit code " + std::to_string(code) + "\n");
    showNotification("Program finished (exit " + std::to_string(code) + ")",
      code == 0 ? NotificationKind::Success : NotificationKind::Error);
    editor_.setFocus(); finalcut::FWidget::setFocusWidget(&editor_);
  }
  gdb_.poll();
  const bool debug_active = gdb_.active();
  if (debug_ui_.observeActive(debug_active, gdb_.exited()))
    showNotification("Debuggee finished", NotificationKind::Success);
  for (auto& line : gdb_.takeOutput()) appendOutput(line + "\n");
  for (auto& result : gdb_.takeResults()) {
    const auto operation = result.kind == DebugResultKind::Evaluation ? "Evaluation"
      : result.kind == DebugResultKind::Assignment ? "Assignment"
      : result.kind == DebugResultKind::Disassembly ? "Disassembly" : "Memory";
    if (!result.error.empty()) {
      appendOutput(std::string(operation) + " error for " + result.expression + ": " + result.error + "\n");
      showTextDialog(std::string(operation) + " error", result.expression + "\n\n" + result.error);
    } else {
      appendOutput(std::string(operation) + ": " + result.expression + " = " + result.value + "\n");
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
  PromptDialog dialog(title, label, this);
  const auto result = dialog.exec() == finalcut::FDialog::ResultCode::Accept ? dialog.value() : std::string{};
  timer_id_ = addTimer(100);
  return result;
}

auto IdeWindow::choose(std::string title, const std::vector<std::string>& items) -> std::size_t {
  if (items.empty()) return 0;
  delTimer(timer_id_);
  SelectionDialog dialog(title, items, this);
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
    appendOutput("Workspace edit error: " + message + "; no pending text changes were applied\n");
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
  ConfirmTextDialog dialog(title + " preview", message.str(), this);
  const auto accepted = dialog.exec() == finalcut::FDialog::ResultCode::Accept;
  timer_id_ = addTimer(100);
  if (!accepted) {
    if (failure_reason) *failure_reason = "User cancelled the workspace edit.";
    appendOutput(title + " cancelled; no files changed\n"); return false;
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
    appendOutput("Workspace edit warning: cannot remove a transaction backup: " + commit_error + "\n");
  appendOutput(title + " applied " + std::to_string(changed_ranges) + " text change(s) in "
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
