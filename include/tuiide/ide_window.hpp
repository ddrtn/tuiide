#pragma once

#include "tuiide/code_editor.hpp"
#include "tuiide/build_diagnostic.hpp"
#include "tuiide/build_progress.hpp"
#include "tuiide/cmake_model.hpp"
#include "tuiide/cmake_presets.hpp"
#include "tuiide/cmake_source_edit.hpp"
#include "tuiide/clang_format.hpp"
#include "tuiide/compilation_database.hpp"
#include "tuiide/command_state.hpp"
#include "tuiide/console_widget.hpp"
#include "tuiide/debug_session.hpp"
#include "tuiide/gdb_client.hpp"
#include "tuiide/lsp_client.hpp"
#include "tuiide/process.hpp"
#include "tuiide/pseudo_terminal.hpp"
#include "tuiide/project_template.hpp"
#include "tuiide/project_creation.hpp"
#include "tuiide/project_history.hpp"
#include "tuiide/project_import.hpp"
#include "tuiide/project_settings.hpp"
#include "tuiide/project_tree.hpp"
#include "tuiide/recovery.hpp"
#include "tuiide/sidebar_tabs.hpp"
#include "tuiide/text_search.hpp"
#include "tuiide/workspace_edit.hpp"

#include <final/final.h>

#include <filesystem>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tuiide {

class CommandListBox final : public finalcut::FListBox {
 public:
  explicit CommandListBox(finalcut::FWidget* parent = nullptr) : FListBox(parent) {}
  void setCommandHandler(std::function<bool(finalcut::FKey)> handler) { command_handler_ = std::move(handler); }
  void setContextHandler(std::function<void(finalcut::FPoint)> handler) { context_handler_ = std::move(handler); }
 protected:
  void onKeyPress(finalcut::FKeyEvent* event) override {
    if (context_handler_ && (event->key() == finalcut::FKey::Menu || event->key() == finalcut::FKey::F22)) {
      context_handler_({getTermX() + 2, getTermY() + 2}); event->accept(); return;
    }
    if (command_handler_ && command_handler_(event->key())) { event->accept(); return; }
    FListBox::onKeyPress(event);
  }
  void onMouseDown(finalcut::FMouseEvent* event) override {
    if (context_handler_ && event->getButton() == finalcut::MouseButton::Right) {
      finalcut::FMouseEvent select(finalcut::Event::MouseDown, event->getPos(), event->getTermPos(),
        finalcut::MouseButton::Left);
      FListBox::onMouseDown(&select);
      context_handler_(event->getTermPos()); return;
    }
    FListBox::onMouseDown(event);
  }
 private:
  std::function<bool(finalcut::FKey)> command_handler_;
  std::function<void(finalcut::FPoint)> context_handler_;
};

class CommandListView final : public finalcut::FListView {
 public:
  explicit CommandListView(finalcut::FWidget* parent = nullptr) : FListView(parent) {
    addColumn("Project");
    setTreeView();
  }
  void setCommandHandler(std::function<bool(finalcut::FKey)> handler) { command_handler_ = std::move(handler); }
  void setContextHandler(std::function<void(finalcut::FPoint)> handler) { context_handler_ = std::move(handler); }
 protected:
  void onKeyPress(finalcut::FKeyEvent* event) override {
    if (context_handler_ && (event->key() == finalcut::FKey::Menu || event->key() == finalcut::FKey::F22)) {
      context_handler_({getTermX() + 2, getTermY() + 2}); event->accept(); return;
    }
    if (command_handler_ && command_handler_(event->key())) { event->accept(); return; }
    FListView::onKeyPress(event);
  }
  void onMouseDown(finalcut::FMouseEvent* event) override {
    if (context_handler_ && event->getButton() == finalcut::MouseButton::Right) {
      finalcut::FMouseEvent select(finalcut::Event::MouseDown, event->getPos(), event->getTermPos(),
        finalcut::MouseButton::Left);
      FListView::onMouseDown(&select);
      context_handler_(event->getTermPos()); return;
    }
    FListView::onMouseDown(event);
  }
 private:
  std::function<bool(finalcut::FKey)> command_handler_;
  std::function<void(finalcut::FPoint)> context_handler_;
};

class IdeWindow final : public finalcut::FDialog {
 public:
  explicit IdeWindow(std::filesystem::path root, finalcut::FWidget* parent = nullptr);
  ~IdeWindow() override;

 protected:
  void adjustSize() override;
  void onTimer(finalcut::FTimerEvent* event) override;
  void onKeyPress(finalcut::FKeyEvent* event) override;
  void onClose(finalcut::FCloseEvent* event) override;

 private:
  void layout();
  void resizeSidebar(int delta);
  void resizeLowerPanel(int delta);
  void resetPanelSizes();
  void setupMenus();
  void applyShortcutAccelerators();
  void queueMenuCommand(finalcut::FKey key);
  void showAbout();
  void showKeyboardHelp();
  void showCommandPalette();
  void configureShortcut();
  void showShortcutConflicts();
  void selectTheme();
  void configureEditorColor();
  void showTextDialog(std::string title, std::string text);
  void showOpenFilesContextMenu(finalcut::FPoint position);
  void showProjectContextMenu(finalcut::FPoint position);
  void showDebugContextMenu(finalcut::FPoint position, bool breakpoints);
  void showContextMenu(finalcut::FMenu& menu, finalcut::FPoint position);
  void refreshFiles();
  void refreshTabs();
  void refreshDebugPanel();
  void refreshBreakpointsPanel();
  void openSelectedBreakpoint();
  void editSelectedBreakpoint();
  void toggleSelectedBreakpoint();
  void removeSelectedBreakpoint();
  void clearBreakpoints();
  void refreshOutline();
  void openSelectedOutlineSymbol();
  void openSelectedFrame();
  void addWatch();
  void evaluateExpression();
  void editVariableValue();
  void showDisassembly();
  void showMemory();
  void removeSelectedWatch();
  void loadDebugState();
  void saveDebugState();
  void activateDocument(std::size_t index);
  void newFile();
  void newProject();
  void openProject();
  void openRecentProject();
  void importProject(const std::filesystem::path& directory);
  void closeProject();
  void projectSettings();
  auto closeAllDocuments() -> bool;
  auto loadProject(std::filesystem::path root, std::filesystem::path build_directory = {}) -> bool;
  void unloadProject();
  void createProjectFile();
  void showCMakeCompletion();
  void requestSignatureHelp();
  void requestCodeActions(bool organize_includes);
  void switchSourceHeader();
  void requestWorkspaceSymbols();
  void requestHierarchy(bool type_hierarchy);
  void navigateTo(const LspNavigationItem& item);
  auto chooseProjectSavePath(std::string title, const std::filesystem::path& start,
                             std::string suggested_name) -> std::optional<std::filesystem::path>;
  void exitIde();
  void closeActiveDocument(bool remember = true);
  void closeOtherDocuments();
  void reopenClosedDocument();
  void switchDocument(int direction);
  void openSelected();
  void removeSelectedProjectFile();
  void filterProjectTree();
  void createProjectDirectory();
  void deleteSelectedProjectDirectory();
  void renameSelectedProjectEntry();
  void openFile(const std::filesystem::path& path);
  auto save() -> bool;
  auto saveAllDocuments() -> bool;
  auto saveAs() -> bool;
  void find();
  void findNext(bool previous);
  void replaceCurrent();
  void replaceAll(bool project);
  void showProjectSearch();
  void goToLine();
  void showProblems();
  void refreshProblemsPanel();
  void openSelectedProblem();
  void clearLowerPanel();
  void copyLowerPanel();
  void filterProblems();
  void formatDocument(bool selection_only);
  void checkExternalChanges();
  void autosaveRecovery();
  void restoreRecovery();
  void build();
  void configure();
  void rebuild();
  void clean();
  void cancelBuild();
  void launchSettings();
  auto beginBuildOperation(bool save_documents) -> bool;
  auto startConfigureStage() -> bool;
  auto startBuildStage(bool clean_stage) -> bool;
  void finishBuildOperation(int exit_code, std::string_view failed_stage);
  auto refreshCMakePresets(bool report_error = true) -> bool;
  void selectCMakePreset();
  void selectCMakeBuildPreset();
  void refreshCMakeTargets();
  void selectCMakeTarget();
  void processBuildOutput(std::string_view chunk);
  void run();
  void stopRun();
  void startRun();
  void debugRun();
  void startDebug();
  void debugStop();
  void debugRestart();
  auto applyWorkspaceEdit(WorkspaceEdit edit, std::string title = "Workspace edit",
    std::string* failure_reason = nullptr) -> bool;
  void appendOutput(std::string_view text);
  void appendBuildOutput(std::string_view text);
  enum class NotificationKind { Information, Success, Warning, Error };
  void showNotification(std::string message, NotificationKind kind = NotificationKind::Information,
      std::chrono::milliseconds duration = std::chrono::milliseconds{4000});
  void updateNotification();
  void refreshCompilationDatabase(bool report);
  void restartLanguageServer();
  void updateMenuState();
  void updateStatus();
  auto handleCommand(finalcut::FKey key) -> bool;
  auto prompt(std::string title, std::string label) -> std::string;
  auto choose(std::string title, const std::vector<std::string>& items) -> std::size_t;
  auto launchCommand(LaunchCommand& command, std::string& error) -> bool;

  std::filesystem::path root_;
  std::filesystem::path build_dir_;
  std::filesystem::path session_file_;
  std::filesystem::path recovery_file_;
  std::filesystem::path project_history_file_;
  ProjectSettings project_settings_;
  std::vector<std::filesystem::path> recent_projects_;
  std::vector<std::filesystem::path> file_paths_;
  std::string project_filter_;
  std::unordered_map<const finalcut::FListViewItem*, std::filesystem::path> project_item_paths_;
  std::vector<std::optional<SourceLocation>> debug_locations_;
  std::vector<std::optional<std::string>> debug_thread_ids_;
  std::vector<std::optional<std::size_t>> debug_watch_indices_;
  std::vector<std::optional<int>> debug_frame_levels_;
  std::vector<std::optional<std::size_t>> debug_variable_indices_;
  std::vector<DebugBreakpoint> breakpoint_rows_;
  std::vector<Position> outline_positions_;
  std::filesystem::path outline_path_;
  int outline_version_{-1};
  std::filesystem::path outline_requested_path_;
  int outline_requested_version_{-1};
  std::filesystem::path outline_observed_path_;
  int outline_observed_version_{-1};
  unsigned outline_stable_tick_{};
  std::string debug_signature_;
  std::string breakpoint_signature_;
  std::vector<std::unique_ptr<Document>> documents_;
  struct ClosedDocument { std::filesystem::path path; Position cursor; };
  std::vector<ClosedDocument> closed_documents_;
  Document* document_{};
  std::size_t active_document_{};
  LspClient lsp_;
  CompilationDatabase compilation_database_;
  std::unordered_set<std::filesystem::path> compilation_database_warnings_;
  GdbClient gdb_;
  AsyncProcess build_process_;
  AsyncProcess run_process_;
  PseudoTerminal terminal_;
  enum class BuildOperation { None, Configure, Build, Rebuild, Clean };
  enum class BuildStage { Idle, Configure, Clean, Build };
  BuildOperation build_operation_{BuildOperation::None};
  BuildStage build_stage_{BuildStage::Idle};
  std::chrono::steady_clock::time_point build_started_{};
  std::chrono::steady_clock::time_point build_stage_started_{};
  std::optional<unsigned> build_progress_;
  std::string build_partial_;
  std::vector<BuildDiagnostic> build_diagnostics_;
  std::vector<CMakeTarget> cmake_targets_;
  std::vector<CMakeConfigurePreset> cmake_presets_;
  std::vector<CMakeBuildPreset> cmake_build_presets_;
  std::optional<std::size_t> selected_cmake_target_;
  std::string selected_cmake_preset_;
  std::string selected_cmake_build_preset_;
  std::string preferred_cmake_target_;
  std::string preferred_cmake_configuration_;
  std::size_t diagnostic_index_{};
  std::string output_text_;
  std::string build_output_text_;
  std::string problems_filter_;
  std::string problems_signature_;
  std::string problems_text_;
  struct ProblemRow { std::filesystem::path path; Position position; bool utf16{}; std::string message; };
  std::vector<ProblemRow> problem_rows_;
  int timer_id_{};
  bool debug_state_dirty_{};
  bool run_active_{};
  bool lsp_ready_observed_{};
  bool debug_active_observed_{};
  std::chrono::steady_clock::time_point notification_deadline_{};
  finalcut::FWidget* context_focus_{};
  finalcut::FMenu* active_context_menu_{};
  std::size_t sidebar_width_{};
  std::size_t lower_panel_height_{};
  enum class PendingLaunch { None, Run, Debug };
  PendingLaunch pending_launch_{PendingLaunch::None};
  std::function<void()> deferred_command_;
  std::uint64_t diagnostics_revision_{};
  std::uint64_t semantic_tokens_revision_{};
  bool ui_ready_{};
  std::optional<CommandAvailability> menu_state_;
  std::string search_query_;
  std::string search_replacement_;
  SearchOptions search_options_;
  bool search_project_{};
  unsigned maintenance_ticks_{};

  struct FileMenu {
    explicit FileMenu(finalcut::FMenuBar& bar) : menu{"&File", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem new_project{"New &Project...", &menu};
    finalcut::FMenuItem open_project{"&Open Project...", &menu};
    finalcut::FMenuItem recent_projects{"Recent P&rojects...", &menu};
    finalcut::FMenuItem close_project{"Close Pro&ject", &menu};
    finalcut::FMenuItem project_separator{&menu};
    finalcut::FMenuItem new_file{finalcut::FKey::Ctrl_n, "&New", &menu};
    finalcut::FMenuItem new_project_file{"New from &template...", &menu};
    finalcut::FMenuItem open{finalcut::FKey::Ctrl_o, "&Open...", &menu};
    finalcut::FMenuItem save{finalcut::FKey::Ctrl_s, "&Save", &menu};
    finalcut::FMenuItem save_all{"Save A&ll", &menu};
    finalcut::FMenuItem save_as{"Save &As...", &menu};
    finalcut::FMenuItem close{finalcut::FKey::Ctrl_w, "&Close", &menu};
    finalcut::FMenuItem close_others{"Close Ot&hers", &menu};
    finalcut::FMenuItem close_all{finalcut::FKey::Meta_W, "Clos&e All", &menu};
    finalcut::FMenuItem reopen_closed{finalcut::FKey::Meta_u, "Reopen Close&d", &menu};
    finalcut::FMenuItem separator{&menu};
    finalcut::FMenuItem quit{finalcut::FKey::Ctrl_q, "E&xit", &menu};
  };
  struct EditMenu {
    explicit EditMenu(finalcut::FMenuBar& bar) : menu{"&Edit", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem undo{finalcut::FKey::Ctrl_z, "&Undo", &menu};
    finalcut::FMenuItem redo{finalcut::FKey::Ctrl_y, "&Redo", &menu};
    finalcut::FMenuItem separator1{&menu};
    finalcut::FMenuItem cut{finalcut::FKey::Ctrl_x, "Cu&t", &menu};
    finalcut::FMenuItem copy{finalcut::FKey::Ctrl_c, "&Copy", &menu};
    finalcut::FMenuItem paste{finalcut::FKey::Ctrl_v, "&Paste", &menu};
    finalcut::FMenuItem separator2{&menu};
    finalcut::FMenuItem select_all{finalcut::FKey::Ctrl_a, "Select &All", &menu};
    finalcut::FMenuItem separator3{&menu};
    finalcut::FMenuItem toggle_comment{"Toggle co&mment", &menu};
    finalcut::FMenuItem duplicate_line{"D&uplicate line", &menu};
    finalcut::FMenuItem move_line_up{"Move line u&p", &menu};
    finalcut::FMenuItem move_line_down{"Move line do&wn", &menu};
    finalcut::FMenuItem delete_line{"De&lete line", &menu};
  };
  struct SearchMenu {
    explicit SearchMenu(finalcut::FMenuBar& bar) : menu{"&Search", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem find{finalcut::FKey::Ctrl_f, "&Find...", &menu};
    finalcut::FMenuItem find_next{"Find &next", &menu};
    finalcut::FMenuItem find_previous{"Find &previous", &menu};
    finalcut::FMenuItem replace{"&Replace...", &menu};
    finalcut::FMenuItem project_search{"Find in pro&ject...", &menu};
    finalcut::FMenuItem go_to_line{finalcut::FKey::Ctrl_g, "&Go to line...", &menu};
    finalcut::FMenuItem separator{&menu};
    finalcut::FMenuItem definition{finalcut::FKey::F3, "Go to &definition", &menu};
    finalcut::FMenuItem references{finalcut::FKey::F4, "Find &references", &menu};
    finalcut::FMenuItem problems{finalcut::FKey::Meta_e, "&Problems...", &menu};
  };
  struct RunMenu {
    explicit RunMenu(finalcut::FMenuBar& bar) : menu{"&Run", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem configure{"&Configure", &menu};
    finalcut::FMenuItem build{finalcut::FKey::F7, "&Build", &menu};
    finalcut::FMenuItem rebuild{"&Rebuild", &menu};
    finalcut::FMenuItem clean{"C&lean", &menu};
    finalcut::FMenuItem cancel_build{"Cancel B&uild", &menu};
    finalcut::FMenuItem separator1{&menu};
    finalcut::FMenuItem run{finalcut::FKey::F6, "&Run", &menu};
    finalcut::FMenuItem stop_run{"S&top program", &menu};
    finalcut::FMenuItem launch_settings{"Launch &configuration...", &menu};
    finalcut::FMenuItem separator2{&menu};
    finalcut::FMenuItem configure_preset{finalcut::FKey::Meta_p, "Configure &preset...", &menu};
    finalcut::FMenuItem build_preset{finalcut::FKey::Meta_b, "&Build preset...", &menu};
    finalcut::FMenuItem target{finalcut::FKey::Meta_t, "Select &target...", &menu};
  };
  struct ProjectMenu {
    explicit ProjectMenu(finalcut::FMenuBar& bar) : menu{"&Project", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem refresh{"&Refresh tree", &menu};
    finalcut::FMenuItem filter{"&Filter...", &menu};
    finalcut::FMenuItem clear_filter{"&Clear filter", &menu};
    finalcut::FMenuItem separator1{&menu};
    finalcut::FMenuItem new_directory{"New &directory...", &menu};
    finalcut::FMenuItem rename_move{"Rename / &Move...", &menu};
    finalcut::FMenuItem delete_directory{"Delete director&y", &menu};
  };
  struct DebugMenu {
    explicit DebugMenu(finalcut::FMenuBar& bar) : menu{"&Debug", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem start{finalcut::FKey::F5, "&Start / Continue", &menu};
    finalcut::FMenuItem pause{finalcut::FKey::F17, "&Pause", &menu};
    finalcut::FMenuItem stop{"S&top", &menu};
    finalcut::FMenuItem restart{"&Restart", &menu};
    finalcut::FMenuItem separator1{&menu};
    finalcut::FMenuItem breakpoint{finalcut::FKey::F9, "Toggle &breakpoint", &menu};
    finalcut::FMenuItem breakpoint_properties{"Breakpoint &properties...", &menu};
    finalcut::FMenuItem breakpoint_enable{"Enable / &disable breakpoint", &menu};
    finalcut::FMenuItem breakpoint_remove{"&Remove breakpoint", &menu};
    finalcut::FMenuItem breakpoint_clear{"Remove &all breakpoints", &menu};
    finalcut::FMenuItem next{finalcut::FKey::F34, "&Next", &menu};
    finalcut::FMenuItem step{finalcut::FKey::F11, "Step &into", &menu};
    finalcut::FMenuItem finish{finalcut::FKey::F12, "Step &out", &menu};
    finalcut::FMenuItem separator2{&menu};
    finalcut::FMenuItem watch{finalcut::FKey::Meta_w, "Add &watch...", &menu};
    finalcut::FMenuItem evaluate{"&Evaluate expression...", &menu};
    finalcut::FMenuItem set_variable{"Set &variable value...", &menu};
    finalcut::FMenuItem disassembly{"&Disassembly...", &menu};
    finalcut::FMenuItem memory{"&Memory...", &menu};
    finalcut::FMenuItem registers{finalcut::FKey::Meta_r, "Toggle &registers", &menu};
  };
  struct ToolsMenu {
    explicit ToolsMenu(finalcut::FMenuBar& bar) : menu{"&Tools", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem completion{finalcut::FKey::Ctrl_space, "&Completion", &menu};
    finalcut::FMenuItem signature{"Signature &help...", &menu};
    finalcut::FMenuItem hover{finalcut::FKey::F1, "Symbol &information", &menu};
    finalcut::FMenuItem rename{finalcut::FKey::F2, "&Rename symbol...", &menu};
    finalcut::FMenuItem separator{&menu};
    finalcut::FMenuItem code_actions{finalcut::FKey::Meta_a, "Code &actions...", &menu};
    finalcut::FMenuItem organize_includes{"Organize &includes", &menu};
    finalcut::FMenuItem switch_source_header{"Switch header / &source", &menu};
    finalcut::FMenuItem workspace_symbols{"&Workspace symbols...", &menu};
    finalcut::FMenuItem call_hierarchy{"Call &hierarchy...", &menu};
    finalcut::FMenuItem type_hierarchy{"Type hierarch&y...", &menu};
    finalcut::FMenuItem separator_lsp{&menu};
    finalcut::FMenuItem format_document{"Format &document", &menu};
    finalcut::FMenuItem format_selection{"Format &selection", &menu};
    finalcut::FMenuItem separator2{&menu};
    finalcut::FMenuItem command_palette{finalcut::FKey::Meta_k, "Command &palette...", &menu};
    finalcut::FMenuItem configure_shortcut{"Configure &shortcut...", &menu};
    finalcut::FMenuItem shortcut_conflicts{"Shortcut &conflicts...", &menu};
    finalcut::FMenuItem separator3{&menu};
    finalcut::FMenuItem theme{"Editor &theme...", &menu};
    finalcut::FMenuItem colors{"Editor &colors...", &menu};
    finalcut::FMenuItem separator4{&menu};
    finalcut::FMenuItem project_settings{"Project &Settings...", &menu};
  };
  struct WindowMenu {
    explicit WindowMenu(finalcut::FMenuBar& bar) : menu{"&Window", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem previous{finalcut::FKey::Ctrl_page_up, "&Previous file", &menu};
    finalcut::FMenuItem next{finalcut::FKey::Ctrl_page_down, "&Next file", &menu};
    finalcut::FMenuItem separator{&menu};
    finalcut::FCheckMenuItem open_files{"Show Open &files", &menu};
    finalcut::FCheckMenuItem project{"Show &Project", &menu};
    finalcut::FCheckMenuItem outline{"Show &Outline", &menu};
    finalcut::FCheckMenuItem debug{"Show &Debug", &menu};
    finalcut::FCheckMenuItem breakpoints{"Show &Breakpoints", &menu};
    finalcut::FMenuItem separator2{&menu};
    finalcut::FMenuItem clear_lower{"&Clear active lower panel", &menu};
    finalcut::FMenuItem copy_lower{"&Copy active lower panel", &menu};
    finalcut::FMenuItem filter_problems{"Filter &Problems...", &menu};
    finalcut::FMenuItem separator3{&menu};
    finalcut::FMenuItem sidebar_narrower{"Sidebar &narrower", &menu};
    finalcut::FMenuItem sidebar_wider{"Sidebar &wider", &menu};
    finalcut::FMenuItem lower_shorter{"Lower panel &shorter", &menu};
    finalcut::FMenuItem lower_taller{"Lower panel &taller", &menu};
    finalcut::FMenuItem reset_panels{"&Reset panel sizes", &menu};
  };
  struct HelpMenu {
    explicit HelpMenu(finalcut::FMenuBar& bar) : menu{"&Help", &bar} {}
    finalcut::FMenu menu;
    finalcut::FMenuItem keyboard{"&Keyboard shortcuts", &menu};
    finalcut::FMenuItem separator{&menu};
    finalcut::FMenuItem about{"&About TUI IDE", &menu};
  };

  finalcut::FMenuBar menu_bar_{this};
  FileMenu file_menu_{menu_bar_};
  EditMenu edit_menu_{menu_bar_};
  SearchMenu search_menu_{menu_bar_};
  ProjectMenu project_menu_{menu_bar_};
  RunMenu run_menu_{menu_bar_};
  DebugMenu debug_menu_{menu_bar_};
  ToolsMenu tools_menu_{menu_bar_};
  WindowMenu window_menu_{menu_bar_};
  HelpMenu help_menu_{menu_bar_};

  struct OpenFilesContextMenu {
    explicit OpenFilesContextMenu(finalcut::FWidget* parent) : menu(finalcut::FString{"-"}, parent) {
      menu.getItem()->hide();
    }
    finalcut::FMenu menu;
    finalcut::FMenuItem activate{finalcut::FKey::Return, "&Activate", &menu};
    finalcut::FMenuItem save{finalcut::FKey::Ctrl_s, "&Save", &menu};
    finalcut::FMenuItem close{finalcut::FKey::Ctrl_w, "&Close", &menu};
    finalcut::FMenuItem close_others{"Close &others", &menu};
    finalcut::FMenuItem close_all{finalcut::FKey::Meta_W, "Close &all", &menu};
    finalcut::FMenuItem reopen{finalcut::FKey::Meta_u, "&Reopen closed", &menu};
  };
  std::unique_ptr<OpenFilesContextMenu> open_files_context_;

  struct ProjectContextMenu {
    explicit ProjectContextMenu(finalcut::FWidget* parent) : menu(finalcut::FString{"-"}, parent) {
      menu.getItem()->hide();
    }
    finalcut::FMenu menu;
    finalcut::FMenuItem open{finalcut::FKey::Return, "&Open", &menu};
    finalcut::FMenuItem new_file{finalcut::FKey::Insert, "New from &template...", &menu};
    finalcut::FMenuItem new_directory{finalcut::FKey::Meta_n, "New &directory...", &menu};
    finalcut::FMenuItem rename{finalcut::FKey::F2, "&Rename / move...", &menu};
    finalcut::FMenuItem remove{finalcut::FKey::Del_char, "&Remove / delete...", &menu};
    finalcut::FMenuItem refresh{finalcut::FKey::F5, "Re&fresh", &menu};
  };
  std::unique_ptr<ProjectContextMenu> project_context_;

  struct DebugContextMenu {
    explicit DebugContextMenu(finalcut::FWidget* parent)
        : debug_menu(finalcut::FString{"-"}, parent), breakpoint_menu(finalcut::FString{"-"}, parent) {
      debug_menu.getItem()->hide(); breakpoint_menu.getItem()->hide();
    }
    finalcut::FMenu debug_menu;
    finalcut::FMenuItem open{finalcut::FKey::Return, "&Open selected", &debug_menu};
    finalcut::FMenuItem add_watch{finalcut::FKey::Insert, "Add &watch...", &debug_menu};
    finalcut::FMenuItem remove_watch{finalcut::FKey::Del_char, "&Remove watch", &debug_menu};
    finalcut::FMenuItem registers{finalcut::FKey::Meta_r, "Toggle &registers", &debug_menu};
    finalcut::FMenu breakpoint_menu;
    finalcut::FMenuItem open_breakpoint{finalcut::FKey::Return, "&Open source", &breakpoint_menu};
    finalcut::FMenuItem properties{finalcut::FKey::F2, "Breakpoint &properties...", &breakpoint_menu};
    finalcut::FMenuItem toggle{finalcut::FKey::Space, "&Enable / disable", &breakpoint_menu};
    finalcut::FMenuItem remove_breakpoint{finalcut::FKey::Del_char, "&Remove breakpoint", &breakpoint_menu};
    finalcut::FMenuItem clear_breakpoints{finalcut::FKey::Meta_D, "Remove &all breakpoints", &breakpoint_menu};
  };
  std::unique_ptr<DebugContextMenu> debug_context_;

  SidebarTabs sidebar_tabs_{this};
  CommandListBox tabs_{&sidebar_tabs_};
  CommandListView files_{&sidebar_tabs_};
  CommandListBox outline_{&sidebar_tabs_};
  CommandListBox debug_{&sidebar_tabs_};
  CommandListBox breakpoints_{&sidebar_tabs_};
  CodeEditor editor_{this};
  SidebarTabs lower_tabs_{this};
  finalcut::FTextView output_{&lower_tabs_};
  CommandListBox problems_{&lower_tabs_};
  finalcut::FTextView build_output_{&lower_tabs_};
  ConsoleWidget console_{&lower_tabs_};
  SystemClipboard lower_clipboard_;
  finalcut::FLabel status_{this};
  finalcut::FLabel notification_{this};
};

}  // namespace tuiide
