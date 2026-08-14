#include "tuiide/command_state.hpp"

namespace tuiide {

auto commandAvailability(const CommandContext& context) -> CommandAvailability {
  const bool lsp_document = context.has_document && context.source_document && context.lsp_ready;
  const bool idle_project = context.has_project && !context.build_running;
  const bool debugger_stopped = context.gdb_running && context.gdb_active && context.gdb_stopped;
  return {
    .close_project = context.has_project,
    .project_file = context.has_project,
    .save = context.has_document,
    .save_all = context.has_modified_documents,
    .save_as = context.has_document,
    .close_document = context.has_document,
    .close_all = context.has_document,
    .close_others = context.document_count > 1,
    .reopen_closed = context.has_closed_document,
    .undo = context.has_document && context.can_undo,
    .redo = context.has_document && context.can_redo,
    .cut = context.has_document && context.has_selection,
    .copy = context.has_document && context.has_selection,
    .paste = context.has_document,
    .select_all = context.has_document,
    .find = context.has_document,
    .go_to_line = context.has_document,
    .definition = lsp_document,
    .references = lsp_document,
    .problems = context.has_project,
    .build = idle_project,
    .cancel_build = context.has_project && context.build_running,
    .run = idle_project && !context.run_running,
    .stop_run = context.run_running,
    .cmake_configuration = idle_project,
    .debug_start = idle_project && (!context.gdb_running || !context.gdb_active || context.gdb_stopped),
    .debug_pause = context.gdb_running && context.gdb_active && !context.gdb_stopped,
    .debug_stop = context.gdb_running,
    .debug_restart = idle_project && context.gdb_running,
    .breakpoint = context.has_saved_document && context.source_document,
    .debug_step = debugger_stopped,
    .watch = context.has_project,
    .registers = context.has_project,
    .completion = context.has_document
      && (context.cmake_document || (context.source_document && context.lsp_ready)),
    .signature_help = lsp_document,
    .hover = lsp_document,
    .rename = lsp_document,
    .code_actions = lsp_document,
    .workspace_symbols = context.has_project && context.lsp_ready,
    .hierarchy = lsp_document,
    .format_document = context.has_document && context.source_document,
    .format_selection = context.has_document && context.source_document && context.has_selection,
    .switch_document = context.document_count > 1,
    .project_panel = context.has_project,
    .debug_panel = context.has_project,
  };
}

}  // namespace tuiide
