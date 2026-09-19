#include "tuiide/document.hpp"
#include "tuiide/analysis_session.hpp"
#include "tuiide/document_labels.hpp"
#include "tuiide/document_session.hpp"
#include "tuiide/event_log.hpp"
#include "tuiide/build_command.hpp"
#include "tuiide/build_diagnostic.hpp"
#include "tuiide/build_output_collector.hpp"
#include "tuiide/build_progress.hpp"
#include "tuiide/build_session.hpp"
#include "tuiide/build_workflow.hpp"
#include "tuiide/cmake_model.hpp"
#include "tuiide/cmake_presets.hpp"
#include "tuiide/cmake_session.hpp"
#include "tuiide/cmake_source_edit.hpp"
#include "tuiide/ctest_session.hpp"
#include "tuiide/clipboard.hpp"
#include "tuiide/cli.hpp"
#include "tuiide/clang_format.hpp"
#include "tuiide/compilation_database.hpp"
#include "tuiide/command_state.hpp"
#include "tuiide/debug_session.hpp"
#include "tuiide/debug_ui_controller.hpp"
#include "tuiide/gdb_client.hpp"
#include "tuiide/gdb_mi.hpp"
#include "tuiide/json_utils.hpp"
#include "tuiide/lsp_client.hpp"
#include "tuiide/lsp_ui_controller.hpp"
#include "tuiide/launch_configuration.hpp"
#include "tuiide/process.hpp"
#include "tuiide/pseudo_terminal.hpp"
#include "tuiide/project_template.hpp"
#include "tuiide/project_creation.hpp"
#include "tuiide/project_history.hpp"
#include "tuiide/project_import.hpp"
#include "tuiide/project_settings.hpp"
#include "tuiide/project_session.hpp"
#include "tuiide/project_tree.hpp"
#include "tuiide/recovery.hpp"
#include "tuiide/run_session.hpp"
#include "tuiide/syntax.hpp"
#include "tuiide/tab_bar_layout.hpp"
#include "tuiide/text_display.hpp"
#include "tuiide/text_search.hpp"
#include "tuiide/terminal_buffer.hpp"
#include "tuiide/tool_discovery.hpp"
#include "tuiide/toolchain_kit.hpp"
#include "tuiide/user_settings.hpp"
#include "tuiide/workspace_edit.hpp"
#include "tuiide/workspace_file_transaction.hpp"

#include <chrono>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace {
void expect(bool condition, const char* message) {
  if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
  const auto cli = tuiide::parseCommandLine({"--diagnostic", "--log-file", "/tmp/tuiide.log",
    "--project=/tmp/project with spaces"});
  expect(cli.error.empty() && cli.options.diagnostic
      && cli.options.log_file == "/tmp/tuiide.log"
      && cli.options.project == "/tmp/project with spaces",
    "command line parser accepts diagnostic, log, and explicit project options");
  expect(tuiide::parseCommandLine({"first", "second"}).error.find("more than once") != std::string::npos,
    "command line parser rejects multiple project paths");
  expect(tuiide::parseCommandLine({"--unknown"}).error.find("unknown option") != std::string::npos,
    "command line parser rejects unknown options");
  expect(tuiide::commandLineHelp("tuiide").find("--project PATH") != std::string::npos,
    "command line help documents project selection");

  const auto tool_directory = std::filesystem::temp_directory_path()
    / ("tuiide-tool-discovery-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(tool_directory);
  const auto fake_tool = tool_directory / "fake-tool";
  { std::ofstream file(fake_tool); file << "#!/bin/sh\nexit 0\n"; }
  std::filesystem::permissions(fake_tool, std::filesystem::perms::owner_all);
  const auto discovered_tool = tuiide::findExecutable("fake-tool", tool_directory.string());
  expect(discovered_tool && discovered_tool->filename() == "fake-tool",
    "tool discovery finds an executable in an explicit PATH");
  expect(!tuiide::findExecutable("missing-tool", tool_directory.string()),
    "tool discovery reports an absent executable");
  const auto makeFakeTool = [&](std::string_view name, std::string_view version) {
    const auto path = tool_directory / name;
    std::ofstream file(path);
    file << "#!/bin/sh\nif [ \"$1\" = --print-sysroot ]; then echo /sdk; "
         << "else echo '" << version << "'; fi\n";
    file.close();
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  };
  makeFakeTool("gcc", "gcc 14.1"); makeFakeTool("g++", "g++ 14.1");
  makeFakeTool("gdb", "GNU gdb 15.1"); makeFakeTool("ninja", "1.12.0");
  std::filesystem::create_directories(tool_directory / "cmake");
  { std::ofstream file(tool_directory / "cmake/arm-toolchain.cmake"); file << "# fixture\n"; }
  const auto kits = tuiide::discoverToolchainKits(tool_directory.string(), tool_directory);
  expect(kits.size() == 2 && kits.front().name == "GCC native"
      && kits.front().compiler_version == "g++ 14.1"
      && kits.front().debugger_kind == "GDB" && kits.front().generator == "Ninja"
      && kits.front().sysroot == "/sdk" && kits.front().valid
      && kits.back().toolchain_file.filename() == "arm-toolchain.cmake"
      && tuiide::describeToolchainKit(kits.front()).find("g++ 14.1") != std::string::npos,
    "toolchain discovery verifies native tools and project CMake toolchain files");
  expect(tuiide::firstVersionLine("\n  clang version 19.0  \nTarget")
      == "clang version 19.0", "tool version parsing returns a trimmed first line");
  std::error_code tool_cleanup_error;
  std::filesystem::remove_all(tool_directory, tool_cleanup_error);

  tuiide::BuildWorkflow build_workflow;
  build_workflow.begin(tuiide::BuildOperation::Build, false, tuiide::BuildContinuation::Run);
  expect(build_workflow.operation() == tuiide::BuildOperation::Build
      && build_workflow.stage() == tuiide::BuildStage::Configure
      && build_workflow.continuation() == tuiide::BuildContinuation::Run
      && build_workflow.nextStageAfterSuccess() == tuiide::BuildStage::Build
      && build_workflow.nextStageAfterSuccess() == tuiide::BuildStage::Idle,
    "unconfigured build workflow runs configure and build in order");
  build_workflow.setProgress(37);
  expect(build_workflow.progress() == 37 && build_workflow.operationElapsed() >= 0.0
      && build_workflow.stageElapsed() >= 0.0,
    "build workflow owns progress and elapsed operation/stage timing");
  build_workflow.enterStage(tuiide::BuildStage::Build);
  expect(!build_workflow.progress(), "entering a new build stage clears stale progress");
  expect(build_workflow.takeContinuation() == tuiide::BuildContinuation::Run
      && build_workflow.continuation() == tuiide::BuildContinuation::None,
    "build continuation survives intermediate stages and can be consumed exactly once");
  build_workflow.begin(tuiide::BuildOperation::Rebuild, true);
  expect(build_workflow.stage() == tuiide::BuildStage::Clean
      && build_workflow.nextStageAfterSuccess() == tuiide::BuildStage::Build,
    "configured rebuild workflow runs clean before build");
  build_workflow.begin(tuiide::BuildOperation::Configure, true);
  expect(build_workflow.operationName() == "Configure"
      && build_workflow.stageName() == "Configure"
      && build_workflow.nextStageAfterSuccess() == tuiide::BuildStage::Idle,
    "configure-only workflow finishes without a build stage");
  build_workflow.begin(tuiide::BuildOperation::Clean, false);
  expect(!build_workflow.running() && build_workflow.operation() == tuiide::BuildOperation::Clean,
    "clean workflow remains idle when no configured build tree exists");
  build_workflow.reset();
  expect(build_workflow.operation() == tuiide::BuildOperation::None
      && build_workflow.stage() == tuiide::BuildStage::Idle,
    "build workflow reset clears operation and stage together");

  expect(tuiide::base64Encode("").empty(), "empty clipboard text has empty base64");
  expect(tuiide::base64Encode("f") == "Zg==" && tuiide::base64Encode("foo") == "Zm9v", "clipboard base64 padding is correct");
  expect(tuiide::base64Encode("Привет") == "0J/RgNC40LLQtdGC", "clipboard base64 preserves UTF-8 bytes");
  expect(tuiide::osc52CopySequence("foo") == "\033]52;c;Zm9v\a", "OSC 52 copy sequence is encoded");
  expect(tuiide::osc52CopySequence("foo", true).starts_with("\033Ptmux;\033\033]52;c;"), "OSC 52 supports tmux passthrough");
  ::setenv("TUIIDE_CLIPBOARD_NATIVE", "0", 1);
  ::setenv("TUIIDE_OSC52", "0", 1);
  tuiide::SystemClipboard clipboard;
  clipboard.copy("internal UTF-8: текст");
  expect(clipboard.paste() == "internal UTF-8: текст", "internal clipboard remains available as fallback");
  const auto clipboard_tools = std::filesystem::temp_directory_path()
      / ("tuiide-clipboard-tools-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(clipboard_tools);
  const auto clipboard_capture = clipboard_tools / "copied.txt";
  { std::ofstream script(clipboard_tools / "wl-copy"); script << "#!/bin/sh\ncat > \"$TUIIDE_CLIPBOARD_TEST_FILE\"\n"; }
  { std::ofstream script(clipboard_tools / "wl-paste"); script << "#!/bin/sh\nprintf 'native paste: текст'\n"; }
  std::filesystem::permissions(clipboard_tools / "wl-copy", std::filesystem::perms::owner_all);
  std::filesystem::permissions(clipboard_tools / "wl-paste", std::filesystem::perms::owner_all);
  const std::string original_path = std::getenv("PATH") ? std::getenv("PATH") : "";
  ::setenv("PATH", (clipboard_tools.string() + ":" + original_path).c_str(), 1);
  ::setenv("WAYLAND_DISPLAY", "tuiide-test", 1);
  ::setenv("TUIIDE_CLIPBOARD_TEST_FILE", clipboard_capture.c_str(), 1);
  ::unsetenv("TUIIDE_CLIPBOARD_NATIVE");
  clipboard.copy("native copy: данные");
  for (int attempt = 0; attempt < 40; ++attempt) {
    std::error_code size_error;
    if (std::filesystem::file_size(clipboard_capture, size_error) >= std::string("native copy: данные").size() && !size_error) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::ifstream captured(clipboard_capture);
  const std::string captured_text((std::istreambuf_iterator<char>(captured)), std::istreambuf_iterator<char>());
  expect(captured_text == "native copy: данные", "Wayland clipboard helper receives UTF-8 input");
  expect(clipboard.paste() == "native paste: текст", "Wayland clipboard helper provides UTF-8 paste text");
  ::setenv("PATH", original_path.c_str(), 1);
  ::unsetenv("WAYLAND_DISPLAY");
  ::unsetenv("TUIIDE_CLIPBOARD_TEST_FILE");
  std::error_code clipboard_cleanup_error;
  std::filesystem::remove_all(clipboard_tools, clipboard_cleanup_error);

  const auto diagnostic = tuiide::parseCompilerDiagnostic("src/main.cpp:12:7: error: expected ';'", "/project");
  expect(diagnostic.has_value(), "compiler diagnostic is parsed");
  expect(diagnostic->path == "/project/src/main.cpp", "relative diagnostic path is resolved");
  expect(diagnostic->line == 11 && diagnostic->column == 6, "diagnostic positions are zero-based");
  expect(diagnostic->severity == tuiide::DiagnosticSeverity::Error, "error severity is parsed");
  const auto warning = tuiide::parseCompilerDiagnostic("/tmp/a.cpp:2:1: warning: unused value", "/project");
  expect(warning && warning->severity == tuiide::DiagnosticSeverity::Warning, "warning is parsed");
  const auto line_only = tuiide::parseCompilerDiagnostic("src/a.cpp:9: fatal error: missing header", "/project");
  expect(line_only && line_only->line == 8 && line_only->column == 0, "line-only fatal diagnostic is parsed");
  const auto colored_note = tuiide::parseCompilerDiagnostic("\x1b[36msrc/a.cpp:4:2: note: declared here\x1b[0m", "/project");
  expect(colored_note && colored_note->severity == tuiide::DiagnosticSeverity::Note, "ANSI-colored note is parsed");
  const auto ubsan = tuiide::parseCompilerDiagnostic(
    "src/math.cpp:8:14: runtime error: signed integer overflow", "/project");
  expect(ubsan && ubsan->severity == tuiide::DiagnosticSeverity::Error
      && ubsan->path == "/project/src/math.cpp" && ubsan->line == 7,
    "UBSan runtime errors use compiler-compatible source navigation");
  const auto asan = tuiide::parseSanitizerDiagnostic(
    "SUMMARY: AddressSanitizer: heap-use-after-free /project/src/memory.cpp:21:9 in read", "/project");
  expect(asan && asan->path == "/project/src/memory.cpp" && asan->line == 20
      && asan->column == 8 && asan->message == "heap-use-after-free",
    "ASan summaries expose source locations to Problems");
  expect(tuiide::parseBuildProgress("[12/48] Building CXX object") == 25,
    "Ninja fractional build progress is parsed");
  expect(tuiide::parseBuildProgress("[ 73%] Linking CXX executable") == 73,
    "CMake percentage build progress is parsed");
  expect(!tuiide::parseBuildProgress("Building target without progress"),
    "ordinary build output does not invent progress");

  tuiide::BuildOutputCollector build_output;
  const auto first_build_update = build_output.append(
    "[12/48] Building CXX object\nsrc/main.cpp:12:7: error: expec", "/project");
  expect(first_build_update.progress == 25,
    "build output collector reports progress from complete lines");
  expect(first_build_update.diagnostics_added == 0 && build_output.hasPartialLine(),
    "build output collector keeps an incomplete diagnostic line");
  const auto second_build_update = build_output.append("ted ';'\n", "/project");
  expect(second_build_update.diagnostics_added == 1 && !build_output.hasPartialLine(),
    "build output collector joins chunked diagnostic lines");
  expect(build_output.diagnostics().size() == 1
      && build_output.diagnostics().front().path == "/project/src/main.cpp",
    "build output collector stores parsed diagnostics");
  const auto trailing_build_update = build_output.append(
    "src/lib.cpp:3: warning: unfinished output", "/project");
  expect(trailing_build_update.diagnostics_added == 0 && build_output.hasPartialLine(),
    "build output collector defers a final unterminated line");
  const auto finished_build_update = build_output.finish("/project");
  expect(finished_build_update.diagnostics_added == 1 && !build_output.hasPartialLine(),
    "build output collector flushes the final unterminated line");
  build_output.clearDiagnostics();
  expect(build_output.diagnostics().empty(), "build diagnostics can be cleared independently");
  (void)build_output.append("partial", "/project");
  build_output.reset();
  expect(build_output.diagnostics().empty() && !build_output.hasPartialLine(),
    "build output collector reset clears all state");

  tuiide::BuildSession build_session;
  build_session.prepare();
  build_session.begin(tuiide::BuildOperation::Build, false, tuiide::BuildContinuation::Run);
  expect(build_session.running() && build_session.stage() == tuiide::BuildStage::Configure,
    "build session starts an unconfigured build at the configure stage");
  const auto session_output = build_session.ingest(
    "[ 40%] Building CXX object\nsrc/main.cpp:2:1: warning: check this\n", "/project");
  expect(session_output.diagnostics_added == 1 && build_session.progress() == 40
      && build_session.diagnostics().size() == 1,
    "build session keeps progress and diagnostics synchronized with streamed output");
  expect(build_session.nextStageAfterSuccess() == tuiide::BuildStage::Build,
    "build session advances configure to build through its workflow");
  build_session.enterStage(tuiide::BuildStage::Build);
  expect(!build_session.start({}, {}, {}), "build session rejects an empty process command");
  (void)build_session.ingest("src/lib.cpp:3: error: final line", "/project");
  const auto build_finish = build_session.finish("/project");
  expect(build_finish.diagnostics_added == 1 && build_finish.operation == "Build"
      && build_finish.continuation == tuiide::BuildContinuation::Run
      && !build_session.running() && build_session.diagnostics().size() == 2,
    "finishing a build flushes output, returns continuation, and resets runtime state");
  build_session.prepare();
  build_session.begin(tuiide::BuildOperation::Configure, false);
  (void)build_session.ingest("src/cancel.cpp:4: warning: cancelled", "/project");
  const auto build_cancel = build_session.cancel("/project");
  expect(build_cancel.stage == "Configure" && build_cancel.diagnostics_added == 1
      && !build_session.running() && build_session.diagnostics().size() == 1,
    "cancelling a build flushes its last partial diagnostic and resets the workflow");

  tuiide::LspUiController lsp_ui;
  auto lsp_changes = lsp_ui.observe(false, 0, 0);
  expect(!lsp_changes.became_ready && !lsp_changes.diagnostics_changed
      && !lsp_changes.semantic_tokens_changed,
    "LSP UI controller starts without synthetic state changes");
  lsp_changes = lsp_ui.observe(true, 2, 3);
  expect(lsp_changes.became_ready && lsp_changes.diagnostics_changed
      && lsp_changes.semantic_tokens_changed,
    "LSP UI controller reports readiness and revision transitions once");
  lsp_changes = lsp_ui.observe(true, 2, 3);
  expect(!lsp_changes.became_ready && !lsp_changes.diagnostics_changed
      && !lsp_changes.semantic_tokens_changed,
    "LSP UI controller suppresses unchanged revisions");
  const tuiide::LspDocumentIdentity first_lsp_document{"/project/main.cpp", 4};
  const tuiide::LspDocumentIdentity second_lsp_document{"/project/main.cpp", 5};
  expect(lsp_ui.updateOutline(first_lsp_document, true, 10) == tuiide::OutlineDecision::Clear
      && lsp_ui.updateOutline(first_lsp_document, true, 14) == tuiide::OutlineDecision::None
      && lsp_ui.updateOutline(first_lsp_document, true, 15) == tuiide::OutlineDecision::Request,
    "outline reset clears stale presentation and waits for a stable document revision");
  expect(lsp_ui.acceptOutline(first_lsp_document, first_lsp_document)
      && lsp_ui.renderedOutlineMatches(first_lsp_document),
    "outline response is accepted only for the active document revision");
  expect(lsp_ui.updateOutline(second_lsp_document, true, 16) == tuiide::OutlineDecision::Clear
      && !lsp_ui.renderedOutlineMatches(first_lsp_document),
    "editing a document invalidates its rendered outline immediately");
  expect(lsp_ui.updateOutline(second_lsp_document, true, 21) == tuiide::OutlineDecision::Request
      && !lsp_ui.acceptOutline(first_lsp_document, second_lsp_document)
      && lsp_ui.acceptOutline(second_lsp_document, second_lsp_document),
    "stale outline responses cannot replace the active revision");
  expect(lsp_ui.updateOutline(std::nullopt, false, 22) == tuiide::OutlineDecision::Clear
      && lsp_ui.updateOutline(std::nullopt, false, 23) == tuiide::OutlineDecision::None,
    "closing the active source clears outline state once");
  expect(tuiide::lspOperationLabel(tuiide::LspOperation::OrganizeIncludes) == "Organize Includes",
    "LSP feedback operations have stable user-facing labels");
  tuiide::LspEventBatch lsp_batch;
  tuiide::LspCompletionItem current_completion;
  current_completion.label = "current";
  current_completion.source_path = first_lsp_document.path;
  current_completion.source_version = first_lsp_document.version;
  tuiide::LspCompletionItem stale_completion = current_completion;
  stale_completion.label = "stale";
  stale_completion.source_version = first_lsp_document.version - 1;
  lsp_batch.completions = {current_completion, stale_completion};
  tuiide::LspCodeAction current_action;
  current_action.title = "Current action";
  current_action.source_path = first_lsp_document.path;
  current_action.source_version = first_lsp_document.version;
  auto stale_action = current_action;
  stale_action.title = "Stale action";
  stale_action.source_path = "/project/other.cpp";
  lsp_batch.code_actions = {current_action, stale_action};
  auto routed_lsp_batch = tuiide::LspUiController::route(
    std::move(lsp_batch), first_lsp_document);
  expect(routed_lsp_batch.completions.size() == 1
      && routed_lsp_batch.completions.front().label == "current"
      && routed_lsp_batch.discarded_completions == 1,
    "LSP event routing removes completion responses for stale document revisions");
  expect(routed_lsp_batch.code_actions.size() == 1
      && routed_lsp_batch.code_actions.front().title == "Current action"
      && routed_lsp_batch.discarded_code_actions == 1,
    "LSP event routing removes code actions belonging to another document");

  tuiide::DebugUiController debug_ui;
  tuiide::DebugSnapshot debug_snapshot;
  debug_snapshot.running = true;
  debug_snapshot.stopped = true;
  debug_snapshot.registers_enabled = true;
  debug_snapshot.selected_frame = 1;
  debug_snapshot.watches.push_back({"counter", "7", {}});
  debug_snapshot.registers.push_back({"rax", "0x7"});
  debug_snapshot.threads.push_back({"2", "worker", "stopped", true});
  debug_snapshot.frames.push_back({1, "main", "/project/src/main.cpp", 12});
  debug_snapshot.variables.push_back({"value", "7", "int", "value", "var1", 1, true, false});
  expect(debug_ui.updateDebug(debug_snapshot) && debug_ui.debugRows().size() == 5,
    "debug UI controller builds one typed row for every visible debugger item");
  expect(debug_ui.debugRows()[0].watch_index == 0
      && debug_ui.debugRows()[2].thread_id == "2"
      && debug_ui.debugRows()[3].frame_level == 1
      && debug_ui.debugRows()[3].file == "/project/src/main.cpp"
      && debug_ui.debugRows()[4].variable_index == 0,
    "debug UI rows retain watch, thread, frame, source, and variable actions");
  expect(!debug_ui.updateDebug(debug_snapshot),
    "unchanged debugger snapshots do not request a panel redraw");
  std::vector<tuiide::DebugBreakpoint> controller_breakpoints{
    {"/project/src/main.cpp", 12, true, "value > 0", 2, {}, false, {}}};
  expect(debug_ui.updateBreakpoints(controller_breakpoints, false, "/project")
      && debug_ui.breakpointRows().size() == 1
      && debug_ui.breakpointRows().front().label.find("src/main.cpp:12") != std::string::npos
      && debug_ui.breakpointRows().front().label.find("[not started]") != std::string::npos,
    "breakpoint model uses project-relative labels and inactive debugger state");
  expect(debug_ui.updateBreakpoints(controller_breakpoints, true, "/project")
      && debug_ui.breakpointRows().front().label.find("[unresolved]") != std::string::npos,
    "starting GDB refreshes unresolved breakpoint labels without breakpoint changes");
  expect(!debug_ui.observeActive(true, false)
      && debug_ui.observeActive(false, true)
      && !debug_ui.observeActive(false, true),
    "debug UI controller reports the debuggee-finished transition once");
  expect(debug_ui.debugRow(50) == nullptr && debug_ui.breakpointRow(50) == nullptr,
    "debug UI controller rejects out-of-range panel selections");

  tuiide::EventLog event_log;
  const auto event_file = std::filesystem::temp_directory_path()
    / ("tuiide-event-log-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
  std::string event_file_error;
  expect(event_log.openFile(event_file, event_file_error) && event_file_error.empty(),
    "event log opens an append-only diagnostic file");
  event_log.publish(tuiide::EventChannel::Output, tuiide::EventSource::Project,
    tuiide::EventSeverity::Warning, "project warning\n");
  event_log.publish(tuiide::EventChannel::Build, tuiide::EventSource::Build,
    tuiide::EventSeverity::Success, "build complete\n");
  expect(event_log.text(tuiide::EventChannel::Output) == "project warning\n"
      && event_log.text(tuiide::EventChannel::Build) == "build complete\n",
    "structured event log keeps Output and Build channels independent");
  expect(event_log.events(tuiide::EventChannel::Output).front().source
        == tuiide::EventSource::Project
      && event_log.events(tuiide::EventChannel::Output).front().severity
        == tuiide::EventSeverity::Warning
      && event_log.events(tuiide::EventChannel::Output).front().sequence == 1
      && event_log.events(tuiide::EventChannel::Build).front().sequence == 2,
    "structured events retain source, severity, and global sequence metadata");
  const auto output_revision = event_log.revision(tuiide::EventChannel::Output);
  event_log.publish(tuiide::EventChannel::Output, tuiide::EventSource::System,
    tuiide::EventSeverity::Information, std::string(150001, 'x'));
  expect(event_log.text(tuiide::EventChannel::Output).size() == 120000
      && event_log.events(tuiide::EventChannel::Output).size() == 1
      && event_log.events(tuiide::EventChannel::Output).front().message.size() == 120000,
    "event log trims text and structured history at the same event boundary");
  event_log.clear(tuiide::EventChannel::Output);
  expect(event_log.text(tuiide::EventChannel::Output).empty()
      && event_log.events(tuiide::EventChannel::Output).empty()
      && event_log.revision(tuiide::EventChannel::Output) > output_revision
      && !event_log.text(tuiide::EventChannel::Build).empty(),
    "clearing one event channel preserves the other channel");
  std::ifstream event_input(event_file);
  const std::string event_file_text((std::istreambuf_iterator<char>(event_input)),
    std::istreambuf_iterator<char>());
  expect(event_file_text.find("\toutput\tproject\twarning\tproject warning\\n") != std::string::npos
      && event_file_text.find("\tbuild\tbuild\tsuccess\tbuild complete\\n") != std::string::npos,
    "event log file retains structured metadata and escaped messages");
  std::error_code event_cleanup_error;
  std::filesystem::remove(event_file, event_cleanup_error);

  tuiide::Document document;
  document.insert("int main() {");
  document.newline();
  document.insert("return 0;");
  expect(document.lines().size() == 2, "newline creates a line");
  expect(document.text() == "int main() {\nreturn 0;", "text round trip");
  expect(document.undo(), "undo is available");
  expect(document.line(1).empty(), "undo removes insertion");
  expect(document.redo(), "redo is available");
  expect(document.canUndo() && !document.canRedo(), "document exposes undo and redo availability");

  const auto dirty_path = std::filesystem::temp_directory_path()
    / ("tuiide-dirty-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".cpp");
  tuiide::Document dirty;
  dirty.insert("alpha");
  std::string dirty_error;
  expect(dirty.saveAs(dirty_path, dirty_error) && !dirty.modified(),
    "saving marks the current history state clean");
  dirty.insert(" beta");
  expect(dirty.modified() && dirty.undo() && dirty.text() == "alpha" && !dirty.modified(),
    "undo to the saved history state clears dirty status");
  expect(dirty.redo() && dirty.text() == "alpha beta" && dirty.modified(),
    "redo away from the saved history state restores dirty status");
  expect(dirty.undo() && !dirty.modified(), "a second undo returns exactly to the saved state");
  dirty.insert(" gamma");
  expect(dirty.modified() && !dirty.canRedo(), "editing after undo starts a branch and clears redo");
  std::error_code dirty_cleanup_error;
  std::filesystem::remove(dirty_path, dirty_cleanup_error);

  const auto file_format_directory = std::filesystem::temp_directory_path()
    / ("tuiide-file-format-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(file_format_directory);
  const auto crlf_path = file_format_directory / "crlf.cpp";
  { std::ofstream file(crlf_path, std::ios::binary); file << "\xef\xbb\xbf" "alpha\r\nbeta"; }
  const auto preserved_permissions = std::filesystem::perms::owner_all
    | std::filesystem::perms::group_read | std::filesystem::perms::others_read;
  std::filesystem::permissions(crlf_path, preserved_permissions, std::filesystem::perm_options::replace);
  tuiide::Document formatted_file;
  expect(formatted_file.load(crlf_path, dirty_error)
      && formatted_file.text() == "alpha\nbeta" && formatted_file.hasUtf8Bom()
      && formatted_file.lineEnding() == tuiide::LineEnding::CrLf && !formatted_file.hasFinalNewline(),
    "loading strips UTF-8 BOM, normalizes CRLF, and remembers the missing final newline");
  formatted_file.setCursor({1, 4}); formatted_file.insert("!");
  expect(formatted_file.save(dirty_error), "CRLF document is saved atomically");
  std::ifstream crlf_input(crlf_path, std::ios::binary);
  const std::string crlf_bytes((std::istreambuf_iterator<char>(crlf_input)), std::istreambuf_iterator<char>());
  expect(crlf_bytes == "\xef\xbb\xbf" "alpha\r\nbeta!",
    "saving restores BOM and CRLF without inventing a final newline");
  expect((std::filesystem::status(crlf_path).permissions() & std::filesystem::perms::mask)
      == preserved_permissions, "atomic replacement preserves existing file permissions");
  expect(std::none_of(std::filesystem::directory_iterator(file_format_directory),
      std::filesystem::directory_iterator{}, [](const auto& entry) {
        return entry.path().filename().string().find(".tuiide-") != std::string::npos;
      }), "atomic save leaves no temporary file behind");

  const auto newline_path = file_format_directory / "newline.cpp";
  { std::ofstream file(newline_path, std::ios::binary); file << "line\n"; }
  tuiide::Document newline_file;
  expect(newline_file.load(newline_path, dirty_error) && newline_file.hasFinalNewline()
      && !newline_file.hasUtf8Bom() && newline_file.lineEnding() == tuiide::LineEnding::Lf,
    "LF document remembers its final newline and absence of BOM");
  newline_file.setCursor({0, 4}); newline_file.insert("!");
  expect(newline_file.save(dirty_error), "LF document with final newline is saved");
  std::ifstream newline_input(newline_path, std::ios::binary);
  const std::string newline_bytes((std::istreambuf_iterator<char>(newline_input)), std::istreambuf_iterator<char>());
  expect(newline_bytes == "line!\n", "saving preserves LF and the final newline");
  const auto original_formatted_path = formatted_file.path();
  expect(!formatted_file.saveAs(file_format_directory, dirty_error)
      && formatted_file.path() == original_formatted_path,
    "failed atomic Save As keeps the original document path");

  { std::ofstream external(crlf_path, std::ios::binary | std::ios::trunc); external << "external\n"; }
  expect(formatted_file.diskChange(dirty_error) == tuiide::DiskChange::Modified,
    "content fingerprint detects an external modification even without relying on timestamps");
  formatted_file.acknowledgeDiskState();
  expect(formatted_file.diskChange(dirty_error) == tuiide::DiskChange::Unchanged,
    "Keep acknowledges the current external version without changing editor contents");
  std::filesystem::remove(crlf_path, dirty_cleanup_error);
  expect(formatted_file.diskChange(dirty_error) == tuiide::DiskChange::Deleted,
    "file watcher distinguishes an external deletion");
  formatted_file.acknowledgeDiskState();
  expect(formatted_file.diskChange(dirty_error) == tuiide::DiskChange::Unchanged,
    "keeping a deleted file suppresses repeated notifications until it reappears");

  const auto recovery_path = file_format_directory / "recovery/state.json";
  const std::vector<tuiide::RecoveryDocument> recovery_documents{
    {file_format_directory / "saved.cpp", "int recovered = 1;\n", {0, 8}},
    {{}, "unsaved UTF-8: данные", {0, 21}}
  };
  expect(tuiide::saveRecovery(recovery_path, recovery_documents, dirty_error),
    "modified documents are atomically written to a recovery file");
  std::vector<tuiide::RecoveryDocument> loaded_recovery;
  expect(tuiide::loadRecovery(recovery_path, loaded_recovery, dirty_error)
      && loaded_recovery.size() == 2 && loaded_recovery[0].path == recovery_documents[0].path
      && loaded_recovery[0].text == recovery_documents[0].text
      && loaded_recovery[1].text == recovery_documents[1].text
      && loaded_recovery[1].cursor == recovery_documents[1].cursor,
    "crash recovery preserves saved and untitled UTF-8 documents with cursor positions");
  tuiide::clearRecovery(recovery_path);
  expect(!std::filesystem::exists(recovery_path), "accepted or discarded recovery data is removed");

  tuiide::Document recovered_document;
  recovered_document.restoreText("recovered");
  expect(recovered_document.modified(), "restored autosave content remains explicitly unsaved");
  std::filesystem::remove_all(file_format_directory, dirty_cleanup_error);

  tuiide::Document grouped_typing;
  const std::string large_prefix(200000, 'x');
  grouped_typing.setText(large_prefix);
  grouped_typing.moveEnd();
  for (const char character : std::string(" grouped UTF-8: текст"))
    grouped_typing.insert(std::string_view(&character, 1));
  expect(grouped_typing.undoStorageBytes() < 1024,
    "grouped typing history stores edits instead of full document snapshots");
  expect(grouped_typing.undo() && grouped_typing.text() == large_prefix,
    "sequential typing is reverted as one grouped undo operation");

  tuiide::Document grouped_erase;
  grouped_erase.insert("абв");
  grouped_erase.backspace(); grouped_erase.backspace();
  expect(grouped_erase.text() == "а" && grouped_erase.undo() && grouped_erase.text() == "абв",
    "consecutive UTF-8 backspaces form one reversible operation");

  tuiide::Document smart_editing;
  smart_editing.setText("{}"); smart_editing.setCursor({0, 1});
  smart_editing.smartNewline("  ");
  expect(smart_editing.text() == "{\n  \n}" && smart_editing.cursor() == tuiide::Position{1, 2},
    "smart newline indents inside a matching brace pair");
  expect(smart_editing.undo() && smart_editing.text() == "{}"
      && smart_editing.redo() && smart_editing.cursor() == tuiide::Position{1, 2},
    "smart newline is one undoable edit and restores its inner cursor position");
  smart_editing.setText(""); smart_editing.insertPair('(', ')');
  expect(smart_editing.text() == "()" && smart_editing.cursor() == tuiide::Position{0, 1},
    "paired delimiters place the cursor between both characters");
  expect(smart_editing.undo() && smart_editing.redo()
      && smart_editing.cursor() == tuiide::Position{0, 1},
    "paired delimiter redo restores the cursor between the pair");

  tuiide::Document line_edits;
  line_edits.setText("  alpha\n  beta\ngamma"); line_edits.setCursor({1, 4});
  line_edits.toggleLineComment(0, 1);
  expect(line_edits.text() == "  // alpha\n  // beta\ngamma",
    "toggle comment inserts markers after each line's indentation");
  expect(line_edits.undo() && line_edits.text() == "  alpha\n  beta\ngamma"
      && line_edits.redo() && line_edits.cursor() == tuiide::Position{1, 7},
    "multi-line commenting is atomic and preserves an adjusted cursor");
  line_edits.toggleLineComment(0, 1);
  expect(line_edits.text() == "  alpha\n  beta\ngamma", "toggle comment removes markers and optional spaces");

  tuiide::Document line_structure;
  line_structure.setText("a\nb\nc"); line_structure.setCursor({1, 1});
  line_structure.duplicateLines(1, 1);
  expect(line_structure.text() == "a\nb\nb\nc" && line_structure.cursor() == tuiide::Position{2, 1},
    "duplicate line inserts an undoable copy below the source");
  expect(line_structure.undo() && line_structure.text() == "a\nb\nc", "duplicate line can be undone");
  line_structure.moveLines(1, 1, true);
  expect(line_structure.text() == "a\nc\nb" && line_structure.cursor() == tuiide::Position{2, 1},
    "move line down swaps it with the following line");
  expect(line_structure.undo() && line_structure.text() == "a\nb\nc", "move line can be undone atomically");
  line_structure.deleteLines(1, 1);
  expect(line_structure.text() == "a\nc" && line_structure.undo() && line_structure.text() == "a\nb\nc",
    "delete line removes its newline and is reversible");

  const std::string display_text = "a\t界🙂e\u0301";
  expect(tuiide::displayWidth(display_text, 4) == 9,
    "display width expands tabs and counts CJK, emoji, and combining marks");
  expect(tuiide::displayColumn(display_text, 1, 4) == 1
      && tuiide::displayColumn(display_text, 2, 4) == 4
      && tuiide::displayColumn(display_text, 5, 4) == 6
      && tuiide::displayColumn(display_text, display_text.size(), 4) == 9,
    "UTF-8 byte offsets convert to deterministic screen columns");
  expect(tuiide::displayColumn(display_text, 3, 4) == 4,
    "screen-column conversion never counts a partial UTF-8 code point");
  expect(tuiide::byteColumnAtDisplay(display_text, 5, 4) == 5
      && tuiide::byteColumnAtDisplay(display_text, 7, 4) == 9,
    "mouse columns inside wide characters snap to UTF-8 boundaries");
  expect(tuiide::byteColumnAtDisplay(display_text, 2, 4) == 1
      && tuiide::byteColumnAtDisplay(display_text, 3, 4) == 2,
    "mouse columns inside a tab snap to the nearest text boundary");
  for (std::size_t column = 0; column <= tuiide::displayWidth(display_text, 4); ++column) {
    const auto byte = tuiide::byteColumnAtDisplay(display_text, column, 4);
    expect(byte == display_text.size() || byte == 0
        || (static_cast<unsigned char>(display_text[byte]) & 0xc0U) != 0x80U,
      "screen-to-byte mapping never returns a UTF-8 continuation byte");
  }

  const auto document_labels = tuiide::distinguishDocumentLabels({
    "/workspace/app/src/main.cpp", "/workspace/tests/main.cpp", "/workspace/app/src/widget.cpp",
    {}, {}
  });
  expect(document_labels.size() == 5 && document_labels[0] == "src/main.cpp"
      && document_labels[1] == "tests/main.cpp" && document_labels[2] == "widget.cpp",
    "duplicate basenames receive the shortest distinguishing path suffix");
  expect(document_labels[3] == "Untitled 1" && document_labels[4] == "Untitled 2",
    "multiple unsaved documents receive distinct labels");

  const std::vector<std::string> sidebar_titles{
    "Open files", "Project", "Outline", "Debug", "Breakpoints"};
  const std::vector<bool> all_sidebar_tabs(sidebar_titles.size(), true);
  const auto first_sidebar_page = tuiide::layoutTabBar(
    sidebar_titles, all_sidebar_tabs, 0, 0, 28);
  expect(!first_sidebar_page.items.empty() && first_sidebar_page.items.front().index == 0
      && first_sidebar_page.right_overflow,
    "narrow sidebar keeps the active first tab visible and exposes forward scrolling");
  const auto last_sidebar_page = tuiide::layoutTabBar(
    sidebar_titles, all_sidebar_tabs, 4, 0, 28);
  expect(last_sidebar_page.left_overflow
      && std::any_of(last_sidebar_page.items.begin(), last_sidebar_page.items.end(),
        [](const auto& item) { return item.index == 4; }),
    "selecting an off-screen sidebar tab scrolls it into view");
  auto selected_sidebar_tabs = all_sidebar_tabs;
  selected_sidebar_tabs[1] = false;
  selected_sidebar_tabs[2] = false;
  const auto filtered_sidebar = tuiide::layoutTabBar(
    sidebar_titles, selected_sidebar_tabs, 4, 0, 28);
  expect(std::none_of(filtered_sidebar.items.begin(), filtered_sidebar.items.end(),
      [](const auto& item) { return item.index == 1 || item.index == 2; })
      && std::any_of(filtered_sidebar.items.begin(), filtered_sidebar.items.end(),
        [](const auto& item) { return item.index == 4; }),
    "hidden sidebar panels are omitted while the active visible panel remains reachable");

  const auto empty_commands = tuiide::commandAvailability({});
  expect(!empty_commands.save && !empty_commands.build && !empty_commands.completion,
    "commands requiring context are disabled in an empty workspace");
  tuiide::CommandContext project_context;
  project_context.has_project = true;
  const auto project_commands = tuiide::commandAvailability(project_context);
  expect(project_commands.close_project && project_commands.project_file && project_commands.build
      && !project_commands.cancel_build && project_commands.run && !project_commands.stop_run
      && project_commands.cmake_configuration,
    "project commands are enabled for an idle open project");
  project_context.run_running = true;
  const auto active_run_commands = tuiide::commandAvailability(project_context);
  expect(!active_run_commands.run && active_run_commands.stop_run,
    "active Run enables explicit program termination and prevents a duplicate launch");
  project_context.run_running = false;
  project_context.has_document = true;
  project_context.has_saved_document = true;
  project_context.document_modified = true;
  project_context.source_document = true;
  project_context.has_selection = true;
  project_context.has_modified_documents = true;
  project_context.has_closed_document = true;
  project_context.can_undo = true;
  project_context.lsp_ready = true;
  project_context.document_count = 2;
  const auto source_commands = tuiide::commandAvailability(project_context);
  expect(source_commands.save && source_commands.save_all && source_commands.undo && source_commands.cut && source_commands.copy
      && source_commands.definition && source_commands.references && source_commands.completion
      && source_commands.signature_help && source_commands.hover && source_commands.rename && source_commands.code_actions
      && source_commands.workspace_symbols && source_commands.hierarchy && source_commands.breakpoint
      && source_commands.format_document && source_commands.format_selection
      && source_commands.switch_document && source_commands.close_all && source_commands.close_others
      && source_commands.reopen_closed,
    "editor and clangd commands follow document capabilities");
  project_context.document_modified = false;
  expect(!tuiide::commandAvailability(project_context).save,
    "Save is disabled for an already saved clean document");
  project_context.document_modified = true;
  project_context.build_running = true;
  const auto building_commands = tuiide::commandAvailability(project_context);
  expect(!building_commands.build && !building_commands.run && !building_commands.cmake_configuration
      && !building_commands.debug_start && building_commands.cancel_build,
    "conflicting project commands are disabled while a build is running");
  project_context.build_running = false;
  project_context.gdb_running = true;
  project_context.gdb_active = true;
  const auto running_debug_commands = tuiide::commandAvailability(project_context);
  expect(running_debug_commands.debug_pause && !running_debug_commands.debug_step
      && !running_debug_commands.debug_start && running_debug_commands.debug_stop
      && running_debug_commands.debug_restart,
    "running debugger enables pause but disables stepping and continue");
  project_context.gdb_stopped = true;
  const auto stopped_debug_commands = tuiide::commandAvailability(project_context);
  expect(!stopped_debug_commands.debug_pause && stopped_debug_commands.debug_step
      && stopped_debug_commands.debug_start && stopped_debug_commands.debug_stop
      && stopped_debug_commands.debug_restart,
    "stopped debugger enables stepping and continue");
  project_context.gdb_active = false;
  project_context.gdb_stopped = false;
  const auto exited_debug_commands = tuiide::commandAvailability(project_context);
  expect(exited_debug_commands.debug_start && exited_debug_commands.debug_stop
      && exited_debug_commands.debug_restart && !exited_debug_commands.debug_pause
      && !exited_debug_commands.debug_step,
    "exited inferior can be started or restarted while stale stepping commands stay disabled");
  expect(tuiide::lspLanguageId("source.c") == "c"
      && tuiide::lspLanguageId("source.cpp") == "cpp"
      && tuiide::lspLanguageId("header.hpp") == "cpp",
    "clangd language id distinguishes C from C++ documents");
  const auto unicode_lsp_path = std::filesystem::absolute("directory with spaces/файл.cpp");
  const auto unicode_lsp_uri = tuiide::lspFileUri(unicode_lsp_path);
  expect(unicode_lsp_uri.find(' ') == std::string::npos && unicode_lsp_uri.find("%D1%84") != std::string::npos,
    "LSP file URI percent-encodes spaces and UTF-8 bytes");
  const auto reserved_lsp_path = std::filesystem::absolute("directory/a#b?c%d.cpp");
  const auto reserved_lsp_uri = tuiide::lspFileUri(reserved_lsp_path);
  expect(reserved_lsp_uri.find("%23") != std::string::npos
      && reserved_lsp_uri.find("%3F") != std::string::npos
      && reserved_lsp_uri.find("%25") != std::string::npos
      && tuiide::lspPathFromFileUri(reserved_lsp_uri) == reserved_lsp_path.lexically_normal(),
    "LSP file URI round-trips reserved filename bytes without treating them as URI syntax");
  expect(tuiide::lspPathFromFileUri("file://localhost/tmp/source.cpp") == "/tmp/source.cpp"
      && tuiide::lspPathFromFileUri("https://example.test/source.cpp").empty()
      && tuiide::lspPathFromFileUri("file://remote-host/tmp/source.cpp").empty()
      && tuiide::lspPathFromFileUri("file:///tmp/bad%2G.cpp").empty()
      && tuiide::lspPathFromFileUri("file:///tmp/bad%00.cpp").empty(),
    "LSP URI decoder accepts local file URIs and rejects schemes, authorities, and malformed escapes");
  expect(!tuiide::lspResponseError({{"jsonrpc", "2.0"}, {"result", nullptr}})
      && tuiide::lspResponseError({{"error", {{"code", -32602}, {"message", "invalid params"}}}})
        == "invalid params"
      && tuiide::lspResponseError({{"error", {{"code", -32601}, {"message", 42}}}})
        == "JSON-RPC error -32601"
      && tuiide::lspResponseError({{"error", "broken"}})
        == "Malformed JSON-RPC error response",
    "LSP JSON-RPC errors are parsed without throwing on missing or malformed messages");
  const nlohmann::json nullable_fields{{"text", nullptr}, {"count", nullptr},
    {"wrong", nlohmann::json::object()}, {"result", nlohmann::json::object()}};
  expect(tuiide::jsonValueOr(nullable_fields, "text", std::string("fallback"))
        == "fallback"
      && tuiide::jsonValueOr(nullable_fields, "count", 17) == 17
      && tuiide::jsonValueOr(nullable_fields, "wrong", std::string("safe"))
        == "safe"
      && tuiide::jsonFieldOrNull(nullable_fields, "result").is_object()
      && tuiide::jsonFieldOrNull(nullable_fields, "missing").is_null(),
    "external JSON fields accept null, missing, and mismatched types safely");

  const auto hierarchical_symbols = tuiide::parseDocumentSymbols(nlohmann::json::array({
    {{"name", "Widget"}, {"detail", "class Widget"}, {"kind", 5},
      {"selectionRange", {{"start", {{"line", 2}, {"character", 6}}},
        {"end", {{"line", 2}, {"character", 12}}}}},
      {"children", nlohmann::json::array({
        {{"name", "draw"}, {"kind", 6}, {"selectionRange", {
          {"start", {{"line", 5}, {"character", 7}}}, {"end", {{"line", 5}, {"character", 11}}}}}}
      })}}
  }), "/tmp/widget.cpp");
  expect(hierarchical_symbols.size() == 2 && hierarchical_symbols[0].name == "Widget"
      && hierarchical_symbols[0].depth == 0 && hierarchical_symbols[0].position == tuiide::Position{2, 6}
      && hierarchical_symbols[1].name == "draw" && hierarchical_symbols[1].depth == 1,
    "documentSymbol parser preserves hierarchical classes and methods with UTF-16 positions");
  auto nullable_symbols = nlohmann::json::array({
    {{"name", "Nullable"}, {"detail", nullptr}, {"kind", nullptr},
      {"selectionRange", {{"start", {{"line", 1}, {"character", 2}}},
        {"end", {{"line", 1}, {"character", 10}}}}}}
  });
  const auto safe_symbols = tuiide::parseDocumentSymbols(nullable_symbols,
    "/tmp/nullable.cpp");
  expect(safe_symbols.size() == 1 && safe_symbols.front().detail.empty()
      && safe_symbols.front().kind == 0,
    "documentSymbol parser tolerates nullable optional fields");
  const auto flat_symbol_path = std::filesystem::absolute("flat.cpp");
  const auto flat_symbols = tuiide::parseDocumentSymbols(nlohmann::json::array({
    {{"name", "main"}, {"containerName", "global"}, {"kind", 12}, {"location", {
      {"uri", tuiide::lspFileUri(flat_symbol_path)}, {"range", {
        {"start", {{"line", 9}, {"character", 0}}}, {"end", {{"line", 9}, {"character", 4}}}}}}}},
    {{"name", "foreign"}, {"kind", 12}, {"location", {
      {"uri", tuiide::lspFileUri(flat_symbol_path.parent_path() / "other.cpp")}, {"range", {
        {"start", {{"line", 1}, {"character", 0}}}, {"end", {{"line", 1}, {"character", 7}}}}}}}}
  }), flat_symbol_path);
  expect(flat_symbols.size() == 1 && flat_symbols[0].name == "main" && flat_symbols[0].detail == "global",
    "documentSymbol parser supports flat SymbolInformation and filters foreign files");
  expect(tuiide::lspPathFromFileUri(unicode_lsp_uri) == unicode_lsp_path.lexically_normal(),
    "LSP file URI round-trips Unicode paths");

  const auto compilation_root = std::filesystem::temp_directory_path()
      / ("tuiide-cdb-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(compilation_root / "build");
  std::filesystem::create_directories(compilation_root / "src");
  { std::ofstream file(compilation_root / "src/main.cpp"); file << "int main() {}\n"; }
  { std::ofstream file(compilation_root / "src/not-built.cpp"); file << "int unused;\n"; }
  { std::ofstream database(compilation_root / "build/compile_commands.json"); database << R"([
    {"directory":"../src","file":"main.cpp","command":"c++ -c main.cpp"},
    {"directory":"../src","file":"main.cpp","arguments":["c++","-c","main.cpp"]}
  ])"; }
  tuiide::CompilationDatabase compilation_database;
  std::string compilation_error;
  expect(compilation_database.load(compilation_root / "build", compilation_error)
      && compilation_database.available() && compilation_database.size() == 1
      && compilation_database.contains(compilation_root / "src/main.cpp")
      && !compilation_database.contains(compilation_root / "src/not-built.cpp"),
    "compilation database resolves relative entries, deduplicates them, and reports missing sources");
  { std::ofstream database(compilation_root / "build/compile_commands.json", std::ios::trunc); database << "{broken"; }
  expect(!compilation_database.load(compilation_root / "build", compilation_error)
      && !compilation_database.available() && !compilation_error.empty(),
    "malformed compilation database is rejected without retaining stale entries");
  std::error_code compilation_cleanup_error;
  std::filesystem::remove_all(compilation_root, compilation_cleanup_error);

  const auto workspace_root = std::filesystem::temp_directory_path()
      / ("tuiide-workspace-edit-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(workspace_root / "old/subdirectory");
  { std::ofstream file(workspace_root / "old.cpp"); file << "old\n"; }
  { std::ofstream file(workspace_root / "old/subdirectory/data.txt"); file << "delete me\n"; }
  const auto parsed_workspace = tuiide::parseWorkspaceEdit({{"documentChanges", nlohmann::json::array({
    {{"textDocument", {{"uri", tuiide::lspFileUri(workspace_root / "old.cpp")}, {"version", 7}}},
      {"edits", nlohmann::json::array({{{"range", {{"start", {{"line", 0}, {"character", 0}}},
        {"end", {{"line", 0}, {"character", 3}}}}}, {"newText", "new"}}})}},
    {{"kind", "create"}, {"uri", tuiide::lspFileUri(workspace_root / "created.hpp")}},
    {{"kind", "rename"}, {"oldUri", tuiide::lspFileUri(workspace_root / "old.cpp")},
      {"newUri", tuiide::lspFileUri(workspace_root / "renamed.cpp")}, {"options", {{"overwrite", true}}}},
    {{"kind", "delete"}, {"uri", tuiide::lspFileUri(workspace_root / "old")},
      {"options", {{"recursive", true}, {"ignoreIfNotExists", true}}}}
  })}});
  expect(parsed_workspace.files.size() == 1 && parsed_workspace.files[0].version == 7
      && parsed_workspace.file_operations.size() == 3
      && parsed_workspace.file_operations[1].kind == tuiide::WorkspaceFileOperationKind::Rename
      && parsed_workspace.file_operations[1].overwrite
      && parsed_workspace.file_operations[2].recursive
      && parsed_workspace.file_operations[2].ignore_if_not_exists,
    "workspace edit parser retains document versions and resource-operation options");
  tuiide::WorkspaceFileTransaction workspace_transaction;
  std::string workspace_error;
  expect(workspace_transaction.prepare(workspace_root, parsed_workspace.file_operations, workspace_error)
      && workspace_transaction.apply(workspace_error),
    "workspace file transaction applies create, rename, and recursive delete operations");
  expect(std::filesystem::exists(workspace_root / "created.hpp")
      && std::filesystem::exists(workspace_root / "renamed.cpp")
      && !std::filesystem::exists(workspace_root / "old.cpp")
      && !std::filesystem::exists(workspace_root / "old"),
    "workspace file operations produce the requested filesystem state");
  expect(workspace_transaction.rollback(workspace_error)
      && !std::filesystem::exists(workspace_root / "created.hpp")
      && std::filesystem::exists(workspace_root / "old.cpp")
      && std::filesystem::exists(workspace_root / "old/subdirectory/data.txt"),
    "workspace file transaction restores every operation on rollback");
  expect(workspace_transaction.apply(workspace_error) && workspace_transaction.commit(workspace_error)
      && std::filesystem::exists(workspace_root / "created.hpp")
      && std::filesystem::exists(workspace_root / "renamed.cpp")
      && !std::filesystem::exists(workspace_root / "old"),
    "committed workspace file transaction removes staged deletion backups");
  std::error_code workspace_cleanup_error;
  std::filesystem::remove_all(workspace_root, workspace_cleanup_error);

  const auto path_test = std::filesystem::temp_directory_path()
      / ("tuiide-path-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(path_test / "real");
  { std::ofstream file(path_test / "real/source.cpp"); file << "int value;\n"; }
  std::error_code symlink_error;
  std::filesystem::create_directory_symlink(path_test / "real", path_test / "alias", symlink_error);
  expect(!symlink_error, "path fixture symlink is created");
  expect(tuiide::normalizePath(path_test / "real/../real/source.cpp")
      == tuiide::normalizePath(path_test / "alias/source.cpp"), "document identity resolves dot segments and symlinks");
  tuiide::Document normalized_document;
  std::string document_error;
  expect(normalized_document.load(path_test / "alias/source.cpp", document_error), "document loads through a symlink");
  expect(normalized_document.path() == tuiide::normalizePath(path_test / "real/source.cpp"), "loaded document stores normalized identity");
  std::filesystem::remove_all(path_test, symlink_error);

  const auto session_documents = std::filesystem::temp_directory_path()
      / ("tuiide-document-session-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(session_documents);
  { std::ofstream file(session_documents / "first.cpp"); file << "int first;\n"; }
  { std::ofstream file(session_documents / "second.cpp"); file << "int second;\n"; }
  tuiide::DocumentSession document_session;
  auto first_open = document_session.open(session_documents / "first.cpp", document_error);
  auto duplicate_open = document_session.open(session_documents / "./first.cpp", document_error);
  expect(first_open && first_open->newly_loaded && duplicate_open && !duplicate_open->newly_loaded
      && document_session.documents().size() == 1 && document_session.activeIndex() == 0,
    "document session normalizes identities and activates an existing document without duplicating it");
  auto second_open = document_session.open(session_documents / "second.cpp", document_error);
  expect(second_open && second_open->newly_loaded && document_session.documents().size() == 2,
    "document session owns newly loaded documents");
  document_session.activate(0)->setCursor({0, 3});
  document_session.closeActive();
  expect(document_session.documents().size() == 1
      && document_session.activeDocument() == second_open->document && document_session.activeIndex() == 0,
    "closing the active document selects the nearest remaining document");
  const auto closed_document = document_session.takeLastClosed();
  expect(closed_document && closed_document->path == tuiide::normalizePath(session_documents / "first.cpp")
      && closed_document->cursor == tuiide::Position{0, 3},
    "document session retains the path and cursor in reopen history");
  expect(!document_session.open(session_documents / "missing.cpp", document_error) && !document_error.empty(),
    "document session reports load failures without adding a document");
  const auto untitled = document_session.createUntitled();
  expect(untitled.newly_loaded && untitled.document->path().empty()
      && document_session.documents().size() == 2,
    "document session creates and activates an untitled document");
  document_session.clear();
  expect(document_session.documents().empty() && !document_session.activeDocument()
      && document_session.closedDocuments().empty(),
    "clearing a document session removes documents, active state, and reopen history");
  std::filesystem::remove_all(session_documents, symlink_error);

  const auto history_test = std::filesystem::temp_directory_path()
    / ("tuiide-history-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto first_project = history_test / "first";
  const auto second_project = history_test / "second";
  const auto history_file = history_test / "config" / "recent.json";
  std::filesystem::create_directories(first_project);
  std::filesystem::create_directories(second_project);
  std::string history_error;
  expect(!tuiide::validateProjectDirectory(first_project, history_error)
    && history_error.find("CMakeLists.txt") != std::string::npos,
    "project validation rejects arbitrary directories");
  { std::ofstream file(first_project / "CMakeLists.txt"); file << "project(first)\n"; }
  { std::ofstream file(second_project / "CMakeLists.txt"); file << "project(second)\n"; }
  expect(tuiide::rememberRecentProject(history_file, first_project, history_error)
    && tuiide::rememberRecentProject(history_file, second_project, history_error)
    && tuiide::rememberRecentProject(history_file, first_project, history_error),
    "recent projects are stored atomically");
  auto recent_projects = tuiide::loadRecentProjects(history_file, history_error);
  expect(history_error.empty() && recent_projects == std::vector<std::filesystem::path>{
      tuiide::normalizePath(first_project), tuiide::normalizePath(second_project)},
    "recent projects are normalized, deduplicated, and ordered by last use");
  std::filesystem::remove(second_project / "CMakeLists.txt");
  recent_projects = tuiide::loadRecentProjects(history_file, history_error);
  expect(recent_projects == std::vector<std::filesystem::path>{tuiide::normalizePath(first_project)},
    "recent project loading drops directories that are no longer CMake projects");
  std::filesystem::remove_all(history_test, symlink_error);

  const auto tree_project = std::filesystem::temp_directory_path()
      / ("tuiide-tree-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto tree_build = tree_project / "build";
  std::filesystem::create_directories(tree_project / "src/empty");
  std::filesystem::create_directories(tree_project / ".git");
  std::filesystem::create_directories(tree_build);
  { std::ofstream file(tree_project / "README.md"); file << "readme\n"; }
  { std::ofstream file(tree_project / "src/main.cpp"); file << "int main() {}\n"; }
  { std::ofstream file(tree_project / "asset.dat"); file << "asset\n"; }
  { std::ofstream file(tree_project / ".git/hidden.cpp"); file << "hidden\n"; }
  { std::ofstream file(tree_build / "generated.cpp"); file << "generated\n"; }
  for (int index = 0; index < 510; ++index) {
    std::ofstream file(tree_project / ("item-" + std::to_string(index) + ".data")); file << index;
  }
  tuiide::ProjectTreeSnapshot tree_snapshot;
  expect(tuiide::scanProjectTree(tree_project, tree_build, {}, tree_snapshot, history_error),
    "project tree scans all project entries");
  expect(tree_snapshot.scanned_files == 513 && tree_snapshot.entries.size() >= 515,
    "project tree displays arbitrary files and does not silently truncate at the old 500-file limit");
  expect(std::find(tree_snapshot.editable_files.begin(), tree_snapshot.editable_files.end(),
      tuiide::normalizePath(tree_project / "src/main.cpp")) != tree_snapshot.editable_files.end()
      && std::none_of(tree_snapshot.entries.begin(), tree_snapshot.entries.end(), [&](const auto& entry) {
        return entry.path == tuiide::normalizePath(tree_project / ".git/hidden.cpp")
          || entry.path == tuiide::normalizePath(tree_build / "generated.cpp");
      }), "project tree keeps editable files while excluding VCS and selected build directories");
  expect(tuiide::scanProjectTree(tree_project, tree_build, "MAIN", tree_snapshot, history_error)
      && std::any_of(tree_snapshot.entries.begin(), tree_snapshot.entries.end(), [](const auto& entry) {
        return entry.path.filename() == "main.cpp";
      }) && std::none_of(tree_snapshot.entries.begin(), tree_snapshot.entries.end(), [](const auto& entry) {
        return entry.path.filename() == "asset.dat";
      }), "project tree filter is case-insensitive and matches relative paths");
  expect(tuiide::createProjectDirectory(tree_project, tree_project / "generated/nested", history_error)
      && std::filesystem::is_directory(tree_project / "generated/nested"),
    "project tree creates nested directories inside the workspace");
  expect(!tuiide::deleteEmptyProjectDirectory(tree_project, tree_project / "generated", history_error)
      && tuiide::deleteEmptyProjectDirectory(tree_project, tree_project / "generated/nested", history_error),
    "directory deletion refuses non-empty directories and removes an empty selected directory");
  expect(tuiide::renameProjectEntry(tree_project, tree_project / "asset.dat",
      tree_project / "src/renamed.asset", history_error)
      && std::filesystem::exists(tree_project / "src/renamed.asset"),
    "project entries can be renamed or moved inside the workspace");
  expect(!tuiide::renameProjectEntry(tree_project, tree_project / "README.md",
      tree_project.parent_path() / "escaped.md", history_error),
    "project move cannot escape the workspace root");
  std::filesystem::remove_all(tree_project, symlink_error);

  tuiide::AsyncProcess process_tree;
  expect(process_tree.start({"sh", "-c", "sleep 30 & wait"}), "process tree starts");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto stop_started = std::chrono::steady_clock::now();
  process_tree.stop();
  expect(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(2),
    "stopping a process also terminates descendants holding output pipes");
  tuiide::AsyncProcess environment_process;
  expect(environment_process.start({"sh", "-c", "printf '%s' \"$TUIIDE_PROCESS_VALUE\""}, true, {},
      {{"TUIIDE_PROCESS_VALUE", "UTF-8: данные"}}),
    "process starts with project environment overrides");
  for (int attempt = 0; attempt < 100 && environment_process.running(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  environment_process.stop();
  std::string environment_output;
  for (const auto& chunk : environment_process.drain()) environment_output += chunk;
  expect(environment_output == "UTF-8: данные", "child process receives UTF-8 project environment");
  tuiide::AsyncProcess stdin_process;
  expect(stdin_process.start({"sh", "-c", "cat"}), "process accepting configured stdin starts");
  expect(stdin_process.write("UTF-8 stdin: данные\n"), "process accepts UTF-8 stdin data");
  stdin_process.closeInput();
  for (int attempt = 0; attempt < 100 && stdin_process.running(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  stdin_process.stop();
  std::string stdin_output;
  for (const auto& chunk : stdin_process.drain()) stdin_output += chunk;
  expect(stdin_output == "UTF-8 stdin: данные\n", "closing process stdin delivers EOF without losing data");

  tuiide::AsyncProcess reusable_process;
  for (int run = 1; run <= 3; ++run) {
    expect(reusable_process.start({"sh", "-c", "printf 'run-%s' \"$1\"", "sh",
        std::to_string(run)}), "completed AsyncProcess can start again");
    for (int attempt = 0; attempt < 200 && reusable_process.running(); ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    reusable_process.stop();
    std::string output;
    for (const auto& chunk : reusable_process.drain()) output += chunk;
    expect(output == "run-" + std::to_string(run),
      "repeated AsyncProcess start/stop keeps descriptors and output isolated");
  }

  tuiide::AsyncProcess orphan_process;
  expect(orphan_process.start({"sh", "-c", "sleep 30 & printf '%s' \"$!\""}),
    "process with a background descendant starts");
  for (int attempt = 0; attempt < 200 && orphan_process.running(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::string orphan_output;
  for (int attempt = 0; attempt < 200 && orphan_output.empty(); ++attempt) {
    for (const auto& chunk : orphan_process.drain()) orphan_output += chunk;
    if (orphan_output.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(!orphan_output.empty(), "leader output is drained while its descendant holds the pipe");
  const auto orphan_pid = static_cast<pid_t>(std::stol(orphan_output));
  expect(!orphan_process.running() && ::kill(orphan_pid, 0) == 0,
    "background descendant remains observable after its process-group leader exits");
  orphan_process.stop();
  bool orphan_terminated{};
  for (int attempt = 0; attempt < 100 && !orphan_terminated; ++attempt) {
    errno = 0;
    if (::kill(orphan_pid, 0) != 0 && errno == ESRCH) orphan_terminated = true;
    else {
      std::ifstream status("/proc/" + std::to_string(orphan_pid) + "/stat");
      std::string pid_field; std::string command_field; char state{};
      if (status >> pid_field >> command_field >> state && state == 'Z') orphan_terminated = true;
    }
    if (!orphan_terminated) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(orphan_terminated,
    "stop terminates background descendants after the process-group leader has exited");

  tuiide::PseudoTerminal pseudo_terminal;
  expect(pseudo_terminal.start({"sh", "-c", "stty size; IFS= read -r line; printf 'received:%s\\n' \"$line\""},
      {}, {}, 40, 10), "PTY process starts with a configured terminal size");
  expect(pseudo_terminal.write("интерактивный ввод\n"), "PTY accepts interactive UTF-8 input");
  for (int attempt = 0; attempt < 200 && pseudo_terminal.running(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::string terminal_output;
  for (const auto& chunk : pseudo_terminal.drain()) terminal_output += chunk;
  const auto terminal_exit = pseudo_terminal.exitCode();
  pseudo_terminal.stop();
  expect(terminal_exit && *terminal_exit == 0 && terminal_output.find("10 40") != std::string::npos
      && terminal_output.find("received:интерактивный ввод") != std::string::npos,
    "PTY exposes dimensions and transports terminal input and output");

  tuiide::PseudoTerminal bounded_terminal;
  expect(bounded_terminal.start({"sh", "-c", "stty size; IFS= read -r line; stty size"},
      {}, {}, std::numeric_limits<unsigned>::max(), 0),
    "PTY clamps out-of-range initial dimensions");
  std::string bounded_output;
  for (int attempt = 0; attempt < 200
      && bounded_output.find("1 65535") == std::string::npos; ++attempt) {
    for (const auto& chunk : bounded_terminal.drain()) bounded_output += chunk;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(bounded_terminal.resize(0, std::numeric_limits<unsigned>::max()),
    "PTY clamps out-of-range resize dimensions");
  expect(bounded_terminal.write("continue\n"), "resized PTY accepts input");
  for (int attempt = 0; attempt < 200 && bounded_terminal.running(); ++attempt) {
    for (const auto& chunk : bounded_terminal.drain()) bounded_output += chunk;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  bounded_terminal.stop();
  for (const auto& chunk : bounded_terminal.drain()) bounded_output += chunk;
  expect(bounded_output.find("1 65535") != std::string::npos
      && bounded_output.find("65535 1") != std::string::npos,
    "PTY applies clamped initial and resized terminal dimensions");

  tuiide::PseudoTerminal reusable_terminal;
  for (int run = 1; run <= 3; ++run) {
    expect(reusable_terminal.start({"sh", "-c", "printf 'pty-%s\\n' \"$1\"", "sh",
        std::to_string(run)}), "completed PTY can start again");
    for (int attempt = 0; attempt < 200 && reusable_terminal.running(); ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto code = reusable_terminal.exitCode();
    reusable_terminal.stop();
    std::string output;
    for (const auto& chunk : reusable_terminal.drain()) output += chunk;
    expect(code && *code == 0 && output.find("pty-" + std::to_string(run)) != std::string::npos,
      "repeated PTY start/stop keeps process state and output isolated");
  }

  tuiide::PseudoTerminal orphan_terminal;
  expect(orphan_terminal.start({"sh", "-c", "sleep 30 & printf '%s\\n' \"$!\""}),
    "PTY process with a background descendant starts");
  for (int attempt = 0; attempt < 200 && orphan_terminal.running(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  std::string orphan_terminal_output;
  for (int attempt = 0; attempt < 200 && orphan_terminal_output.empty(); ++attempt) {
    for (const auto& chunk : orphan_terminal.drain()) orphan_terminal_output += chunk;
    if (orphan_terminal_output.empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(!orphan_terminal_output.empty(),
    "PTY drains leader output while a descendant holds the slave");
  const auto orphan_terminal_pid = static_cast<pid_t>(std::stol(orphan_terminal_output));
  expect(!orphan_terminal.running() && ::kill(orphan_terminal_pid, 0) == 0,
    "PTY retains its process group after the leader exits");
  orphan_terminal.stop();
  bool orphan_terminal_terminated{};
  for (int attempt = 0; attempt < 100 && !orphan_terminal_terminated; ++attempt) {
    errno = 0;
    if (::kill(orphan_terminal_pid, 0) != 0 && errno == ESRCH)
      orphan_terminal_terminated = true;
    else {
      std::ifstream status("/proc/" + std::to_string(orphan_terminal_pid) + "/stat");
      std::string pid_field; std::string command_field; char state{};
      if (status >> pid_field >> command_field >> state && state == 'Z')
        orphan_terminal_terminated = true;
    }
    if (!orphan_terminal_terminated)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(orphan_terminal_terminated,
    "PTY stop terminates descendants after the process-group leader exits");

  const auto missing_directory = std::filesystem::temp_directory_path()
    / ("tuiide-missing-pty-directory-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto descriptorCount = [] {
    return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator("/proc/self/fd"),
      std::filesystem::directory_iterator{}));
  };
  const auto descriptors_before = descriptorCount();
  for (int attempt = 0; attempt < 16; ++attempt)
    expect(!reusable_terminal.start({"sh", "-c", "true"}, missing_directory),
      "PTY reports a spawn failure for a missing working directory");
  expect(descriptorCount() == descriptors_before && !reusable_terminal.running()
      && reusable_terminal.slaveName().empty(),
    "failed PTY spawn attempts release master, slave, actions, and attributes");

  tuiide::PseudoTerminal debug_session;
  expect(debug_session.openSession(0, std::numeric_limits<unsigned>::max()),
    "standalone PTY session clamps dimensions and opens");
  const auto session_slave = ::open(debug_session.slaveName().c_str(), O_RDWR | O_NOCTTY);
  winsize session_size{};
  const bool session_sized = session_slave >= 0
    && ::ioctl(session_slave, TIOCGWINSZ, &session_size) == 0;
  if (session_slave >= 0) (void)::close(session_slave);
  expect(session_sized && session_size.ws_col == 1
      && session_size.ws_row == std::numeric_limits<unsigned short>::max(),
    "standalone PTY session exposes clamped dimensions on its slave");
  debug_session.stop();
  expect(debug_session.openSession(32, 8), "standalone PTY session can reopen after stop");
  debug_session.stop();
  expect(!debug_session.running() && debug_session.slaveName().empty(),
    "repeated standalone PTY sessions release their descriptors");

  tuiide::RunSession run_session;
  expect(run_session.start({"sh", "-c", "printf 'run-session-output'"},
      tuiide::RunTransport::Process, {}, {}),
    "run session starts the selected process transport");
  std::string run_output;
  std::optional<int> run_completion;
  for (int attempt = 0; attempt < 200 && !run_completion; ++attempt) {
    auto poll = run_session.poll();
    for (const auto& chunk : poll.output) run_output += chunk;
    run_completion = poll.completion;
    if (!run_completion) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(run_completion && *run_completion == 0 && run_output == "run-session-output"
      && !run_session.running(),
    "run session reports output and completion as one lifecycle transition");
  expect(run_session.openDebugConsole(72, 18) && !run_session.running()
      && run_session.consoleRunning() && !run_session.debugTerminal().empty(),
    "debug console PTY does not masquerade as an active Run command");
  run_session.stop();
  expect(!run_session.consoleRunning(), "stopping a run session also closes its debug console PTY");

  tuiide::TerminalBuffer terminal_buffer;
  terminal_buffer.append("progress 10%\rprogress 90%\x1b[K\n\x1b[31mошибка\x1b[0m\n");
  expect(terminal_buffer.text() == "progress 90%\nошибка\n",
    "terminal buffer applies carriage returns and removes ANSI styling without damaging UTF-8");

  const auto formatter_tools = std::filesystem::temp_directory_path()
      / ("tuiide-format-tools-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(formatter_tools);
  const auto formatter_arguments = formatter_tools / "arguments.txt";
  const auto formatter = formatter_tools / "clang-format-test";
  { std::ofstream script(formatter); script << "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$TUIIDE_FORMAT_ARGS\"\nsed 's/int  /int /g'\n"; }
  std::filesystem::permissions(formatter, std::filesystem::perms::owner_all);
  ::setenv("TUIIDE_FORMAT_ARGS", formatter_arguments.c_str(), 1);
  const auto formatted = tuiide::clangFormat("int  main() {}\n", "/tmp/sample.cpp",
    tuiide::FormatLineRange{2, 4}, formatter.string());
  expect(formatted.success && formatted.text == "int main() {}\n",
    "clang-format filter returns formatted UTF-8 source text");
  std::ifstream formatter_arguments_input(formatter_arguments);
  const std::string formatter_arguments_text((std::istreambuf_iterator<char>(formatter_arguments_input)),
    std::istreambuf_iterator<char>());
  expect(formatter_arguments_text.find("--assume-filename=/tmp/sample.cpp") != std::string::npos
      && formatter_arguments_text.find("--lines=3:5") != std::string::npos,
    "clang-format receives the assumed filename and one-based selected line range");
  const auto unchanged_format = tuiide::clangFormat("int main() {}\n", "/tmp/sample.cpp",
    {}, formatter.string());
  expect(unchanged_format.success && unchanged_format.text == "int main() {}\n",
    "clang-format reports a successful no-change result");
  const auto missing_formatter = tuiide::clangFormat("int x;\n", "/tmp/sample.cpp", {},
    (formatter_tools / "missing-clang-format").string());
  expect(!missing_formatter.success && missing_formatter.error.find("not found") != std::string::npos,
    "missing clang-format executable produces an actionable error without SIGPIPE");
  const auto failing_formatter = formatter_tools / "clang-format-failing";
  { std::ofstream script(failing_formatter); script << "#!/bin/sh\necho 'invalid style' >&2\nexit 9\n"; }
  std::filesystem::permissions(failing_formatter, std::filesystem::perms::owner_all);
  const auto failed_format = tuiide::clangFormat("int x;\n", "/tmp/sample.cpp", {},
    failing_formatter.string());
  expect(!failed_format.success && failed_format.error.find("exit code 9: invalid style") != std::string::npos,
    "clang-format failure preserves its exit code and diagnostic text");
  ::unsetenv("TUIIDE_FORMAT_ARGS");
  std::filesystem::remove_all(formatter_tools, clipboard_cleanup_error);

  tuiide::Document formatted_document;
  formatted_document.setText("int  main() {}\n"); formatted_document.setCursor({0, 4});
  formatted_document.replaceTextPreservingCursor("int main() {}\n");
  expect(formatted_document.text() == "int main() {}\n"
      && formatted_document.cursor() == tuiide::Position{0, 4},
    "formatted text is installed while preserving the cursor");
  expect(formatted_document.undo() && formatted_document.text() == "int  main() {}\n"
      && formatted_document.redo() && formatted_document.cursor() == tuiide::Position{0, 4},
    "whole-document formatting is one reversible undo/redo operation");

  tuiide::Document completion;
  completion.insert("std::vec");
  completion.replaceIdentifierBeforeCursor("vector");
  expect(completion.text() == "std::vector", "completion replaces identifier prefix");
  expect(completion.undo(), "completion replacement is undoable");
  expect(completion.text() == "std::vec", "undo restores completion prefix");

  tuiide::Document lsp_positions;
  lsp_positions.insert("a😀б");
  expect(lsp_positions.utf16Column(0, lsp_positions.text().size()) == 4, "UTF-16 counts surrogate pairs");
  expect(lsp_positions.byteColumn(0, 3) == 5, "UTF-16 column converts to UTF-8 byte offset");
  lsp_positions.applyReplacements({{{0, 1}, {0, 5}, "X"}});
  expect(lsp_positions.text() == "aXб", "range replacement handles UTF-8 byte offsets");
  expect(lsp_positions.undo(), "workspace replacement is one undo step");
  expect(lsp_positions.text() == "a😀б", "undo restores workspace replacement");
  expect(lsp_positions.redo() && lsp_positions.text() == "aXб"
      && lsp_positions.undo() && lsp_positions.text() == "a😀б",
    "workspace replacement redo and second undo preserve UTF-8 edit coordinates");

  std::vector<tuiide::PreparedReplacement> prepared_rename;
  std::string rename_error;
  expect(tuiide::prepareWorkspaceReplacements(lsp_positions,
    {{{0, 1}, {0, 3}, "icon"}}, prepared_rename, rename_error),
    "rename preview validates UTF-16 ranges");
  expect(prepared_rename.size() == 1 && prepared_rename[0].original == "😀"
    && prepared_rename[0].replacement.start.column == 1
    && prepared_rename[0].replacement.end.column == 5,
    "rename preview preserves original UTF-8 text and converts columns");
  expect(!tuiide::prepareWorkspaceReplacements(lsp_positions,
    {{{0, 2}, {0, 3}, "bad"}}, prepared_rename, rename_error)
    && rename_error == "invalid UTF-16 range" && prepared_rename.empty(),
    "rename rejects a position inside a UTF-16 surrogate pair");
  tuiide::Document overlap_document;
  overlap_document.setText("value");
  expect(!tuiide::prepareWorkspaceReplacements(overlap_document,
    {{{0, 0}, {0, 3}, "a"}, {{0, 2}, {0, 5}, "b"}}, prepared_rename, rename_error)
    && rename_error == "overlapping ranges", "rename rejects overlapping edits before applying any change");

  tuiide::Document selection;
  selection.setText("alpha\nbeta\ngamma");
  expect(selection.extractRange({0, 2}, {1, 2}) == "pha\nbe", "multiline selection is extracted");
  selection.replaceRange({0, 2}, {1, 2}, "X\nY");
  expect(selection.text() == "alX\nYta\ngamma", "multiline selection is replaced");
  expect(selection.cursor() == tuiide::Position{1, 1}, "cursor follows replacement text");
  expect(selection.undo() && selection.text() == "alpha\nbeta\ngamma", "selection replacement is one undo step");

  std::string search_error;
  auto search_matches = tuiide::searchText("Value value valuable\nПривет value", "value", "item",
    {.case_sensitive = false, .whole_word = true}, search_error);
  expect(search_error.empty() && search_matches.size() == 3,
    "case-insensitive whole-word search skips identifier prefixes");
  expect(search_matches[2].start == tuiide::Position{1, 13},
    "search reports UTF-8 byte columns on later lines");
  search_matches = tuiide::searchText("item-12 item-34", R"(item-(\d+))", "value-$1",
    {.case_sensitive = true, .regular_expression = true}, search_error);
  expect(search_matches.size() == 2 && search_matches[1].replacement == "value-34",
    "regular-expression search expands replacement capture groups");
  tuiide::Document replace_all_document;
  replace_all_document.setText("item-12 item-34");
  std::vector<tuiide::TextReplacement> search_replacements;
  for (const auto& match : search_matches)
    search_replacements.push_back({match.start, match.end, match.replacement});
  replace_all_document.applyReplacements(std::move(search_replacements));
  expect(replace_all_document.text() == "value-12 value-34" && replace_all_document.undo(),
    "replace all applies regex captures as one undoable document edit");
  search_matches = tuiide::searchText("abc", "[", "", {.regular_expression = true}, search_error);
  expect(search_matches.empty() && search_error.starts_with("Invalid regular expression"),
    "invalid regular expressions return an actionable error");
  search_matches = tuiide::searchText("abc", "missing", "", {}, search_error);
  expect(search_matches.empty() && search_error.empty(),
    "a valid search with no matches is distinct from a search error");
  search_matches = tuiide::searchText("abc", "", "", {}, search_error);
  expect(search_matches.empty() && search_error == "Search text is empty",
    "an empty search query is rejected with an actionable error");

  tuiide::Document utf8;
  utf8.insert("аб");
  utf8.backspace();
  expect(utf8.text() == "а", "backspace removes one UTF-8 code point");

  bool comment = false;
  auto tokens = tuiide::highlightCpp("const char* s = \"text\"; // note", comment);
  expect(tokens.size() >= 4, "C++ tokens detected");
  expect(tokens.back().kind == tuiide::TokenKind::Comment, "line comment detected");

  int cmake_bracket{-1};
  bool cmake_bracket_comment{};
  auto cmake_tokens = tuiide::highlightCMake("target_link_libraries(app PRIVATE ${CORE_LIBRARY}) # note",
    cmake_bracket, cmake_bracket_comment);
  expect(std::any_of(cmake_tokens.begin(), cmake_tokens.end(), [](const auto& token) {
    return token.kind == tuiide::TokenKind::Function;
  }), "CMake command is highlighted as a function");
  expect(std::any_of(cmake_tokens.begin(), cmake_tokens.end(), [](const auto& token) {
    return token.kind == tuiide::TokenKind::Variable;
  }), "CMake variable expansion is highlighted");
  cmake_tokens = tuiide::highlightCMake("#[=[ bracket comment", cmake_bracket, cmake_bracket_comment);
  expect(cmake_bracket == 1 && cmake_bracket_comment, "CMake bracket comment state crosses lines");
  cmake_tokens = tuiide::highlightCMake("still comment ]=] set(VALUE ON)", cmake_bracket, cmake_bracket_comment);
  expect(cmake_bracket == -1 && !cmake_bracket_comment && cmake_tokens.front().kind == tuiide::TokenKind::Comment,
    "CMake bracket comment closes with its matching delimiter");
  const auto cmake_completion = tuiide::completeCMake({"target_link_lib"}, 0, 15);
  expect(std::find(cmake_completion.begin(), cmake_completion.end(), "target_link_libraries") != cmake_completion.end(),
    "CMake completion filters standard commands by prefix");

  std::vector<std::string> large_source;
  large_source.reserve(50000);
  for (std::size_t line = 0; line < 50000; ++line)
    large_source.push_back("int value_" + std::to_string(line) + " = " + std::to_string(line) + ";");
  tuiide::CppSyntaxCache syntax_cache;
  expect(syntax_cache.update(large_source) == large_source.size(), "large syntax cache performs an initial full scan");
  large_source[25000] = "constexpr int changed = 42;";
  syntax_cache.invalidateFrom(25000);
  expect(syntax_cache.update(large_source) == 1, "single-line edit only re-highlights one line");
  large_source.insert(large_source.begin() + 100, "int inserted = 1;");
  syntax_cache.invalidateFrom(100);
  expect(syntax_cache.update(large_source) == 1, "inserted line reuses the shifted syntax-cache suffix");
  large_source[30000] = "/* begin comment";
  large_source[30010] = "end comment */";
  syntax_cache.invalidateFrom(30000);
  expect(syntax_cache.update(large_source) == 11, "block-comment edit re-highlights only until lexical state stabilizes");

  const auto session_path = std::filesystem::temp_directory_path()
      / ("tuiide-session-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
  tuiide::DebugSession saved_session{{{{"/tmp/source.cpp"}, 17, false, "counter > 3", 2, "counter reached"}}, {"counter", "items.size()"}, true,
    "app", "Debug", "dev", "dev-build", 34, 9};
  std::string session_error;
  expect(tuiide::saveDebugSession(session_path, saved_session, session_error), "debug session is saved");
  tuiide::DebugSession loaded_session;
  expect(tuiide::loadDebugSession(session_path, loaded_session, session_error), "debug session is loaded");
  expect(loaded_session.breakpoints.size() == 1 && loaded_session.breakpoints[0].line == 17
      && !loaded_session.breakpoints[0].enabled && loaded_session.breakpoints[0].condition == "counter > 3"
      && loaded_session.breakpoints[0].hit_count == 2 && loaded_session.breakpoints[0].log_message == "counter reached",
    "advanced breakpoint settings round trip");
  expect(loaded_session.watches == saved_session.watches && loaded_session.registers_enabled, "debug options round trip");
  expect(loaded_session.cmake_target == "app" && loaded_session.cmake_configuration == "Debug", "CMake selection round trip");
  expect(loaded_session.cmake_configure_preset == "dev", "CMake preset round trip");
  expect(loaded_session.cmake_build_preset == "dev-build", "CMake build preset round trip");
  expect(loaded_session.sidebar_width == 34 && loaded_session.lower_panel_height == 9,
    "saved panel dimensions round trip");
  { std::ofstream corrupted(session_path, std::ios::trunc); corrupted << "{broken"; }
  expect(!tuiide::loadDebugSession(session_path, loaded_session, session_error), "corrupted debug session is rejected");
  std::error_code cleanup_error;
  std::filesystem::remove(session_path, cleanup_error);

  tuiide::GdbClient debugger;
  expect(tuiide::gdbAttachCommand(42) == "-target-attach 42"
      && tuiide::gdbAttachCommand(0).empty() && tuiide::gdbAttachCommand(-7).empty(),
    "GDB attach command accepts only a positive PID");
  const auto fake_proc = std::filesystem::temp_directory_path()
    / ("tuiide-proc-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(fake_proc / "17");
  std::filesystem::create_directories(fake_proc / "3");
  std::filesystem::create_directories(fake_proc / "not-a-pid");
  {
    std::ofstream cmdline(fake_proc / "17/cmdline", std::ios::binary);
    std::string value{"demo"}; value.push_back('\0');
    value += "--name"; value.push_back('\n');
    value += "тест"; value.push_back('\0');
    cmdline.write(value.data(), static_cast<std::streamsize>(value.size()));
    std::ofstream comm(fake_proc / "3/comm"); comm << "worker\n";
    std::error_code link_error;
    std::filesystem::create_symlink("/usr/bin/demo", fake_proc / "17/exe", link_error);
  }
  const auto process_list = tuiide::debugProcesses(fake_proc);
  expect(process_list.size() == 2 && process_list[0].pid == 3
      && process_list[0].command == "worker" && process_list[1].pid == 17
      && process_list[1].command == "demo --name тест"
      && process_list[1].executable == "/usr/bin/demo",
    "procfs process discovery sorts PIDs and sanitizes control-separated UTF-8 arguments");
  std::error_code proc_cleanup_error;
  std::filesystem::remove_all(fake_proc, proc_cleanup_error);
  expect(tuiide::gdbSignalCommand("SIGUSR1") == "-interpreter-exec console \"signal SIGUSR1\""
      && tuiide::gdbSignalCommand("0") == "-interpreter-exec console \"signal 0\"",
    "GDB signal delivery supports named signals and suppression of a pending signal");
  expect(tuiide::gdbSignalCommand("SIGUSR1\nquit").empty()
      && tuiide::gdbSignalCommand("СИГНАЛ").empty()
      && tuiide::gdbSignalCommand("SIGKILL").empty(),
    "GDB rejects command injection, unknown UTF-8 names and uncatchable signals");
  expect(tuiide::gdbSignalPolicyCommand("SIGUSR1", true, true, false)
      == "-interpreter-exec console \"handle SIGUSR1 stop print nopass\""
      && tuiide::gdbSignalPolicyCommand("SIGUSR2", false, false, true)
        == "-interpreter-exec console \"handle SIGUSR2 nostop noprint pass\"",
    "GDB signal handling distinguishes stopping, reporting and delivery to the inferior");
  expect(tuiide::gdbSignalPolicyCommand("SIGINT", true, true, true).empty()
      && tuiide::gdbSignalPolicyCommand("SIGTRAP", false, false, false).empty()
      && tuiide::gdbSignalPolicyCommand("SIGUSR1", true, false, true).empty(),
    "GDB policies reject debugger-owned signals and contradictory stop/silent settings");
  expect(!debugger.sendSignal("SIGUSR1") && !debugger.inspectSignals()
      && !debugger.setSignalPolicy("SIGUSR1", true, true, true),
    "GDB signal operations require a stopped debug session");
  expect(tuiide::gdbEvaluateCommand("result + pair.right")
      == "-data-evaluate-expression \"result + pair.right\""
      && tuiide::gdbEvaluateCommand("name == \"value\"")
        == "-data-evaluate-expression \"name == \\\"value\\\"\"",
    "GDB evaluation command preserves spaces and escapes expression quotes");
  expect(tuiide::gdbDisassembleCommand("$pc", 96)
      == "-data-disassemble -s \"$pc\" -e \"($pc) + 96\" -- 0"
      && tuiide::gdbReadMemoryCommand("$sp + 8", 32)
        == "-data-read-memory-bytes \"$sp + 8\" 32",
    "GDB disassembly and memory commands retain their address before request state moves it");
  expect(!debugger.evaluate("1 + 1") && !debugger.assign("value", "2")
      && !debugger.disassemble("$pc") && !debugger.readMemory("$sp", 32),
    "GDB expression operations require a stopped debuggee");
  expect(debugger.addBreakpoint("/tmp/source.cpp", 17), "breakpoint can be restored");
  expect(!debugger.addBreakpoint("/tmp/source.cpp", 17), "restored breakpoint is deduplicated");
  expect(debugger.breakpoints().size() == 1 && debugger.breakpoints()[0].line == 17, "breakpoint state can be exported");
  auto configured_breakpoint = debugger.breakpoints()[0];
  configured_breakpoint.enabled = false; configured_breakpoint.condition = "value == 42";
  configured_breakpoint.hit_count = 3; configured_breakpoint.log_message = "hit value";
  expect(debugger.updateBreakpoint(configured_breakpoint)
      && !debugger.breakpoints()[0].enabled && debugger.breakpoints()[0].condition == "value == 42",
    "breakpoint properties can be updated before GDB starts");
  expect(debugger.removeBreakpoint("/tmp/source.cpp", 17) && debugger.breakpoints().empty(),
    "breakpoint can be removed through the structured API");
  expect(debugger.addBreakpoint("/tmp/old-project.cpp", 9)
      && debugger.addWatch("old_project_value"),
    "debug session can hold project-specific breakpoint and watch state");
  debugger.setRegistersEnabled(true);
  debugger.clearSessionState();
  expect(debugger.breakpoints().empty() && debugger.watches().empty()
      && !debugger.registersEnabled() && debugger.takeOutput().empty()
      && debugger.takeResults().empty(),
    "clearing a debug session removes all state owned by the previous project");

  const auto mi_stack = tuiide::parseMiRecord(
    R"(27^done,stack=[frame={level="0",func="compute",fullname="/tmp/a,b.cpp",line="7"},frame={level="1",func="main",line="12"}])");
  expect(mi_stack.valid() && mi_stack.token == 27 && mi_stack.prefix == '^' && mi_stack.klass == "done",
    "GDB/MI parser retains token and result record class");
  const auto* mi_frames = mi_stack.result("stack");
  expect(mi_frames && mi_frames->values.size() == 2 && mi_frames->names[0] == "frame"
      && mi_frames->values[0].string("fullname") == "/tmp/a,b.cpp"
      && mi_frames->values[1].string("func") == "main",
    "GDB/MI parser preserves nested result lists without splitting quoted commas");
  const auto mi_children = tuiide::parseMiRecord(
    R"(9^done,numchild="2",children=[child={name="var1.left",exp="left",numchild="0",value="42"},child={name="var1.text",exp="text",numchild="0",value="a\n\"b\""}])");
  const auto* children = mi_children.result("children");
  expect(mi_children.valid() && children && children->values.size() == 2
      && children->values[1].string("value") == "a\n\"b\"",
    "GDB/MI parser decodes escapes inside nested tuples");
  const auto mi_stream = tuiide::parseMiRecord(R"(~"UTF-8: \320\237\321\200\320\270\320\262\320\265\321\202\n")");
  expect(mi_stream.valid() && mi_stream.stream == "UTF-8: Привет\n",
    "GDB/MI stream parser decodes octal UTF-8 and newlines");
  const auto mi_error = tuiide::parseMiRecord(R"(31^error,msg="No symbol \"missing\"")");
  expect(mi_error.valid() && mi_error.klass == "error" && mi_error.string("msg") == "No symbol \"missing\"",
    "GDB/MI parser exposes structured command errors");
  expect(!tuiide::parseMiRecord("4^done,broken={").valid(),
    "GDB/MI parser rejects truncated aggregates deterministically");

  const auto cmake_build = std::filesystem::temp_directory_path()
      / ("tuiide-cmake-model-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  expect(tuiide::createCMakeFileApiQuery(cmake_build, session_error), "CMake File API query is created");
  expect(std::filesystem::exists(cmake_build / ".cmake/api/v1/query/codemodel-v2"), "codemodel query file exists");
  const auto reply = cmake_build / ".cmake/api/v1/reply";
  std::filesystem::create_directories(reply);
  { std::ofstream file(reply / "index-test.json"); file << R"({"reply":{"codemodel-v2":{"jsonFile":"model.json"}}})"; }
  { std::ofstream file(reply / "model.json"); file << R"({"configurations":[{"name":"Debug","targets":[{"name":"app","jsonFile":"app.json"},{"name":"core","jsonFile":"core.json"}]}]})"; }
  { std::ofstream file(reply / "app.json"); file
      << "{\"name\":\"app\",\"type\":\"EXECUTABLE\",\"artifacts\":[{\"path\":\"bin/app\"}],"
      << "\"paths\":{\"source\":\"" << cmake_build.string() << "\"},"
      << "\"sources\":[{\"path\":\"src/main.cpp\"},{\"path\":null}]}"; }
  { std::ofstream file(reply / "core.json"); file << R"({"name":"core","type":"STATIC_LIBRARY","artifacts":[{"path":"libcore.a"}]})"; }
  auto targets = tuiide::loadCMakeExecutableTargets(cmake_build, session_error);
  expect(targets.size() == 1 && targets[0].name == "app" && targets[0].configuration == "Debug", "executable CMake target is parsed");
  expect(targets[0].artifact == cmake_build / "bin/app", "target artifact is resolved against build directory");
  expect(targets[0].sources == std::vector<std::filesystem::path>{cmake_build / "src/main.cpp"},
    "CMake target sources are resolved for target-scoped analysis");
  std::filesystem::remove_all(cmake_build, cleanup_error);

  const auto cmake_edit = std::filesystem::temp_directory_path()
      / ("tuiide-cmake-edit-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(cmake_edit / "src");
  { std::ofstream file(cmake_edit / "src/main.cpp"); file << "int main() {}\n"; }
  { std::ofstream file(cmake_edit / "src/main.cpp.old"); file << "not a source\n"; }
  { std::ofstream file(cmake_edit / "CMakeLists.txt"); file
      << "add_executable(app src/main.cpp \"src/main.cpp\" src/main.cpp.old)\n"
         "# src/main.cpp must remain in this comment\n"
         "#[[\nsrc/main.cpp must remain in this bracket comment\n]]\n"
         "set(documentation [=[src/main.cpp]=])\n"; }
  tuiide::CMakeSourceRemoval source_removal;
  expect(tuiide::removeCMakeSourceReferences(cmake_edit, cmake_edit / "src/main.cpp", source_removal, session_error),
    "CMake source references are removed");
  expect(source_removal.references_removed == 2 && source_removal.changed_files.size() == 1,
    "all exact quoted and unquoted CMake references are reported");
  std::ifstream edited_cmake(cmake_edit / "CMakeLists.txt");
  const std::string edited_cmake_text((std::istreambuf_iterator<char>(edited_cmake)), std::istreambuf_iterator<char>());
  expect(edited_cmake_text.find("src/main.cpp.old") != std::string::npos,
    "CMake edit preserves paths with a common prefix");
  expect(edited_cmake_text.find("# src/main.cpp must remain") != std::string::npos,
    "CMake edit preserves comments");
  expect(edited_cmake_text.find("src/main.cpp must remain in this bracket comment") != std::string::npos
      && edited_cmake_text.find("[=[src/main.cpp]=]") != std::string::npos,
    "CMake edit preserves bracket comments and arguments");
  expect(!tuiide::removeCMakeSourceReferences(cmake_edit, cmake_edit.parent_path() / "outside.cpp", source_removal, session_error),
    "CMake edit rejects files outside the project root");
  std::filesystem::remove_all(cmake_edit, cleanup_error);

  const auto cmake_rename = std::filesystem::temp_directory_path()
      / ("tuiide-cmake-rename-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(cmake_rename / "src");
  { std::ofstream file(cmake_rename / "src/main.cpp"); file << "int main() {}\n"; }
  { std::ofstream file(cmake_rename / "src/lib.hpp"); file << "#pragma once\n"; }
  { std::ofstream file(cmake_rename / "shared.cpp"); file << "int shared;\n"; }
  { std::ofstream file(cmake_rename / "CMakeLists.txt"); file
      << "add_subdirectory(src)\n"
         "add_executable(app src/main.cpp \"src/lib.hpp\" src/main.cpp.old)\n"
         "# src/main.cpp must remain in this comment\n"
         "set(documentation [=[src/lib.hpp]=])\n"; }
  { std::ofstream file(cmake_rename / "src/CMakeLists.txt"); file
      << "target_sources(app PRIVATE main.cpp ../shared.cpp)\n"; }
  tuiide::CMakeSourceRename source_rename;
  expect(tuiide::moveProjectEntryWithCMake(cmake_rename, cmake_rename / "src",
      cmake_rename / "code", source_rename, session_error),
    "project directory move updates its CMake references");
  expect(std::filesystem::exists(cmake_rename / "code/main.cpp")
      && !std::filesystem::exists(cmake_rename / "src"),
    "project directory is moved on disk");
  std::ifstream renamed_root_cmake(cmake_rename / "CMakeLists.txt");
  const std::string renamed_root_text((std::istreambuf_iterator<char>(renamed_root_cmake)),
    std::istreambuf_iterator<char>());
  expect(renamed_root_text.find("add_subdirectory(code)") != std::string::npos
      && renamed_root_text.find("code/main.cpp") != std::string::npos
      && renamed_root_text.find("\"code/lib.hpp\"") != std::string::npos,
    "directory and descendant paths are replaced in parent CMake files");
  expect(renamed_root_text.find("src/main.cpp.old") != std::string::npos
      && renamed_root_text.find("# src/main.cpp must remain") != std::string::npos
      && renamed_root_text.find("[=[src/lib.hpp]=]") != std::string::npos,
    "CMake rename preserves prefix matches, comments, and bracket arguments");
  std::ifstream renamed_nested_cmake(cmake_rename / "code/CMakeLists.txt");
  const std::string renamed_nested_text((std::istreambuf_iterator<char>(renamed_nested_cmake)),
    std::istreambuf_iterator<char>());
  expect(renamed_nested_text.find("main.cpp ../shared.cpp") != std::string::npos,
    "paths relative to a moved CMake file remain stable");
  expect(source_rename.references_changed == 3 && source_rename.changed_files.size() == 1,
    "CMake-aware move reports exact changed references and files");
  std::filesystem::remove_all(cmake_rename, cleanup_error);

  const auto cmake_rollback = std::filesystem::temp_directory_path()
      / ("tuiide-cmake-rollback-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(cmake_rollback / "sub");
  { std::ofstream file(cmake_rollback / "asset.dat"); file << "data\n"; }
  { std::ofstream file(cmake_rollback / "CMakeLists.txt"); file << "set(asset asset.dat)\n"; }
  { std::ofstream file(cmake_rollback / "sub/CMakeLists.txt"); file << "set(asset ../asset.dat)\n"; }
  { std::ofstream file(cmake_rollback / "sub/CMakeLists.txt.tuiide.tmp"); file << "collision\n"; }
  expect(!tuiide::moveProjectEntryWithCMake(cmake_rollback, cmake_rollback / "asset.dat",
      cmake_rollback / "renamed.dat", source_rename, session_error),
    "project move fails when a CMake transaction cannot be written");
  expect(std::filesystem::exists(cmake_rollback / "asset.dat")
      && !std::filesystem::exists(cmake_rollback / "renamed.dat"),
    "failed CMake-aware move restores the filesystem entry");
  std::ifstream rollback_root_cmake(cmake_rollback / "CMakeLists.txt");
  std::ifstream rollback_nested_cmake(cmake_rollback / "sub/CMakeLists.txt");
  const std::string rollback_root_text((std::istreambuf_iterator<char>(rollback_root_cmake)),
    std::istreambuf_iterator<char>());
  const std::string rollback_nested_text((std::istreambuf_iterator<char>(rollback_nested_cmake)),
    std::istreambuf_iterator<char>());
  expect(rollback_root_text.find("asset.dat") != std::string::npos
      && rollback_root_text.find("renamed.dat") == std::string::npos
      && rollback_nested_text.find("../asset.dat") != std::string::npos,
    "failed CMake transaction leaves no partial reference edits");
  expect(session_error.find("restored") != std::string::npos,
    "failed CMake-aware move reports successful rollback");
  std::filesystem::remove_all(cmake_rollback, cleanup_error);

  const auto template_project = std::filesystem::temp_directory_path()
      / ("tuiide-template-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(template_project);
  std::filesystem::create_directories(template_project / "src");
  { std::ofstream file(template_project / "existing.cpp"); file << "int existing;\n"; }
  { std::ofstream file(template_project / "CMakeLists.txt"); file
      << "cmake_minimum_required(VERSION 3.20)\nproject(template_test)\nadd_library(core existing.cpp)\n"; }
  { std::ofstream file(template_project / "src/CMakeLists.txt"); file << "add_library(other INTERFACE)\n"; }
  tuiide::ProjectTemplateResult template_result;
  expect(tuiide::createProjectTemplate(template_project, tuiide::ProjectTemplate::CppClass,
      "src/sample_widget", "core", template_result, session_error), "C++ class template is created");
  expect(template_result.created_files.size() == 2
      && std::filesystem::exists(template_project / "src/sample_widget.hpp")
      && std::filesystem::exists(template_project / "src/sample_widget.cpp"),
    "C++ class template creates header and implementation in a subdirectory");
  std::ifstream generated_header(template_project / "src/sample_widget.hpp");
  const std::string generated_header_text((std::istreambuf_iterator<char>(generated_header)), std::istreambuf_iterator<char>());
  expect(generated_header_text.find("class SampleWidget") != std::string::npos,
    "C++ class name is derived from the requested file name");
  std::ifstream template_cmake(template_project / "CMakeLists.txt");
  const std::string template_cmake_text((std::istreambuf_iterator<char>(template_cmake)), std::istreambuf_iterator<char>());
  expect(template_cmake_text.find("src/sample_widget.hpp") != std::string::npos
      && template_cmake_text.find("src/sample_widget.cpp") != std::string::npos,
    "generated class files are added to the selected CMake target");
  std::ifstream nested_template_cmake(template_project / "src/CMakeLists.txt");
  const std::string nested_template_text((std::istreambuf_iterator<char>(nested_template_cmake)), std::istreambuf_iterator<char>());
  expect(nested_template_text.find("sample_widget") == std::string::npos,
    "preferred CMake target wins over an unrelated nearer target");
  const std::vector<std::pair<tuiide::ProjectTemplate, std::filesystem::path>> individual_templates{
    {tuiide::ProjectTemplate::CHeader, "include/c_api"},
    {tuiide::ProjectTemplate::CppHeader, "include/cpp_api"},
    {tuiide::ProjectTemplate::CSource, "src/c_module"},
    {tuiide::ProjectTemplate::CppSource, "src/cpp_module"}
  };
  for (const auto& [type, path] : individual_templates)
    expect(tuiide::createProjectTemplate(template_project, type, path, "core", template_result, session_error),
      "individual C/C++ project template is created");
  expect(std::filesystem::exists(template_project / "include/c_api.h")
      && std::filesystem::exists(template_project / "include/cpp_api.hpp")
      && std::filesystem::exists(template_project / "src/c_module.c")
      && std::filesystem::exists(template_project / "src/cpp_module.cpp"),
    "header and implementation templates apply their requested language extensions");
  tuiide::CppClassOptions class_options;
  class_options.class_name = "Model";
  class_options.header_file_name = "domain_model.hpp";
  class_options.source_file_name = "domain_model.cpp";
  class_options.namespace_name = "demo::domain";
  class_options.base_class = "Entity";
  class_options.base_header = "domain/entity.hpp";
  class_options.inheritance = tuiide::InheritanceAccess::Protected;
  class_options.header_path = "include/model.hpp";
  class_options.source_path = "src/model.cpp";
  class_options.final_class = true;
  class_options.generate_copy_operations = true;
  class_options.generate_move_operations = true;
  expect(tuiide::createCppClassTemplate(template_project, class_options, "core", template_result, session_error),
    "configured C++ class template is created");
  std::ifstream configured_header(template_project / "include/model.hpp");
  const std::string configured_header_text((std::istreambuf_iterator<char>(configured_header)), std::istreambuf_iterator<char>());
  expect(configured_header_text.find("namespace demo::domain") != std::string::npos
      && configured_header_text.find("#include \"domain/entity.hpp\"") != std::string::npos
      && configured_header_text.find("class Model final : protected Entity") != std::string::npos,
    "class template applies namespace, final, base class, and inheritance access");
  expect(configured_header_text.find("virtual ~Model()") != std::string::npos
      && configured_header_text.find("Model(Model&&) noexcept") != std::string::npos,
    "class template applies destructor and special-member settings");
  std::ifstream configured_source(template_project / "src/model.cpp");
  const std::string configured_source_text((std::istreambuf_iterator<char>(configured_source)), std::istreambuf_iterator<char>());
  expect(configured_source_text.find("Model::Model() = default") != std::string::npos
      && configured_source_text.find("Model::~Model() = default") != std::string::npos
      && configured_source_text.find("#include \"../include/model.hpp\"") != std::string::npos,
    "configured constructor and destructor definitions are generated");
  class_options.class_name = "bad-name";
  expect(!tuiide::validateCppClassSettings(class_options, session_error),
    "class settings reject invalid C++ identifiers before choosing paths");
  class_options.class_name = "Model";
  class_options.header_file_name = "../model.hpp";
  expect(!tuiide::validateCppClassSettings(class_options, session_error),
    "class settings reject a header file name containing a directory");
  class_options.header_file_name = "model.hpp";
  class_options.source_file_name = "model.c";
  expect(!tuiide::validateCppClassSettings(class_options, session_error),
    "class settings require a C++ implementation extension");
  expect(!tuiide::createProjectTemplate(template_project, tuiide::ProjectTemplate::CppClass,
      "src/sample_widget", "core", template_result, session_error), "templates never overwrite existing files");
  expect(!tuiide::createProjectTemplate(template_project, tuiide::ProjectTemplate::CppSource,
      "../outside", "core", template_result, session_error), "template paths cannot escape the project root");
  std::filesystem::remove_all(template_project, cleanup_error);

  const auto new_project_parent = std::filesystem::temp_directory_path()
      / ("tuiide-new-project-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto new_project_path = new_project_parent / "sample";
  const auto new_project_build = new_project_parent / "sample-build";
  tuiide::NewProjectOptions new_project;
  new_project.name = "sample_core";
  new_project.project_directory = new_project_path;
  new_project.build_directory = new_project_build;
  new_project.language = tuiide::ProjectLanguage::Cpp;
  new_project.target_type = tuiide::ProjectTargetType::StaticLibrary;
  new_project.language_standard = "23";
  new_project.cpp_header_extension = "h";
  new_project.generator = "Ninja";
  new_project.build_type = "Release";
  new_project.install_layout = tuiide::ProjectInstallLayout::Gnu;
  new_project.enable_testing = true;
  expect(tuiide::createNewProject(new_project, session_error), "new C++ project is generated");
  expect(std::filesystem::exists(new_project_path / "include/sample_core.h")
      && std::filesystem::exists(new_project_path / "src/sample_core.cpp"),
    "project wizard applies C++ header naming and library layout");
  std::ifstream new_project_cmake(new_project_path / "CMakeLists.txt");
  const std::string new_project_cmake_text((std::istreambuf_iterator<char>(new_project_cmake)), std::istreambuf_iterator<char>());
  expect(new_project_cmake_text.find("LANGUAGES CXX") != std::string::npos
      && new_project_cmake_text.find("CMAKE_CXX_STANDARD 23") != std::string::npos
      && new_project_cmake_text.find("add_library(sample_core STATIC") != std::string::npos
      && new_project_cmake_text.find("include(GNUInstallDirs)") != std::string::npos
      && new_project_cmake_text.find("install(TARGETS sample_core") != std::string::npos
      && new_project_cmake_text.find("install(DIRECTORY include/") != std::string::npos,
    "project wizard applies language, standard, and target type");
  expect(tuiide::loadProjectBuildDirectory(new_project_path) == std::filesystem::absolute(new_project_build),
    "custom build directory is persisted for reopening");
  tuiide::ProjectSettings generated_settings;
  expect(tuiide::loadProjectSettings(new_project_path, generated_settings, session_error)
      && generated_settings.generator == "Ninja"
      && generated_settings.build_type == "Release"
      && generated_settings.cpp_standard == "23"
      && generated_settings.cpp_header_extension == "h",
    "project wizard persists generator, build type, language standard, and header style");
  expect(!tuiide::createNewProject(new_project, session_error), "project wizard refuses a non-empty project directory");

  tuiide::NewProjectOptions c_project;
  c_project.name = "sample_c";
  c_project.project_directory = new_project_parent / "sample-c";
  c_project.build_directory = new_project_parent / "sample-c-build";
  c_project.language = tuiide::ProjectLanguage::C;
  c_project.target_type = tuiide::ProjectTargetType::Executable;
  c_project.language_standard = "17";
  expect(tuiide::createNewProject(c_project, session_error), "new C project is generated");
  expect(std::filesystem::exists(c_project.project_directory / "src/main.c"),
    "C project wizard generates a C source file");
  std::ifstream c_project_cmake(c_project.project_directory / "CMakeLists.txt");
  const std::string c_project_cmake_text((std::istreambuf_iterator<char>(c_project_cmake)), std::istreambuf_iterator<char>());
  expect(c_project_cmake_text.find("LANGUAGES C") != std::string::npos
      && c_project_cmake_text.find("CMAKE_C_STANDARD 17") != std::string::npos
      && c_project_cmake_text.find("add_executable(sample_c") != std::string::npos,
    "C project wizard applies the selected language, standard, and executable target");
  auto invalid_project = c_project;
  invalid_project.project_directory = new_project_parent / "invalid";
  invalid_project.build_directory = new_project_parent / "invalid-build";
  invalid_project.generator = "Unknown generator";
  expect(!tuiide::createNewProject(invalid_project, session_error),
    "project wizard rejects unsupported generator values");
  std::filesystem::remove_all(new_project_parent, cleanup_error);

  const auto settings_project = std::filesystem::temp_directory_path()
      / ("tuiide-settings-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(settings_project / "cmake");
  auto project_settings = tuiide::defaultProjectSettings(settings_project);
  expect(project_settings.build_jobs == std::clamp(std::thread::hardware_concurrency(), 1U, 1024U),
    "parallel jobs default to the host hardware thread count");
  project_settings.build_directory = settings_project / "out/debug";
  project_settings.generator = "Ninja";
  project_settings.kit = "GCC native";
  project_settings.toolchain = settings_project / "cmake/toolchain.cmake";
  project_settings.make_program = "/usr/bin/ninja";
  project_settings.sysroot = "/opt/sdk";
  project_settings.c_compiler = "/usr/bin/cc";
  project_settings.cpp_compiler = "/usr/bin/c++";
  project_settings.c_standard = "17";
  project_settings.cpp_standard = "23";
  project_settings.cpp_header_extension = "h";
  project_settings.build_type = "RelWithDebInfo";
  project_settings.build_jobs = 3;
  project_settings.tab_width = 4;
  project_settings.use_spaces = false;
  project_settings.environment = {{"APP_MODE", "тест"}, {"TRACE", "1"}};
  project_settings.clangd_arguments = {"--header-insertion=never", "--query-driver=/opt/tool chain/*"};
  project_settings.shortcuts = {{"run.build", "Ctrl+B"}, {"search.find", "Alt+K"}};
  project_settings.theme = "Team dark";
  project_settings.custom_themes = {{"Team dark", "Dark"}};
  project_settings.colors = {{"diagnosticError", "LightRed"}, {"keyword", "Yellow"},
    {"executionLineBackground", "Green"}};
  project_settings.launch.executable = settings_project / "bin/custom app";
  project_settings.launch.target = "cmake_app";
  project_settings.launch.working_directory = settings_project / "run";
  project_settings.launch.arguments = {"--mode", "тестовый режим"};
  project_settings.launch.environment = {{"LAUNCH_MODE", "проверка"}};
  project_settings.launch.stdin_file = settings_project / "input data.txt";
  project_settings.launch.pre_launch_build = true;
  project_settings.launch.external_terminal = true;
  project_settings.launch.terminal = "test-terminal";
  { std::ofstream gitignore(settings_project / ".gitignore"); gitignore << "*.user-cache\n"; }
  expect(tuiide::saveProjectSettings(settings_project, project_settings, session_error),
    "versioned project settings are saved atomically");
  expect(tuiide::updateProjectGitignore(settings_project, project_settings, session_error),
    "project settings add a managed gitignore block");
  tuiide::ProjectSettings loaded_settings;
  expect(tuiide::loadProjectSettings(settings_project, loaded_settings, session_error)
      && loaded_settings.build_directory == tuiide::normalizePath(settings_project / "out/debug")
      && loaded_settings.toolchain == tuiide::normalizePath(settings_project / "cmake/toolchain.cmake")
      && loaded_settings.kit == "GCC native"
      && loaded_settings.make_program == "/usr/bin/ninja"
      && loaded_settings.sysroot == "/opt/sdk"
      && loaded_settings.build_jobs == 3
      && loaded_settings.tab_width == 4 && !loaded_settings.use_spaces
      && loaded_settings.environment == project_settings.environment
      && loaded_settings.clangd_arguments == project_settings.clangd_arguments
      && loaded_settings.cpp_header_extension == project_settings.cpp_header_extension
      && loaded_settings.shortcuts == project_settings.shortcuts
      && loaded_settings.theme == project_settings.theme
      && loaded_settings.custom_themes == project_settings.custom_themes
      && tuiide::effectiveEditorTheme(loaded_settings) == "Dark"
      && loaded_settings.colors == project_settings.colors
      && loaded_settings.launch_configurations.size() == 1
      && loaded_settings.active_launch_configuration == "Default"
      && loaded_settings.launch_configurations.front().configuration.arguments
        == project_settings.launch.arguments
      && loaded_settings.launch.executable == tuiide::normalizePath(settings_project / "bin/custom app")
      && loaded_settings.launch.target == "cmake_app"
      && loaded_settings.launch.working_directory == tuiide::normalizePath(settings_project / "run")
      && loaded_settings.launch.arguments == project_settings.launch.arguments
      && loaded_settings.launch.environment == project_settings.launch.environment
      && loaded_settings.launch.stdin_file == tuiide::normalizePath(settings_project / "input data.txt")
      && loaded_settings.launch.pre_launch_build && loaded_settings.launch.external_terminal
      && loaded_settings.launch.terminal == "test-terminal",
    "project settings preserve shortcuts and all UTF-8 launch configuration fields");
  { std::ofstream cmake_file(settings_project / "CMakeLists.txt"); cmake_file << "project(settings_test)\n"; }
  tuiide::ProjectSession project_session;
  tuiide::ProjectOpenResult project_open_result;
  expect(project_session.open(settings_project, {}, project_open_result, session_error)
      && !project_open_result.used_default_settings
      && project_session.root() == tuiide::normalizePath(settings_project)
      && project_session.buildDirectory() == loaded_settings.build_directory
      && project_session.sessionFile() == loaded_settings.build_directory / ".tuiide-session.json"
      && project_session.recoveryFile() == loaded_settings.build_directory / ".tuiide-recovery.json",
    "project session owns normalized identity, settings, and derived state paths");
  auto session_settings = project_session.settings();
  session_settings.build_directory = settings_project / "out/alternate";
  project_session.applySettings(session_settings);
  expect(project_session.buildDirectory() == session_settings.build_directory
      && project_session.sessionFile().parent_path() == session_settings.build_directory,
    "applying project settings updates all derived session paths together");
  project_session.close();
  expect(!project_session.open() && project_session.root().empty()
      && project_session.buildDirectory().empty() && project_session.sessionFile().empty()
      && project_session.recoveryFile().empty()
      && project_session.settings().build_directory.empty()
      && project_session.settings().launch.target.empty()
      && project_session.settings().shortcuts.empty()
      && project_session.settings().colors.empty(),
    "closing a project session clears paths, launch settings, shortcuts, and colors");

  const auto fallback_project = settings_project / "fallback";
  std::filesystem::create_directories(fallback_project);
  { std::ofstream cmake_file(fallback_project / "CMakeLists.txt"); cmake_file << "project(fallback)\n"; }
  { std::ofstream settings_file(fallback_project / ".tuiide-project.json"); settings_file << "{invalid"; }
  expect(project_session.open(fallback_project, settings_project / "fallback-build",
      project_open_result, session_error)
      && project_open_result.used_default_settings && !project_open_result.warning.empty()
      && project_session.buildDirectory() == tuiide::normalizePath(settings_project / "fallback-build"),
    "project session reports invalid or missing settings and accepts an explicit build-directory override");
  std::ifstream settings_json(settings_project / ".tuiide-project.json");
  const std::string settings_text((std::istreambuf_iterator<char>(settings_json)), std::istreambuf_iterator<char>());
  expect(settings_text.find("\"version\": 1") != std::string::npos
      && settings_text.find("\"buildDirectory\": \"out/debug\"") != std::string::npos,
    "project settings file is versioned and keeps in-project paths portable");

  const auto user_settings_path = settings_project / "config/tuiide/settings.json";
  const auto recent_first = settings_project / "recent-first.cpp";
  const auto recent_second = settings_project / "recent-second.hpp";
  { std::ofstream file(recent_first); file << "int recent_first;\n"; }
  { std::ofstream file(recent_second); file << "#pragma once\n"; }
  const auto missing_recent = settings_project / "removed.cpp";
  const auto normalized_recent = tuiide::normalizeRecentFiles(
    {recent_first, recent_first, missing_recent, recent_second});
  expect(normalized_recent == std::vector<std::filesystem::path>{
      tuiide::normalizePath(recent_first), tuiide::normalizePath(recent_second)},
    "recent files are normalized, deduplicated, and stripped of unavailable paths");
  auto ordered_recent = normalized_recent;
  tuiide::rememberRecentFile(ordered_recent, recent_second);
  expect(ordered_recent == std::vector<std::filesystem::path>{
      tuiide::normalizePath(recent_second), tuiide::normalizePath(recent_first)},
    "opening a recent file moves it to the front without duplicating it");
  tuiide::rememberRecentFile(ordered_recent, recent_first, 1);
  expect(ordered_recent == std::vector<std::filesystem::path>{tuiide::normalizePath(recent_first)}
      && tuiide::normalizeRecentFiles(ordered_recent, 0).empty(),
    "recent file history obeys its configured size limit, including zero");
  tuiide::UserSettings user_settings;
  user_settings.shortcuts = {{"file.open", "Ctrl+B"}};
  user_settings.theme = "Team dark";
  user_settings.custom_themes = {{"Team dark", "Dark"}};
  user_settings.colors = {{"keyword", "Yellow"}, {"diagnosticError", "LightRed"},
    {"executionLineBackground", "Green"}};
  user_settings.recent_files = {recent_first, recent_first, missing_recent, recent_second};
  expect(tuiide::saveUserSettings(user_settings_path, user_settings, session_error),
    "user settings create their configuration directory and save atomically");
  tuiide::UserSettings loaded_user_settings;
  expect(tuiide::loadUserSettings(user_settings_path, loaded_user_settings, session_error)
      && loaded_user_settings.shortcuts == user_settings.shortcuts
      && loaded_user_settings.theme == user_settings.theme
      && loaded_user_settings.custom_themes == user_settings.custom_themes
      && loaded_user_settings.colors == user_settings.colors
      && loaded_user_settings.recent_files == normalized_recent
      && tuiide::effectiveEditorTheme(loaded_user_settings) == "Dark",
    "user shortcuts, theme, colors, and filtered recent files round-trip independently of a project");
  const auto malformed_recent_path = settings_project / "config/tuiide/malformed-recent.json";
  {
    std::ofstream output(malformed_recent_path);
    output << "{\"version\":1,\"recentFiles\":[null,{},7,\""
      << recent_first.string() << "\"]}";
  }
  tuiide::UserSettings malformed_recent_settings;
  expect(tuiide::loadUserSettings(malformed_recent_path, malformed_recent_settings, session_error)
      && malformed_recent_settings.recent_files
        == std::vector<std::filesystem::path>{tuiide::normalizePath(recent_first)},
    "malformed recent-file entries are ignored without rejecting otherwise valid settings");
  const auto saved_user_text = [&] {
    std::ifstream input(user_settings_path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  }();
  expect(tuiide::reservedShortcutReason("  ctrl + l \t").find("Final Cut") != std::string::npos
      && !tuiide::reservedShortcutReason("alt+w").empty()
      && !tuiide::reservedShortcutReason("F10").empty()
      && !tuiide::reservedShortcutReason("Alt+K").empty()
      && tuiide::reservedShortcutReason("Alt+Shift+U").empty()
      && tuiide::reservedShortcutReason("").empty(),
    "reserved shortcut policy normalizes case and whitespace without rejecting free or disabled bindings");
  const auto legacy_shortcuts_path = settings_project / "legacy-shortcuts.json";
  {
    std::ofstream file(legacy_shortcuts_path);
    file << "{\"version\":1,\"theme\":\"Light\",\"colors\":{\"keyword\":\"Yellow\"},"
      "\"shortcuts\":{\"debug.watch\":\" ctrl + l \",\"file.open\":\"Ctrl+B\","
      "\"file.new\":\"\",\"search.find\":\"Alt+W\",\"команда\":\"F10\"}}\n";
  }
  std::vector<std::string> shortcut_warnings;
  tuiide::UserSettings legacy_shortcuts;
  expect(tuiide::loadUserSettings(legacy_shortcuts_path, legacy_shortcuts, session_error, &shortcut_warnings)
      && session_error.empty() && shortcut_warnings.size() == 3
      && legacy_shortcuts.shortcuts.size() == 2
      && legacy_shortcuts.shortcuts.at("file.open") == "Ctrl+B"
      && legacy_shortcuts.shortcuts.at("file.new").empty()
      && legacy_shortcuts.theme == "Light" && legacy_shortcuts.colors.at("keyword") == "Yellow",
    "legacy reserved bindings are ignored with diagnostics while valid shortcuts and appearance survive");
  expect(tuiide::loadUserSettings(user_settings_path, loaded_user_settings, session_error, &shortcut_warnings)
      && shortcut_warnings.empty(), "loading clean settings clears stale shortcut warnings");
  auto reserved_settings = user_settings;
  reserved_settings.shortcuts["debug.watch"] = "Ctrl+L";
  expect(!tuiide::saveUserSettings(user_settings_path, reserved_settings, session_error)
      && session_error.find("Final Cut") != std::string::npos,
    "reserved shortcut updates are rejected before the installed settings file is overwritten");
  auto invalid_user_settings = user_settings;
  invalid_user_settings.colors["keyword"] = "Invisible";
  expect(!tuiide::saveUserSettings(user_settings_path, invalid_user_settings, session_error),
    "invalid user color settings are rejected");
  std::ifstream unchanged_user_file(user_settings_path, std::ios::binary);
  const std::string unchanged_user_text((std::istreambuf_iterator<char>(unchanged_user_file)),
    std::istreambuf_iterator<char>());
  expect(unchanged_user_text == saved_user_text,
    "a failed user settings update leaves the installed file unchanged");
  std::ifstream managed_gitignore(settings_project / ".gitignore");
  std::string managed_gitignore_text((std::istreambuf_iterator<char>(managed_gitignore)), std::istreambuf_iterator<char>());
  expect(managed_gitignore_text.find("*.user-cache") != std::string::npos
      && managed_gitignore_text.find("# BEGIN TUI IDE\n.tuiide-project.json\n/out/debug/\n# END TUI IDE") != std::string::npos,
    "managed gitignore block preserves user rules and ignores an in-project build directory");
  project_settings.build_directory = settings_project.parent_path() / "external-settings-build";
  expect(tuiide::updateProjectGitignore(settings_project, project_settings, session_error),
    "managed gitignore block is updated when the build directory changes");
  std::ifstream updated_gitignore(settings_project / ".gitignore");
  managed_gitignore_text.assign(std::istreambuf_iterator<char>(updated_gitignore), std::istreambuf_iterator<char>());
  expect(managed_gitignore_text.find("*.user-cache") != std::string::npos
      && managed_gitignore_text.find("/out/debug/") == std::string::npos
      && managed_gitignore_text.find(".tuiide-project.json") != std::string::npos,
    "external build directory removes the stale project-local ignore rule");
  std::map<std::string, std::string> parsed_environment;
  expect(tuiide::parseEnvironmentSettings("ONE=1; MESSAGE=hello world", parsed_environment, session_error)
      && parsed_environment["MESSAGE"] == "hello world",
    "project environment parser accepts semicolon-separated values");
  expect(!tuiide::parseEnvironmentSettings("BAD-NAME=value", parsed_environment, session_error),
    "project environment parser rejects invalid variable names");
  std::vector<std::string> parsed_arguments;
  expect(tuiide::parseArgumentList("--flag 'value with spaces' \"quoted\"", parsed_arguments, session_error)
      && parsed_arguments == std::vector<std::string>{"--flag", "value with spaces", "quoted"},
    "clangd argument parser handles shell-style quoting without invoking a shell");
  expect(!tuiide::parseArgumentList("'unfinished", parsed_arguments, session_error),
    "clangd argument parser rejects unterminated quotes");
  std::filesystem::create_directories(settings_project / "bin");
  std::filesystem::create_directories(settings_project / "run");
  { std::ofstream stdin_file(settings_project / "input data.txt"); stdin_file << "input\n"; }
  { std::ofstream executable_file(settings_project / "bin/custom app"); executable_file << "#!/bin/sh\nexit 0\n"; }
  std::filesystem::permissions(settings_project / "bin/custom app", std::filesystem::perms::owner_all);
  tuiide::CMakeTarget launch_target{"cmake_app", "Debug", settings_project / "bin/cmake-app"};
  { std::ofstream executable_file(launch_target.artifact); executable_file << "#!/bin/sh\nexit 0\n"; }
  std::filesystem::permissions(launch_target.artifact, std::filesystem::perms::owner_all);
  tuiide::LaunchCommand launch_command;
  expect(tuiide::resolveLaunchCommand(settings_project, loaded_settings.launch, &launch_target,
      launch_command, session_error)
      && launch_command.explicit_executable
      && launch_command.executable == tuiide::normalizePath(settings_project / "bin/custom app")
      && launch_command.working_directory == tuiide::normalizePath(settings_project / "run")
      && launch_command.arguments == std::vector<std::string>{"--mode", "тестовый режим"}
      && launch_command.environment == project_settings.launch.environment
      && launch_command.stdin_file == tuiide::normalizePath(settings_project / "input data.txt")
      && launch_command.pre_launch_build && launch_command.external_terminal,
    "explicit launch executable and advanced settings override the CMake target");
  const auto external_arguments = tuiide::launchProcessArguments(launch_command);
  expect(external_arguments == std::vector<std::string>{"test-terminal", "-e", "sh", "-c",
      "input=$1; shift; exec \"$@\" < \"$input\"", "tuiide-launch",
      tuiide::normalizePath(settings_project / "input data.txt").string(),
      tuiide::normalizePath(settings_project / "bin/custom app").string(), "--mode", "тестовый режим"},
    "external terminal launch preserves executable arguments and safely redirects stdin");
  const auto integrated_arguments = tuiide::integratedLaunchArguments(launch_command);
  expect(integrated_arguments == std::vector<std::string>{"sh", "-c",
      "input=$1; shift; exec \"$@\" < \"$input\"", "tuiide-launch",
      tuiide::normalizePath(settings_project / "input data.txt").string(),
      tuiide::normalizePath(settings_project / "bin/custom app").string(), "--mode", "тестовый режим"},
    "integrated PTY launch redirects configured stdin without shell-interpolating paths or arguments");
  tuiide::LaunchConfiguration target_launch;
  target_launch.arguments = {"--from-target"};
  expect(tuiide::resolveLaunchCommand(settings_project, target_launch, &launch_target,
      launch_command, session_error)
      && !launch_command.explicit_executable && launch_command.executable == launch_target.artifact
      && launch_command.working_directory == tuiide::normalizePath(settings_project)
      && tuiide::launchProcessArguments(launch_command)
        == std::vector<std::string>{launch_target.artifact.string(), "--from-target"},
    "selected CMake File API target is used when no explicit executable is configured");
  expect(!tuiide::resolveLaunchCommand(settings_project, target_launch, nullptr,
      launch_command, session_error)
      && session_error.find("no CMake executable target") != std::string::npos,
    "launch never falls back to scanning arbitrary executables in the build tree");
  target_launch.executable = settings_project / "bin/missing";
  expect(!tuiide::resolveLaunchCommand(settings_project, target_launch, &launch_target,
      launch_command, session_error)
      && session_error.find("does not exist") != std::string::npos,
    "missing explicit executable reports an actionable launch error");
  auto named_settings = loaded_settings;
  tuiide::LaunchConfiguration tests_launch;
  tests_launch.target = "cmake_app";
  tests_launch.arguments = {"--suite", "unit"};
  named_settings.launch_configurations.push_back({"Tests", tests_launch});
  expect(tuiide::selectLaunchConfiguration(named_settings, "Tests")
      && named_settings.launch.arguments == std::vector<std::string>{"--suite", "unit"}
      && named_settings.active_launch_configuration == "Tests",
    "a named launch configuration can be selected as the shared Run/Debug profile");
  named_settings.launch.arguments.push_back("--verbose");
  tuiide::synchronizeActiveLaunchConfiguration(named_settings);
  expect(named_settings.launch_configurations.back().configuration.arguments
      == std::vector<std::string>{"--suite", "unit", "--verbose"}
      && !tuiide::selectLaunchConfiguration(named_settings, "Missing"),
    "active launch edits synchronize without accepting an unknown profile");
  expect(tuiide::saveProjectSettings(settings_project, named_settings, session_error),
    "multiple named launch configurations are saved atomically");
  tuiide::ProjectSettings reloaded_named_settings;
  expect(tuiide::loadProjectSettings(settings_project, reloaded_named_settings, session_error)
      && reloaded_named_settings.launch_configurations.size() == 2
      && reloaded_named_settings.active_launch_configuration == "Tests"
      && reloaded_named_settings.launch.arguments
        == std::vector<std::string>{"--suite", "unit", "--verbose"},
    "named launch configurations and active selection round-trip");
  auto invalid_launch_settings = named_settings;
  invalid_launch_settings.launch_configurations.push_back({"Tests", {}});
  expect(!tuiide::validateProjectSettings(settings_project, invalid_launch_settings, session_error)
      && session_error.find("Duplicate") != std::string::npos,
    "duplicate launch configuration names are rejected");
  const auto legacy_launch_project = settings_project / "legacy-launch";
  std::filesystem::create_directories(legacy_launch_project);
  {
    std::ofstream legacy(legacy_launch_project / ".tuiide-project.json");
    legacy << "{\"version\":1,\"buildDirectory\":\"build\","
      "\"launch\":{\"arguments\":[\"--legacy\"]}}";
  }
  tuiide::ProjectSettings migrated_launch_settings;
  expect(tuiide::loadProjectSettings(legacy_launch_project, migrated_launch_settings, session_error)
      && migrated_launch_settings.active_launch_configuration == "Default"
      && migrated_launch_settings.launch_configurations.size() == 1
      && migrated_launch_settings.launch.arguments == std::vector<std::string>{"--legacy"},
    "legacy single launch settings migrate to the Default named configuration");
  auto invalid_theme = project_settings;
  invalid_theme.theme = "Invisible";
  expect(!tuiide::validateProjectSettings(settings_project, invalid_theme, session_error),
    "unknown editor themes are rejected");
  invalid_theme = project_settings;
  invalid_theme.custom_themes["Broken"] = "Invisible";
  expect(!tuiide::validateProjectSettings(settings_project, invalid_theme, session_error),
    "custom editor themes require a supported base palette");
  invalid_theme = project_settings;
  invalid_theme.colors["keyword"] = "Invisible";
  expect(!tuiide::validateProjectSettings(settings_project, invalid_theme, session_error),
    "unknown editor colors are rejected");
  { std::ofstream corrupt(settings_project / ".tuiide-project.json"); corrupt << "{invalid"; }
  expect(!tuiide::loadProjectSettings(settings_project, loaded_settings, session_error)
      && session_error.find("Cannot parse") != std::string::npos,
    "corrupt project settings produce an actionable error");
  std::filesystem::remove_all(settings_project, cleanup_error);

  const auto import_parent = std::filesystem::temp_directory_path()
      / ("tuiide-import-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto import_project = import_parent / "legacy sources";
  const auto import_build = import_parent / "legacy-build";
  std::filesystem::create_directories(import_project / "src");
  std::filesystem::create_directories(import_project / "include");
  std::filesystem::create_directories(import_project / ".git");
  { std::ofstream file(import_project / "src/main.cpp"); file << "int main() { return 0; }\n"; }
  { std::ofstream file(import_project / "src/compat.c"); file << "int compat(void) { return 1; }\n"; }
  { std::ofstream file(import_project / "include/demo.hpp"); file << "#pragma once\n"; }
  { std::ofstream file(import_project / ".git/ignored.cpp"); file << "invalid\n"; }
  tuiide::ProjectImportOptions import_options;
  import_options.project_directory = import_project;
  import_options.build_directory = import_build;
  import_options.target_name = "legacy_app";
  import_options.language_standard = "20";
  tuiide::ProjectImportPlan import_plan;
  expect(tuiide::planProjectImport(import_options, import_plan, session_error),
    "existing C/C++ source directory can be planned for import");
  expect(import_plan.c_sources == 1 && import_plan.cpp_sources == 1 && import_plan.headers == 1
      && import_plan.files.size() == 3,
    "import recursively discovers implementation and header files while ignoring VCS metadata");
  expect(import_plan.cmake_text.find("LANGUAGES C CXX") != std::string::npos
      && import_plan.cmake_text.find("\"src/main.cpp\"") != std::string::npos
      && import_plan.cmake_text.find("target_include_directories(legacy_app PRIVATE include)") != std::string::npos,
    "import preview generates a mixed-language CMake target with quoted source paths");
  { std::ofstream file(import_project / "src/late.cpp"); file << "void late() {}\n"; }
  expect(!tuiide::createImportedProject(import_options, import_plan, session_error)
      && session_error.find("changed after") != std::string::npos,
    "import refuses to apply when source contents changed after preview");
  expect(tuiide::planProjectImport(import_options, import_plan, session_error)
      && tuiide::createImportedProject(import_options, import_plan, session_error),
    "confirmed import creates project files");
  expect(std::filesystem::exists(import_project / "CMakeLists.txt")
      && std::filesystem::exists(import_project / ".tuiide-project.json")
      && tuiide::loadProjectBuildDirectory(import_project) == std::filesystem::absolute(import_build),
    "import persists generated CMake and the selected build directory");
  expect(!tuiide::createImportedProject(import_options, import_plan, session_error),
    "import never overwrites an existing CMakeLists.txt");

  const auto cpp_only_project = import_parent / "cpp-only";
  std::filesystem::create_directories(cpp_only_project);
  { std::ofstream file(cpp_only_project / "main.cpp"); file << "int main() { return 0; }\n"; }
  tuiide::ProjectImportOptions c_import_options;
  c_import_options.project_directory = cpp_only_project;
  c_import_options.build_directory = import_parent / "cpp-only-build";
  c_import_options.target_name = "c_only";
  c_import_options.language = tuiide::ProjectLanguage::C;
  c_import_options.language_standard = "17";
  expect(!tuiide::planProjectImport(c_import_options, import_plan, session_error)
      && session_error.find("No compatible") != std::string::npos,
    "C import rejects a directory that contains only C++ implementations");
  std::filesystem::remove_all(import_parent, cleanup_error);

  const auto preset_source = std::filesystem::temp_directory_path()
      / ("tuiide-cmake-presets-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(preset_source);
  { std::ofstream file(preset_source / "base.json"); file << R"({
    "version": 6,
    "configurePresets": [{"name":"base","hidden":true,"generator":"Unix Makefiles","binaryDir":"${sourceDir}/out/${presetName}",
      "condition":{"type":"equals","lhs":"${hostSystemName}","rhs":"Linux"}}],
    "buildPresets": [{"name":"build-base","hidden":true,"cleanFirst":true}]
  })"; }
  { std::ofstream file(preset_source / "CMakePresets.json"); file << R"({
    "version": 6,
    "include": ["base.json"],
    "configurePresets": [
      {"name":"dev","displayName":"Development","inherits":"base"},
      {"name":"disabled","inherits":"base","condition":{"type":"equals","lhs":"${hostSystemName}","rhs":"Windows"}}
    ],
    "buildPresets": [
      {"name":"dev-build","displayName":"Build Development","inherits":"build-base","configurePreset":"dev",
       "configuration":"Debug","targets":"${presetName}"},
      {"name":"disabled-build","configurePreset":"dev","condition":false}
    ]
  })"; }
  { std::ofstream file(preset_source / "CMakeUserPresets.json"); file << R"({
    "version": 6,
    "configurePresets": [{"name":"user","inherits":"dev","binaryDir":"relative-build"}],
    "buildPresets": [{"name":"user-build","inherits":"dev-build","configurePreset":"user","targets":["app"],"verbose":true}]
  })"; }
  auto presets = tuiide::loadCMakeConfigurePresets(preset_source, session_error);
  expect(session_error.empty() && presets.size() == 2, "visible configure presets are loaded across includes");
  const auto dev_preset = std::find_if(presets.begin(), presets.end(), [](const auto& item) { return item.name == "dev"; });
  expect(dev_preset != presets.end() && dev_preset->display_name == "Development", "preset display name is retained");
  expect(dev_preset->generator == "Unix Makefiles", "preset generator is inherited");
  expect(dev_preset->binary_directory == preset_source / "out/dev", "preset macros use the child preset name");
  const auto user_preset = std::find_if(presets.begin(), presets.end(), [](const auto& item) { return item.name == "user"; });
  expect(user_preset != presets.end() && user_preset->binary_directory == preset_source / "relative-build",
    "relative preset binary directory is resolved against source");
  auto build_presets = tuiide::loadCMakeBuildPresets(preset_source, session_error);
  expect(session_error.empty() && build_presets.size() == 2, "visible build presets are loaded and conditioned");
  const auto dev_build = std::find_if(build_presets.begin(), build_presets.end(), [](const auto& item) {
    return item.name == "dev-build";
  });
  expect(dev_build != build_presets.end() && dev_build->configure_preset == "dev"
    && dev_build->configuration == "Debug", "build preset configuration is parsed");
  expect(dev_build->clean_first && dev_build->targets == std::vector<std::string>{"dev-build"},
    "build preset fields and macros are inherited");
  const auto user_build = std::find_if(build_presets.begin(), build_presets.end(), [](const auto& item) {
    return item.name == "user-build";
  });
  expect(user_build != build_presets.end() && user_build->verbose && user_build->targets == std::vector<std::string>{"app"},
    "user build preset overrides inherited fields");
  expect(dev_preset != presets.end() && !dev_preset->user_editable
      && dev_preset->source_file.filename() == "CMakePresets.json"
      && user_preset != presets.end() && user_preset->user_editable
      && user_preset->source_file.filename() == "CMakeUserPresets.json",
    "loaded presets retain their source and user editability");

  auto editable_presets = tuiide::loadCMakePresetsForEdit(
    preset_source, tuiide::CMakePresetKind::Configure, session_error);
  expect(session_error.empty() && editable_presets.size() == 4,
    "preset manager loads visible, hidden, and conditioned configure presets");
  tuiide::CMakePresetEdit editable_user;
  expect(tuiide::loadCMakePresetForEdit(preset_source,
      tuiide::CMakePresetKind::Configure, "user", editable_user, session_error)
      && editable_user.user_editable && editable_user.binary_directory == "relative-build",
    "user preset is loaded with unexpanded editable values");
  tuiide::CMakePresetEdit scratch;
  scratch.kind = tuiide::CMakePresetKind::Configure;
  scratch.name = "scratch"; scratch.display_name = "Scratch";
  scratch.inherits = {"dev"}; scratch.binary_directory = "${sourceDir}/scratch";
  expect(tuiide::saveCMakeUserPreset(preset_source, {}, scratch, session_error),
    "new user configure preset is saved");
  tuiide::CMakePresetEdit scratch_build;
  scratch_build.kind = tuiide::CMakePresetKind::Build;
  scratch_build.name = "scratch-build"; scratch_build.configure_preset = "scratch";
  scratch_build.targets = {"app"};
  expect(tuiide::saveCMakeUserPreset(preset_source, {}, scratch_build, session_error),
    "new user build preset is saved");
  expect(!tuiide::deleteCMakeUserPreset(preset_source,
      tuiide::CMakePresetKind::Configure, "scratch", session_error)
      && session_error.find("references unknown configure preset") != std::string::npos,
    "configure preset deletion is rejected while a build preset references it");
  scratch.name = "scratch-renamed";
  expect(!tuiide::saveCMakeUserPreset(preset_source, "scratch", scratch, session_error),
    "configure preset rename is rejected while references still use its old name");
  expect(tuiide::deleteCMakeUserPreset(preset_source,
      tuiide::CMakePresetKind::Build, "scratch-build", session_error)
      && tuiide::saveCMakeUserPreset(preset_source, "scratch", scratch, session_error)
      && tuiide::deleteCMakeUserPreset(preset_source,
        tuiide::CMakePresetKind::Configure, "scratch-renamed", session_error),
    "unreferenced user presets can be renamed and deleted");
  expect(!tuiide::deleteCMakeUserPreset(preset_source,
      tuiide::CMakePresetKind::Configure, "dev", session_error)
      && session_error.find("read-only") != std::string::npos,
    "project presets cannot be deleted through the user preset store");
  expect(tuiide::cloneCMakePresetToUser(preset_source,
      tuiide::CMakePresetKind::Configure, "base", "base-copy", session_error),
    "an included preset can be cloned into the user file");
  {
    std::ifstream user_file(preset_source / "CMakeUserPresets.json");
    const auto user_json = nlohmann::json::parse(user_file, nullptr, false);
    const auto clone = std::find_if(user_json["configurePresets"].begin(),
      user_json["configurePresets"].end(), [](const auto& item) {
        return item.value("name", std::string{}) == "base-copy";
      });
    expect(clone != user_json["configurePresets"].end()
        && clone->contains("condition") && clone->value("hidden", false),
      "cloning preserves unknown and non-editor preset fields");
  }
  expect(tuiide::deleteCMakeUserPreset(preset_source,
      tuiide::CMakePresetKind::Configure, "base-copy", session_error),
    "cloned user preset can be removed");

  tuiide::CMakeSession cmake_session;
  const auto default_build = preset_source / "default-build";
  cmake_session.reset(default_build);
  cmake_session.restoreSelection("demo", "Release", "dev", "dev-build");
  const auto preset_refresh = cmake_session.refreshPresets(preset_source, default_build);
  expect(preset_refresh.valid && preset_refresh.configure_error.empty()
      && preset_refresh.build_error.empty() && preset_refresh.validation_error.empty(),
    "CMake session validates restored configure and build presets");
  expect(cmake_session.buildDirectory() == preset_source / "out/dev",
    "CMake session derives the active build directory from the configure preset");
  cmake_session.replaceTargets({{"helper", "Debug", default_build / "helper"},
    {"demo", "Release", default_build / "demo"}});
  expect(cmake_session.selectedTarget() && cmake_session.selectedTarget()->name == "demo"
      && cmake_session.selectedTarget()->configuration == "Release",
    "CMake session restores an executable target by name and configuration");
  expect(!cmake_session.selectTarget(2), "CMake session rejects an out-of-range target selection");
  expect(cmake_session.selectConfigurePreset("user", default_build)
      && cmake_session.configurePreset() == "user" && cmake_session.buildPreset().empty()
      && cmake_session.buildDirectory() == preset_source / "relative-build"
      && cmake_session.targets().empty() && cmake_session.preferredTarget().empty(),
    "changing configure preset clears incompatible build and target selections");
  expect(cmake_session.selectBuildPreset("user-build", default_build)
      && cmake_session.configurePreset() == "user"
      && cmake_session.buildPreset() == "user-build",
    "selecting a build preset activates its configure preset");
  expect(!cmake_session.selectConfigurePreset("missing", default_build)
      && !cmake_session.selectBuildPreset("missing", default_build),
    "CMake session rejects selections absent from refreshed presets");
  cmake_session.restoreSelection({}, {}, "missing", {});
  const auto invalid_preset_refresh = cmake_session.refreshPresets(preset_source, default_build);
  expect(!invalid_preset_refresh.valid
      && invalid_preset_refresh.validation_error.find("no longer exists") != std::string::npos,
    "CMake session reports a removed restored preset without changing project settings");
  cmake_session.reset();
  expect(cmake_session.buildDirectory().empty() && cmake_session.targets().empty()
      && cmake_session.configurePresets().empty() && cmake_session.buildPresets().empty()
      && cmake_session.configurePreset().empty() && cmake_session.buildPreset().empty()
      && cmake_session.preferredTarget().empty() && cmake_session.preferredConfiguration().empty()
      && cmake_session.selectedTarget() == nullptr,
    "resetting a CMake session removes targets, presets, selections, and build-directory identity");

  tuiide::ProjectSettings command_settings;
  command_settings.generator = "Ninja";
  command_settings.toolchain = "/opt/toolchains/test.cmake";
  command_settings.make_program = "/usr/bin/ninja";
  command_settings.sysroot = "/opt/sdk";
  command_settings.c_compiler = "/usr/bin/clang";
  command_settings.cpp_compiler = "/usr/bin/clang++";
  command_settings.c_standard = "17";
  command_settings.cpp_standard = "20";
  command_settings.build_type = "RelWithDebInfo";
  const auto command_project = preset_source / "project with spaces";
  const auto command_build = preset_source / "build with spaces";
  auto configure_command = tuiide::BuildCommandService::configure(command_project,
    command_build, command_settings, {}, presets, session_error);
  expect(configure_command && session_error.empty()
      && configure_command->arguments == std::vector<std::string>{
        "cmake", "-S", command_project.string(), "-B", command_build.string(),
        "-G", "Ninja", "-DCMAKE_TOOLCHAIN_FILE=/opt/toolchains/test.cmake",
        "-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja", "-DCMAKE_SYSROOT=/opt/sdk",
        "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
        "-DCMAKE_C_STANDARD=17", "-DCMAKE_CXX_STANDARD=20",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},
    "configure command applies project toolchain, compiler, standard, and build settings without shell splitting");
  command_settings = {};
  configure_command = tuiide::BuildCommandService::configure(command_project,
    command_build, command_settings, "dev", presets, session_error);
  expect(configure_command
      && std::find(configure_command->arguments.begin(), configure_command->arguments.end(), "-B")
        == configure_command->arguments.end()
      && configure_command->arguments.back() == "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "configure preset with binaryDir remains authoritative and exports compile commands");
  configure_command = tuiide::BuildCommandService::configure(command_project,
    command_build, command_settings, "missing", presets, session_error);
  expect(!configure_command && session_error.find("no longer exists") != std::string::npos,
    "removed configure preset produces an actionable command error");

  const tuiide::CMakeTarget command_target{"demo", "Release", command_build / "demo"};
  auto build_command = tuiide::BuildCommandService::build(command_project,
    command_build, 7, {}, &command_target, false);
  expect(build_command.arguments == std::vector<std::string>{
      "cmake", "--build", command_build.string(), "--parallel", "7",
      "--target", "demo", "--config", "Release"}
      && build_command.working_directory.empty(),
    "build command applies the configured parallelism, target, and configuration");
  build_command = tuiide::BuildCommandService::build(command_project,
    command_build, 0, "dev-build", nullptr, true);
  expect(build_command.arguments == std::vector<std::string>{
      "cmake", "--build", "--preset", "dev-build", "--parallel", "1", "--target", "clean"}
      && build_command.working_directory == command_project,
    "preset clean command uses the project working directory and defensively clamps parallelism");

  const std::vector<std::filesystem::path> analysis_sources{
    command_project / "src/main.cpp", command_project / "src/worker.cpp"};
  auto analysis_command = tuiide::makeAnalysisCommand(tuiide::AnalysisTool::ClangTidy,
    tuiide::AnalysisScope::Project,
    "/usr/bin/clang-tidy", command_project, command_build, analysis_sources);
  expect(analysis_command.arguments == std::vector<std::string>{"/usr/bin/clang-tidy",
      analysis_sources[0].string(), analysis_sources[1].string(),
      "-p=" + command_build.string(), "--quiet"}
      && analysis_command.working_directory == command_project
      && analysis_command.display.find("project with spaces") != std::string::npos,
    "clang-tidy analysis command preserves source paths and compilation database");
  analysis_command = tuiide::makeAnalysisCommand(tuiide::AnalysisTool::Cppcheck,
    tuiide::AnalysisScope::File,
    "/usr/bin/cppcheck", command_project, command_build, {analysis_sources.front()});
  expect(analysis_command.arguments.front() == "/usr/bin/cppcheck"
      && std::ranges::find(analysis_command.arguments,
        "--template={file}:{line}:{column}: {severity}: {message}") != analysis_command.arguments.end()
      && analysis_command.arguments.back() == analysis_sources.front().string(),
    "cppcheck command emits compiler-compatible diagnostics for Problems");
  analysis_command = tuiide::makeAnalysisCommand(tuiide::AnalysisTool::Cppcheck,
    tuiide::AnalysisScope::Project, "/usr/bin/cppcheck", command_project,
    command_build, analysis_sources);
  expect(analysis_command.arguments.back()
      == "--project=" + (command_build / "compile_commands.json").string(),
    "project cppcheck consumes the compilation database instead of losing compiler flags");
  analysis_command = tuiide::makeAnalysisCommand(tuiide::AnalysisTool::IncludeWhatYouUse,
    tuiide::AnalysisScope::File,
    "/usr/bin/iwyu_tool.py", command_project, command_build, {analysis_sources.front()});
  expect(analysis_command.arguments == std::vector<std::string>{"/usr/bin/iwyu_tool.py",
      "-p", command_build.string(), analysis_sources.front().string()},
    "IWYU tool command uses compile_commands without shell interpolation");
  const auto sanitizer_directory = command_build / ".tuiide-sanitizers";
  const auto sanitizer_configure = tuiide::makeSanitizerConfigureCommand(
    "/usr/bin/cmake", command_project, sanitizer_directory);
  expect(std::ranges::find(sanitizer_configure.arguments,
      "-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer")
        != sanitizer_configure.arguments.end(),
    "sanitizer configure enables ASan and UBSan in a separate build directory");
  const auto sanitizer_build = tuiide::makeSanitizerBuildCommand(
    "/usr/bin/cmake", sanitizer_directory, 0, "demo");
  expect(sanitizer_build.arguments == std::vector<std::string>{"/usr/bin/cmake", "--build",
      sanitizer_directory.string(), "--parallel", "1", "--target", "demo"},
    "sanitizer target build clamps invalid parallelism and keeps target separate");
  const auto sanitizer_run = tuiide::makeSanitizerRunCommand(
    sanitizer_directory / "demo", {"--sample", "value with spaces"}, command_project);
  expect(sanitizer_run.arguments == std::vector<std::string>{
      (sanitizer_directory / "demo").string(), "--sample", "value with spaces"}
      && sanitizer_run.working_directory == command_project,
    "sanitizer executable preserves configured arguments and working directory");
  std::filesystem::remove_all(preset_source, cleanup_error);

  std::vector<tuiide::CTestCase> ctest_cases;
  std::string ctest_error;
  const auto ctest_json = R"({"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[
    {"name":"unit.alpha","command":["/tmp/unit","--alpha"],"properties":[
      {"name":"LABELS","value":["unit","fast"]}]},
    {"name":"unit.disabled","command":null,"properties":[
      {"name":"DISABLED","value":true},{"name":"LABELS","value":null}]},
    {"name":null,"properties":"invalid"}]})";
  expect(tuiide::parseCTestDiscovery(ctest_json, ctest_cases, ctest_error)
      && ctest_error.empty() && ctest_cases.size() == 2
      && ctest_cases[0].name == "unit.alpha"
      && ctest_cases[0].command == std::vector<std::string>{"/tmp/unit", "--alpha"}
      && ctest_cases[0].labels == std::vector<std::string>{"unit", "fast"}
      && ctest_cases[1].status == tuiide::CTestStatus::Disabled,
    "CTest JSON discovery tolerates null and malformed optional fields");
  expect(!tuiide::parseCTestDiscovery(R"({"tests":null})", ctest_cases, ctest_error)
      && ctest_error.find("tests array") != std::string::npos,
    "CTest JSON discovery reports a missing tests array without throwing");

  const auto ctest_directory = std::filesystem::temp_directory_path()
    / ("tuiide-ctest-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(ctest_directory / "src");
  { std::ofstream file(ctest_directory / "CMakePresets.json"); file << R"({
    "version": 6, "include": ["presets/extra.json"],
    "testPresets": [{"name":"default-tests","displayName":"Default tests"},
                    {"name":"hidden-tests","hidden":true}]})"; }
  std::filesystem::create_directories(ctest_directory / "presets");
  { std::ofstream file(ctest_directory / "presets/extra.json"); file << R"({
    "version": 6, "testPresets": [{"name":"integration"}]})"; }
  { std::ofstream file(ctest_directory / "CMakeUserPresets.json"); file << R"({
    "version": 6, "testPresets": [{"name":"local"},{"name":"integration"}]})"; }
  const auto test_presets = tuiide::loadCTestPresets(ctest_directory, ctest_error);
  expect(ctest_error.empty() && test_presets.size() == 3
      && test_presets[0].name == "default-tests"
      && test_presets[1].name == "integration"
      && test_presets[2].name == "local",
    "CTest presets load includes, user presets, display names, hidden flags, and deduplicate names");
  const auto locations = tuiide::parseCTestLocations(
    "src/widget_test.cpp:42: failure\n./src/widget_test.cpp(42): duplicate\n", ctest_directory);
  expect(locations.size() == 1 && locations[0].path == ctest_directory / "src/widget_test.cpp"
      && locations[0].line == 41,
    "CTest failure parser normalizes and deduplicates GCC and parenthesized source locations");
  std::filesystem::remove_all(ctest_directory, cleanup_error);

  const std::vector<std::uint32_t> semantic_data{
    0, 2, 3, 0, 1,
    0, 5, 2, 1, 0,
    2, 1, 4, 0, 0
  };
  const auto semantic = tuiide::decodeSemanticTokens("/tmp/source.cpp", semantic_data, {"function", "parameter"});
  expect(semantic.size() == 3, "semantic token stream is decoded");
  expect(semantic[0].line == 0 && semantic[0].column == 2 && semantic[0].type == "function", "first semantic token is absolute");
  expect(semantic[1].line == 0 && semantic[1].column == 7 && semantic[1].type == "parameter", "same-line semantic delta is accumulated");
  expect(semantic[2].line == 2 && semantic[2].column == 1, "new-line semantic delta resets the column");
  std::cout << "All core tests passed\n";
}
